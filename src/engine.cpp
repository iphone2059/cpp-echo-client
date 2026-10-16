//! cec.engine — converted from a header/implementation pair into a single C++20 named module.

module;

// clang-format off
// The SDK headers are order sensitive: WinSock2 before MSWSock and WS2tcpip, then
// Windows.h and mmsystem.h. clang-format must not reorder this block.
#include <WinSock2.h>
#include <MSWSock.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <mmsystem.h>
// clang-format on

#include <algorithm>
#include <array>
#include <limits>
#include <new>

export module cec.engine;

export import cec.internal;
export import cec.native;
import cec.payload;
export import cec.types;

// ---- declarations (the former engine.h) ----

// Concrete Transport owns native storage and exposes the same operations as a test adapter.
export class cec_rio_transport {
  public:
    cec_native_worker native{};
    cec_rio_transport() noexcept = default;
    ~cec_rio_transport() noexcept;
    cec_rio_transport(const cec_rio_transport&)            = delete;
    cec_rio_transport& operator=(const cec_rio_transport&) = delete;
    cec_rio_transport(cec_rio_transport&&)                 = delete;
    cec_rio_transport& operator=(cec_rio_transport&&)      = delete;
    bool               connect(std::uint32_t index) noexcept;
    bool               receive(std::uint32_t index, std::size_t bytes) noexcept;
    bool               send(std::uint32_t index, std::size_t bytes) noexcept;
    bool               close(std::uint32_t index) noexcept;
    bool               drained(std::uint32_t index) const noexcept;
    bool               closing(std::uint32_t index) const noexcept;
    bool               continue_send(std::uint32_t index) noexcept;
    [[noreturn]] void  invariant(const wchar_t* stage) const noexcept;
    [[noreturn]] void  drain_timeout() const noexcept;
    cec_completion     retire_rio(const RIORESULT& result) noexcept;
    cec_completion     retire_connect(std::uint32_t index, BOOL ok, DWORD error, DWORD bytes) noexcept;

  private:
    cec_native_slot* require_slot(std::uint32_t index) const noexcept;
};

export class cec_native_clock {
  public:
    std::uint64_t now() const noexcept { return cec_engine_now(); }
};

// Compile-time transport/clock substitution: the native production path has no virtual calls.
export template <class Transport, class Clock>
class cec_worker_runtime {
  public:
    cec_worker_runtime(cec_worker_core& core, Transport& transport, Clock& clock) noexcept :
        core_(core),
        transport_(transport),
        clock_(clock) {}

    void event(cec_event value) noexcept { execute(cec_core_on_event(&core_, value, clock_.now())); }

    void execute(cec_action_batch actions) noexcept {
        require_core();
        if (core_.failed) {
            return;
        }
        if (actions.count > actions.actions.size()) {
            transport_.invariant(L"client action batch extent");
            return;
        }
        for (std::uint32_t position = 0; position < actions.count; ++position) {
            const cec_action action = actions.actions[position];
            if (action.index >= core_.session_count) {
                transport_.invariant(L"client action session index");
                return;
            }
            const cec_engine_state state   = core_.sessions[action.index].state;
            bool                   success = true;
            switch (action.kind) {
                case cec_action_kind::connect:
                    if (state != cec_engine_state::connecting) {
                        break;
                    }
                    success = transport_.connect(action.index);
                    if (success) {
                        cec_core_anchor_attempt(&core_, action.index, clock_.now());
                    }
                    if (success && core_.options->protocol == cec_protocol::udp) {
                        event({ cec_event_kind::connected, action.index });
                    }
                    break;
                case cec_action_kind::close:
                    {
                        if (state != cec_engine_state::closing) {
                            break;
                        }
                        const bool first_close = !transport_.closing(action.index);
                        const bool drained     = transport_.close(action.index);
                        if (first_close) {
                            cec_core_anchor_close(&core_, action.index, clock_.now());
                        }
                        if (drained) {
                            event({ cec_event_kind::generation_drained, action.index });
                        }
                        break;
                    }
                case cec_action_kind::drain_timeout:
                    if (state != cec_engine_state::closing) {
                        break;
                    }
                    if (!transport_.drained(action.index)) {
                        transport_.drain_timeout();
                        return;
                    }
                    event({ cec_event_kind::generation_drained, action.index });
                    break;
                case cec_action_kind::receive:
                    if (state != cec_engine_state::active) {
                        break;
                    }
                    cec_core_anchor_attempt(&core_, action.index, clock_.now());
                    success = transport_.receive(action.index, action.bytes);
                    break;
                case cec_action_kind::send:
                    if (state != cec_engine_state::active) {
                        break;
                    }
                    success = transport_.send(action.index, action.bytes);
                    break;
            }
            if (!success) {
                event({ cec_event_kind::transport_failure, action.index });
                return;
            }
            require_core();
            if (core_.failed) {
                return;
            }
        }
    }

    void completion(cec_completion value) noexcept {
        // Native retirement has already validated and returned the provider's operation ownership.
        if (value.index >= core_.session_count) {
            transport_.invariant(L"client completion session index");
            return;
        }
        switch (value.kind) {
            case cec_completion_kind::connected:
                event({ cec_event_kind::connected, value.index });
                break;
            case cec_completion_kind::sent:
            case cec_completion_kind::send_progress:
                core_.metrics->sent_bytes.fetch_add(value.bytes, std::memory_order_relaxed);
                if (value.kind == cec_completion_kind::sent) {
                    event({ cec_event_kind::sent, value.index });
                } else if (!transport_.continue_send(value.index)) {
                    event({ cec_event_kind::transport_failure, value.index });
                }
                break;
            case cec_completion_kind::received:
                event({ cec_event_kind::received, value.index, value.bytes, value.equal });
                break;
            case cec_completion_kind::failed:
                event({ cec_event_kind::transport_failure, value.index });
                break;
            case cec_completion_kind::drained:
                event({ cec_event_kind::generation_drained, value.index });
                break;
            case cec_completion_kind::ignored:
                break;
        }
    }

    void stop(bool fatal) noexcept {
        if (core_.stopping) {
            return;
        }
        core_.stopping = true;
        for (std::uint32_t index = 0; index < core_.session_count; ++index) {
            execute(cec_core_stop_session(&core_, index, clock_.now(), fatal));
        }
    }

    template <class StopProbe>
    void poll_after_stop_observation(StopProbe& stop_probe) noexcept {
        poll_at_after_stop_observation(stop_probe, clock_.now());
    }

    template <class StopProbe>
    void poll_at_after_stop_observation(StopProbe& stop_probe, std::uint64_t decision_now) noexcept {
        if (stop_probe.requested()) {
            stop(stop_probe.fatal());
        }
        while (core_.timers.size != 0 && core_.timers.nodes[0].deadline <= decision_now) {
            execute(cec_core_poll(&core_, decision_now));
        }
    }

    template <class StopProbe, class Drain>
    void drain_before_timers(StopProbe& stop_probe, Drain& drain, std::uint64_t budget) noexcept {
        const std::uint64_t decision_now = clock_.now();
        drain(budget);
        // Acquired completions are accounted first; a stop that arrived inside drain wins over timeout.
        // New posts created by that drain cannot expire inside the same bounded decision.
        poll_at_after_stop_observation(stop_probe, decision_now);
    }

  private:
    // Native transports terminate inside their invariant boundary while a recording test
    // transport returns so a unit test can observe the rejection. Reporting first and letting
    // the caller test the recorded flag keeps both instantiations free of unreachable code and
    // of a missing return.
    void require_core() noexcept {
        if (core_.failed) {
            transport_.invariant(core_.failure_stage == nullptr ? L"client pure engine invariant" :
                                                                  core_.failure_stage);
        }
    }

    cec_worker_core& core_;
    Transport&       transport_;
    Clock&           clock_;
};

export struct cec_engine_worker {
    cec_worker_core                              core{};
    cec_rio_transport                            transport{};
    // The worker alone drains its CQ. Provider results overwrite only the returned prefix.
    std::array<RIORESULT, cec_engine_batch_size> completion_results{};
    std::atomic<bool>*                           external_stop = nullptr;
    std::atomic<bool>*                           fatal         = nullptr;
};

export class cec_atomic_stop_probe {
  public:
    explicit cec_atomic_stop_probe(cec_engine_worker& worker) noexcept : worker_(worker) {}

    bool requested() const noexcept { return worker_.external_stop->load(std::memory_order_acquire); }

    bool fatal() const noexcept { return worker_.fatal->load(std::memory_order_acquire); }

  private:
    cec_engine_worker& worker_;
};

export cec_exit_code cec_run_client(const cec_options* options, std::atomic<bool>* stop_requested) noexcept;

// ---- definitions (the former engine.cpp) ----

cec_rio_transport::~cec_rio_transport() noexcept {
    cec_native_require_release(&native);
    // The embedded notification OVERLAPPED cannot disappear while its CQ or IOCP still exists.
    if (native.port != nullptr || native.completion_queue != RIO_INVALID_CQ) {
        cec_engine_fail_fast(L"client transport resource release precondition", ERROR_IO_INCOMPLETE);
    }
}

cec_native_slot* cec_rio_transport::require_slot(std::uint32_t index) const noexcept {
    if (native.slots == nullptr || index >= native.session_count) {
        cec_engine_fail_fast(L"client transport slot index", ERROR_INVALID_DATA);
    }
    if (native.slots[index].index != index || native.slots[index].owner != &native) {
        cec_engine_fail_fast(L"client transport slot identity", ERROR_INVALID_DATA);
    }
    return &native.slots[index];
}

bool cec_rio_transport::connect(std::uint32_t index) noexcept {
    return cec_native_connect_slot(require_slot(index));
}

bool cec_rio_transport::receive(std::uint32_t index, std::size_t bytes) noexcept {
    return cec_engine_post_receive(require_slot(index), bytes);
}

bool cec_rio_transport::send(std::uint32_t index, std::size_t bytes) noexcept {
    cec_native_slot* slot = require_slot(index);
    if (bytes != slot->attempt_bytes) {
        cec_engine_fail_fast(L"client send action extent", ERROR_INVALID_DATA);
    }
    return cec_engine_post_send(slot);
}

bool cec_rio_transport::close(std::uint32_t index) noexcept {
    cec_native_close_slot(require_slot(index));
    return drained(index);
}

bool cec_rio_transport::drained(std::uint32_t index) const noexcept {
    return cec_native_slot_drained(require_slot(index));
}

cec_completion cec_rio_transport::retire_rio(const RIORESULT& result) noexcept {
    return cec_native_retire_rio(&native, result);
}

cec_completion cec_rio_transport::retire_connect(std::uint32_t index, BOOL ok, DWORD error, DWORD bytes) noexcept {
    return cec_native_retire_connect(require_slot(index), ok, error, bytes);
}

bool cec_rio_transport::closing(std::uint32_t index) const noexcept {
    return require_slot(index)->closing;
}

bool cec_rio_transport::continue_send(std::uint32_t index) noexcept {
    return cec_engine_post_send(require_slot(index));
}

[[noreturn]] void cec_rio_transport::invariant(const wchar_t* stage) const noexcept {
    cec_engine_fail_fast(stage, ERROR_INVALID_DATA);
}

[[noreturn]] void cec_rio_transport::drain_timeout() const noexcept {
    cec_engine_fail_fast(L"client generation drain", ERROR_TIMEOUT);
}

static void cec_runtime_execute(cec_engine_worker* worker, cec_action_batch actions) noexcept {
    cec_native_clock                                        clock{};
    cec_worker_runtime<cec_rio_transport, cec_native_clock> runtime{ worker->core, worker->transport, clock };
    runtime.execute(actions);
}

static void cec_runtime_completion(cec_engine_worker* worker, cec_completion completion) noexcept {
    cec_native_clock                                        clock{};
    cec_worker_runtime<cec_rio_transport, cec_native_clock> runtime{ worker->core, worker->transport, clock };
    runtime.completion(completion);
}

static void cec_runtime_drain(cec_engine_worker* worker,
                              std::uint64_t      maximum_batches = cec_engine_max_drain_batches,
                              std::uint64_t      maximum_results = UINT64_MAX) noexcept {
    auto& results = worker->completion_results;
    for (std::uint64_t batch = 0; batch < maximum_batches && maximum_results != 0; ++batch) {
        const ULONG requested = static_cast<ULONG>(std::min<std::uint64_t>(results.size(), maximum_results));
        const ULONG count =
            cec_require_valid_dequeue_count(worker->transport.native.rio->RIODequeueCompletion(
                                                worker->transport.native.completion_queue, results.data(), requested),
                                            L"RIODequeueCompletion(client)");
        if (count > requested) {
            cec_engine_fail_fast(L"client RIO dequeue extent", ERROR_INVALID_DATA);
        }
        if (count == 0) {
            return;
        }
        maximum_results -= count;
        for (ULONG index = 0; index < count; ++index) {
            cec_runtime_completion(worker, worker->transport.retire_rio(results[index]));
        }
    }
}

static void cec_runtime_stop(cec_engine_worker* worker) noexcept {
    cec_native_clock                                        clock{};
    cec_worker_runtime<cec_rio_transport, cec_native_clock> runtime{ worker->core, worker->transport, clock };
    runtime.stop(worker->fatal->load(std::memory_order_acquire));
}

static void cec_runtime_poll(cec_engine_worker* worker) noexcept {
    cec_native_clock                                        clock{};
    cec_atomic_stop_probe                                   stop_probe{ *worker };
    cec_worker_runtime<cec_rio_transport, cec_native_clock> runtime{ worker->core, worker->transport, clock };
    runtime.poll_after_stop_observation(stop_probe);
}

class cec_runtime_cq_drain {
  public:
    explicit cec_runtime_cq_drain(cec_engine_worker* worker) noexcept : worker_(worker) {}

    void operator()(std::uint64_t budget) noexcept {
        cec_runtime_drain(worker_, budget, budget);
        cec_engine_maybe_arm(&worker_->transport.native);
    }

  private:
    cec_engine_worker* worker_;
};

static void cec_runtime_drain_before_timers(cec_engine_worker* worker, std::uint64_t budget) noexcept {
    cec_native_clock                                        clock{};
    cec_atomic_stop_probe                                   stop_probe{ *worker };
    cec_runtime_cq_drain                                    drain{ worker };
    cec_worker_runtime<cec_rio_transport, cec_native_clock> runtime{ worker->core, worker->transport, clock };
    runtime.drain_before_timers(stop_probe, drain, budget);
}

static DWORD WINAPI cec_engine_worker_thread(void* parameter) noexcept {
    cec_engine_worker* worker = static_cast<cec_engine_worker*>(parameter);
    cec_engine_maybe_arm(&worker->transport.native);
    for (std::uint32_t index = 0; index < worker->core.session_count; ++index) {
        cec_runtime_execute(worker, cec_core_start(&worker->core, index, cec_engine_now()));
    }
    std::uint32_t expired_timer_packets = 0;
    while (worker->core.live_sessions != 0) {
        DWORD       transferred       = 0;
        ULONG_PTR   key               = 0;
        OVERLAPPED* overlapped        = nullptr;
        const DWORD wait_milliseconds = cec_timer_wait_milliseconds(&worker->core.timers, cec_engine_now());
        const BOOL  ok    = GetQueuedCompletionStatus(worker->transport.native.port, &transferred, &key, &overlapped,
                                                      wait_milliseconds);
        const DWORD error = ok == FALSE ? GetLastError() : ERROR_SUCCESS;
        if (overlapped == &worker->transport.native.notification_overlapped) {
            if (ok == FALSE) {
                cec_engine_fail_fast(L"GetQueuedCompletionStatus(client notification)", static_cast<int>(error));
            }
            if (!cec_notification_packet_matches(
                    key, overlapped,
                    static_cast<ULONG_PTR>(reinterpret_cast<std::uintptr_t>(&worker->transport.native)),
                    &worker->transport.native.notification_overlapped)) {
                cec_engine_fail_fast(L"client RIO notification key", ERROR_INVALID_DATA);
            }
            if (!cec_notification_mark_delivered(&worker->transport.native.notification_armed)) {
                cec_engine_fail_fast(L"client notification delivery transition", ERROR_INVALID_STATE);
            }
            ++worker->transport.native.notify_deliveries;
            cec_runtime_drain(worker);
            // A bounded drain may leave results queued; arming an already nonempty CQ notifies
            // immediately, so the rearm only has to happen while work is still outstanding.
            cec_engine_maybe_arm(&worker->transport.native);
        } else if (overlapped == nullptr && key == cec_engine_stop_key) {
            cec_runtime_stop(worker);
        } else if (overlapped != nullptr && key > cec_engine_stop_key) {
            const std::uintptr_t candidate   = static_cast<std::uintptr_t>(key);
            const std::uintptr_t first       = reinterpret_cast<std::uintptr_t>(worker->transport.native.slots);
            const std::size_t    table_bytes = sizeof(cec_native_slot) * worker->transport.native.session_count;
            if (candidate < first || candidate - first >= table_bytes ||
                (candidate - first) % sizeof(cec_native_slot) != 0) {
                cec_engine_fail_fast(L"unexpected ConnectEx completion key", ERROR_INVALID_DATA);
            }
            cec_native_slot* session = reinterpret_cast<cec_native_slot*>(candidate);
            if (overlapped != &session->connect_overlapped || session->owner != &worker->transport.native ||
                session->index != (candidate - first) / sizeof(cec_native_slot)) {
                cec_engine_fail_fast(L"unexpected ConnectEx completion identity", ERROR_INVALID_DATA);
            } else {
                cec_runtime_completion(worker,
                                       worker->transport.retire_connect(session->index, ok, error, transferred));
            }
        } else if (ok == FALSE && error == WAIT_TIMEOUT && overlapped == nullptr) {
            if (!worker->core.stopping && worker->transport.native.rio_outstanding != 0 &&
                !worker->transport.native.notification_armed) {
                ++worker->transport.native.notify_timeout_wakeups;
            }
        } else if (ok == FALSE && error != WAIT_TIMEOUT) {
            cec_engine_fail_fast(L"GetQueuedCompletionStatus(client)", static_cast<int>(error));
        } else if (!(ok == FALSE && error == WAIT_TIMEOUT && overlapped == nullptr)) {
            cec_engine_fail_fast(L"unexpected client IOCP packet", ERROR_INVALID_DATA);
        }
        // Settle the acquired packet first, then arbitrate controlled stop before timers.
        if (worker->external_stop->load(std::memory_order_acquire)) {
            cec_runtime_stop(worker);
        }
        const ULONGLONG now = cec_engine_now();
        if (cec_timer_wait_milliseconds(&worker->core.timers, now) == 0) {
            // A due timer must not reject work whose completion is already queued behind this
            // packet. The next waits are zero, and the fixed session/control budget prevents
            // live traffic from postponing timers without a bound.
            const bool queued_packet = ok != FALSE || overlapped != nullptr || key != 0;
            if (queued_packet && expired_timer_packets < worker->transport.native.session_count + 2U) {
                ++expired_timer_packets;
                continue;
            }
            // CQ results can precede their IOCP notification. A nonempty dequeue may contain
            // only one result, so cover every receive/send owned at entry before the timer.
            const std::uint64_t result_budget =
                static_cast<std::uint64_t>(worker->transport.native.session_count) * 2ULL;
            cec_runtime_drain_before_timers(worker, result_budget);
            expired_timer_packets = 0;
            continue;
        }
        expired_timer_packets = 0;
        cec_runtime_poll(worker);
    }
    // Teardown contract: the loop only returns once every session has retired all of its RIO work,
    // so the completion queue can be closed without waiting for the last RIONotify delivery; the
    // notification OVERLAPPED stays valid until the IOCP handle is closed in
    // cec_engine_worker_destroy, which is the last object that can reference it. Forging an IOCP
    // packet here would claim a notification that RIO never delivered.
    // The coordinator joins every worker before checking their release invariants.
    return worker->fatal->load(std::memory_order_acquire) ? 1U : 0U;
}

static bool cec_engine_worker_initialize(cec_engine_worker*                  worker,
                                         const RIO_EXTENSION_FUNCTION_TABLE* rio,
                                         LPFN_CONNECTEX                      connect_ex,
                                         const cec_options*                  options,
                                         const SOCKADDR_IN*                  remote,
                                         const std::byte*                    pattern,
                                         std::size_t                         pattern_bytes,
                                         std::size_t                         maximum_attempt_bytes,
                                         cec_engine_metrics*                 metrics,
                                         std::atomic<bool>*                  external_stop,
                                         std::atomic<bool>*                  fatal,
                                         std::uint32_t                       worker_index,
                                         std::uint32_t                       session_count,
                                         std::uint64_t                       memory_share,
                                         cec_engine_worker_resources*        resources) noexcept {
    resources->owner                               = &worker->transport.native;
    worker->transport.native.resources             = resources;
    worker->transport.native.completion_queue      = RIO_INVALID_CQ;
    worker->transport.native.rio                   = rio;
    worker->transport.native.connect_ex            = connect_ex;
    worker->core.options                           = options;
    worker->core.session_count                     = session_count;
    worker->transport.native.options               = options;
    worker->transport.native.remote_address        = remote;
    worker->core.pattern_bytes                     = pattern_bytes;
    worker->transport.native.maximum_attempt_bytes = maximum_attempt_bytes;
    worker->core.metrics                           = metrics;
    worker->external_stop                          = external_stop;
    worker->fatal                                  = fatal;
    worker->transport.native.worker_index          = worker_index;
    worker->transport.native.session_count         = session_count;
    worker->core.live_sessions                     = session_count;
    if (QueryPerformanceFrequency(&worker->transport.native.performance_frequency) == FALSE ||
        worker->transport.native.performance_frequency.QuadPart <= 0) {
        cec_engine_report(L"QueryPerformanceFrequency", static_cast<int>(GetLastError()));
        return false;
    }
    worker->transport.native.port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    const DWORD port_error        = worker->transport.native.port == nullptr ? GetLastError() : ERROR_SUCCESS;
    resources->port.reset(worker->transport.native.port);
    constexpr std::uint32_t operations_per_session = CEC_RIO_MAX_OUTSTANDING_RECEIVES + CEC_RIO_MAX_OUTSTANDING_SENDS;
    std::size_t             arena_bytes            = 0;
    if (worker->transport.native.port == nullptr) {
        cec_engine_report(L"CreateIoCompletionPort(client worker)", static_cast<int>(port_error));
        return false;
    }
    if (session_count > options->cq_capacity / operations_per_session ||
        !cec_checked_storage_bytes(session_count, maximum_attempt_bytes, memory_share, &arena_bytes) ||
        arena_bytes > std::numeric_limits<DWORD>::max()) {
        cec_engine_report(L"client worker IOCP/CQ/arena capacity", ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    worker->transport.native.memory =
        static_cast<char*>(VirtualAlloc(nullptr, arena_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const DWORD arena_error = worker->transport.native.memory == nullptr ? GetLastError() : ERROR_SUCCESS;
    resources->arena.reset(worker->transport.native.memory);
    worker->core.sessions = static_cast<cec_session_core*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(cec_session_core) * session_count));
    resources->sessions.reset(worker->core.sessions);
    cec_timer_node* timer_nodes =
        static_cast<cec_timer_node*>(HeapAlloc(GetProcessHeap(), 0, sizeof(cec_timer_node) * session_count));
    resources->timer_nodes.reset(timer_nodes);
    std::uint32_t* timer_positions =
        static_cast<std::uint32_t*>(HeapAlloc(GetProcessHeap(), 0, sizeof(std::uint32_t) * session_count));
    resources->timer_positions.reset(timer_positions);
    resources->session_sockets.reset(new (std::nothrow) cec_socket_owner[session_count]);
    worker->transport.native.slots = static_cast<cec_native_slot*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(cec_native_slot) * session_count));
    resources->slots.reset(worker->transport.native.slots);
    if (worker->transport.native.slots != nullptr) {
        for (std::uint32_t index = 0; index < session_count; ++index) {
            worker->transport.native.slots[index].owner         = &worker->transport.native;
            worker->transport.native.slots[index].index         = index;
            worker->transport.native.slots[index].socket        = INVALID_SOCKET;
            worker->transport.native.slots[index].request_queue = RIO_INVALID_RQ;
        }
    }
    if (worker->transport.native.memory == nullptr) {
        cec_engine_report(L"VirtualAlloc(client arena)", static_cast<int>(arena_error));
        return false;
    }
    if (worker->transport.native.slots == nullptr || worker->core.sessions == nullptr || timer_nodes == nullptr ||
        timer_positions == nullptr || !resources->session_sockets ||
        !cec_timer_initialize(&worker->core.timers, timer_nodes, timer_positions, session_count,
                              static_cast<ULONGLONG>(worker->transport.native.performance_frequency.QuadPart))) {
        cec_engine_report(L"client worker allocation", ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    resources->registrations.reset(new (std::nothrow) cec_rio_registration_owner[session_count * 2U]);
    if (!resources->registrations) {
        cec_engine_report(L"client registration owners", ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    if (!cec_register_windows(rio, worker->transport.native.memory, maximum_attempt_bytes, session_count * 2U,
                              resources->registrations.get())) {
        cec_engine_report(L"RIORegisterBuffer(client windows)", WSAGetLastError());
        return false;
    }
    RIO_NOTIFICATION_COMPLETION notification{};
    notification.Type                         = RIO_IOCP_COMPLETION;
    notification.Iocp.IocpHandle              = worker->transport.native.port;
    notification.Iocp.CompletionKey           = &worker->transport.native;
    notification.Iocp.Overlapped              = &worker->transport.native.notification_overlapped;
    worker->transport.native.completion_queue = rio->RIOCreateCompletionQueue(options->cq_capacity, &notification);
    resources->completion_queue.reset(rio, worker->transport.native.completion_queue);
    if (worker->transport.native.completion_queue == RIO_INVALID_CQ) {
        cec_engine_report(L"RIOCreateCompletionQueue(client)", WSAGetLastError());
        return false;
    }

    for (std::uint32_t index = 0; index < session_count; ++index) {
        worker->core.sessions[index]       = cec_session_core{};
        worker->core.sessions[index].index = index;
        cec_native_slot* slot              = &worker->transport.native.slots[index];
        slot->receive_request              = cec_engine_request{ slot, cec_engine_operation::receive };
        slot->send_request                 = cec_engine_request{ slot, cec_engine_operation::send };
        slot->send_buffer.BufferId         = resources->registrations[index * 2U].get();
        slot->receive_buffer.BufferId      = resources->registrations[index * 2U + 1U].get();
        char* send_data                    = worker->transport.native.memory + index * 2U * maximum_attempt_bytes;
        cec_fill_repeated_pattern(reinterpret_cast<std::byte*>(send_data), maximum_attempt_bytes, pattern,
                                  pattern_bytes);
    }
    worker->transport.native.joined = false;
    worker->transport.native.thread = CreateThread(nullptr, 0, cec_engine_worker_thread, worker, 0, nullptr);
    resources->thread.reset(worker->transport.native.thread);
    if (worker->transport.native.thread == nullptr) {
        worker->transport.native.joined = true;
        cec_engine_report(L"CreateThread(client worker)", static_cast<int>(GetLastError()));
        return false;
    }
    return true;
}

static void cec_engine_worker_join(cec_engine_worker* worker) noexcept {
    if (worker->transport.native.thread != nullptr && !worker->transport.native.joined) {
        if (WaitForSingleObject(worker->transport.native.thread, INFINITE) != WAIT_OBJECT_0) {
            cec_engine_fail_fast(L"WaitForSingleObject(client worker)", static_cast<int>(GetLastError()));
        }
        worker->transport.native.joined = true;
    }
}

static void cec_engine_worker_require_release(const cec_engine_worker* worker) noexcept {
    cec_native_require_release(&worker->transport.native);
    if (worker->transport.native.thread != nullptr) {
        const cec_worker_lifecycle lifecycle{ cec_worker_phase::stopped, worker->core.live_sessions, 0,
                                              worker->transport.native.rio_outstanding };
        if (!cec_worker_may_release(&lifecycle) || worker->core.timers.size != 0) {
            cec_engine_fail_fast(L"client worker release precondition", ERROR_INVALID_STATE);
        }
        if (worker->transport.native.notify_timeout_wakeups != 0) {
            cec_engine_fail_fast(L"client notification starvation", ERROR_INVALID_STATE);
        }
    }
}

static void cec_engine_worker_destroy(cec_engine_worker* worker) noexcept {
    cec_engine_worker_join(worker);
    cec_engine_worker_require_release(worker);
    cec_engine_worker_resources* resources = worker->transport.native.resources;
    if (resources == nullptr) {
        return;
    }
    if (worker->transport.native.slots != nullptr && resources->session_sockets) {
        for (std::uint32_t index = 0; index < worker->transport.native.session_count; ++index) {
            cec_engine_session_socket_close(&worker->transport.native.slots[index]);
        }
    }
    resources->thread.reset();
    worker->transport.native.thread = nullptr;
    resources->completion_queue.reset();
    worker->transport.native.completion_queue = RIO_INVALID_CQ;
    resources->registrations.reset();
    resources->arena.reset();
    worker->transport.native.memory = nullptr;
    resources->session_sockets.reset();
    resources->slots.reset();
    worker->transport.native.slots = nullptr;
    resources->sessions.reset();
    worker->core.sessions = nullptr;
    resources->timer_nodes.reset();
    resources->timer_positions.reset();
    worker->core.timers = {};
    resources->port.reset();
    worker->transport.native.port      = nullptr;
    // Embedded notification OVERLAPPED remains alive until the last referencing IOCP has closed.
    // Both back-pointers are non-owning, so teardown disowns the pair here: everything the resource
    // destructor would check has just been checked and released, and clearing the worker's mirror
    // leaves no stale resources pointer behind. A resources object that never reached this function
    // - an initialization that failed before teardown - keeps its owner and still runs the guard.
    resources->owner                   = nullptr;
    worker->transport.native.resources = nullptr;
}

cec_exit_code cec_run_client(const cec_options* options, std::atomic<bool>* stop_requested) noexcept {
    if (options == nullptr || stop_requested == nullptr) {
        return cec_exit_code::internal;
    }
    if (options->session_count == 0 ||
        options->echo_count > std::numeric_limits<std::uint64_t>::max() / options->session_count) {
        cec_engine_report(L"finite echo quota", ERROR_ARITHMETIC_OVERFLOW);
        return cec_exit_code::usage;
    }
    cec_engine_winsock winsock{};
    if (!winsock.start()) {
        cec_engine_report(L"WSAStartup", winsock.error);
        return cec_exit_code::network;
    }
    RIO_EXTENSION_FUNCTION_TABLE rio{};
    LPFN_CONNECTEX               connect_ex = nullptr;
    if (!cec_engine_load_extensions(&rio, &connect_ex)) {
        return cec_exit_code::network;
    }
    SOCKADDR_IN remote{};
    if (!cec_engine_resolve_ipv4(options->host, options->remote_port, &remote)) {
        return cec_exit_code::network;
    }
    std::size_t                  pattern_bytes = 0;
    std::unique_ptr<std::byte[]> pattern_owner = cec_build_pattern(options, &pattern_bytes);
    std::byte*                   pattern       = pattern_owner.get();
    if (pattern == nullptr || pattern_bytes == 0 ||
        (options->protocol == cec_protocol::udp && pattern_bytes > CEC_MAXIMUM_UDP_PAYLOAD_BYTES)) {
        cec_engine_report(L"payload pattern", ERROR_INVALID_DATA);
        return cec_exit_code::usage;
    }
    std::size_t       maximum_attempt_bytes = 0;
    const std::size_t depth                 = options->protocol == cec_protocol::tcp ? options->pipeline_depth : 1U;
    if (!cec_checked_product(pattern_bytes, depth, &maximum_attempt_bytes) ||
        maximum_attempt_bytes > CEC_MAXIMUM_TCP_BATCH_BYTES) {
        cec_engine_report(L"payload batch size", ERROR_ARITHMETIC_OVERFLOW);
        return cec_exit_code::usage;
    }
    std::size_t total_storage = 0;
    if (!cec_checked_storage_bytes(options->session_count, maximum_attempt_bytes, options->memory_bytes,
                                   &total_storage)) {
        cec_engine_report(L"registered storage /memory limit", ERROR_NOT_ENOUGH_MEMORY);
        return cec_exit_code::usage;
    }

    cec_engine_timer_resolution timer_resolution{};
    if (options->interval_milliseconds != 0 && !timer_resolution.start()) {
        cec_engine_report(L"timeBeginPeriod(client pacing)", static_cast<int>(timer_resolution.error));
        return cec_exit_code::internal;
    }

    const std::uint32_t                            worker_count = cec_resolve_worker_count(options);
    // A real C++ allocation instead of a zero-filled heap block, so every worker member is
    // constructed by its default member initialiser rather than by a memset.
    // Declaration order is the lifetime contract: the resources are declared after the workers, so
    // they are destroyed first, while every cec_native_worker they point back to is still alive for
    // the resource destructor's fallback guard.
    std::unique_ptr<cec_engine_worker[]>           workers{ new (std::nothrow) cec_engine_worker[worker_count] };
    std::unique_ptr<cec_engine_worker_resources[]> worker_resources{ new (std::nothrow)
                                                                         cec_engine_worker_resources[worker_count] };
    if (!workers || !worker_resources) {
        return cec_exit_code::network;
    }
    cec_engine_metrics metrics{};
    std::atomic<bool>  fatal{ false };
    std::uint32_t      initialized        = 0;
    std::uint32_t      prepared           = 0;
    std::uint32_t      remaining_sessions = options->session_count;
    for (; initialized < worker_count; ++initialized) {
        prepared                         = initialized + 1U;
        const std::uint32_t workers_left = worker_count - initialized;
        const std::uint32_t sessions     = (remaining_sessions + workers_left - 1U) / workers_left;
        const std::uint64_t memory_share =
            (options->memory_bytes / options->session_count) * static_cast<std::uint64_t>(sessions);
        if (!cec_engine_worker_initialize(&workers[initialized], &rio, connect_ex, options, &remote, pattern,
                                          pattern_bytes, maximum_attempt_bytes, &metrics, stop_requested, &fatal,
                                          initialized, sessions, memory_share, &worker_resources[initialized])) {
            fatal.store(true, std::memory_order_release);
            break;
        }
        remaining_sessions -= sessions;
    }

    const ULONGLONG start           = GetTickCount64();
    ULONGLONG       next_report     = options->report_seconds == 0 ?
                                          std::numeric_limits<ULONGLONG>::max() :
                                          start + static_cast<ULONGLONG>(options->report_seconds) * 1000ULL;
    bool            all_done        = false;
    bool            stop_posts_sent = false;
    while (!all_done) {
        const ULONGLONG now = GetTickCount64();
        if (options->run_seconds != 0 && now - start >= options->run_seconds * 1000ULL) {
            stop_requested->store(true, std::memory_order_release);
        }
        if (now >= next_report) {
            cec_engine_print_metrics("report", options, &metrics, now - start);
            next_report = now + static_cast<ULONGLONG>(options->report_seconds) * 1000ULL;
        }
        if (!stop_posts_sent &&
            (fatal.load(std::memory_order_acquire) || stop_requested->load(std::memory_order_acquire))) {
            for (std::uint32_t index = 0; index < initialized; ++index) {
                if (PostQueuedCompletionStatus(workers[index].transport.native.port, 0, cec_engine_stop_key, nullptr) ==
                    FALSE) {
                    cec_engine_fail_fast(L"PostQueuedCompletionStatus(client stop)", static_cast<int>(GetLastError()));
                }
            }
            stop_posts_sent = true;
        }
        all_done = true;
        for (std::uint32_t index = 0; index < initialized; ++index) {
            if (WaitForSingleObject(workers[index].transport.native.thread, 0) == WAIT_TIMEOUT) {
                all_done = false;
            }
        }
        if (!all_done) {
            Sleep(10);
        }
    }
    for (std::uint32_t index = 0; index < prepared; ++index) {
        cec_engine_worker_join(&workers[index]);
    }
    for (std::uint32_t index = 0; index < prepared; ++index) {
        cec_engine_worker_require_release(&workers[index]);
    }
    // Snapshot the notification counters while the worker threads are joined and their storage is
    // still alive; the file itself is written once, after every teardown step has completed.
    std::array<cec_notify_diagnostic_snapshot, CEC_MAX_WORKERS> snapshots{};
    for (std::uint32_t index = 0; index < initialized; ++index) {
        snapshots[index].worker          = index;
        snapshots[index].arms            = workers[index].transport.native.notify_arms;
        snapshots[index].deliveries      = workers[index].transport.native.notify_deliveries;
        snapshots[index].timeout_wakeups = workers[index].transport.native.notify_timeout_wakeups;
    }
    for (std::uint32_t index = 0; index < prepared; ++index) {
        cec_engine_worker_destroy(&workers[index]);
    }
    const std::uint64_t run_quota =
        options->echo_count == 0 ? 0 : options->echo_count * static_cast<std::uint64_t>(options->session_count);
    const std::uint64_t never_claimed = cec_unclaimed_echoes(run_quota, metrics.claimed.load(std::memory_order_relaxed),
                                                             stop_requested->load(std::memory_order_acquire));
    if (never_claimed != 0) {
        // The run intended these echoes and never posted them: they are part of the attempted total
        // and, because the run failed before reaching them, they are also part of the shortfall.
        metrics.terminal_unclaimed.fetch_add(never_claimed, std::memory_order_relaxed);
        metrics.lost.fetch_add(never_claimed, std::memory_order_relaxed);
    }
#ifndef NDEBUG
    {
        const std::uint64_t attempted = metrics.claimed.load(std::memory_order_relaxed) +
                                        metrics.terminal_unclaimed.load(std::memory_order_relaxed);
        const std::uint64_t settled =
            metrics.echoed.load(std::memory_order_relaxed) + metrics.corrupted.load(std::memory_order_relaxed) +
            metrics.lost.load(std::memory_order_relaxed) + metrics.cancelled.load(std::memory_order_relaxed);
        if (attempted != settled || metrics.active.load(std::memory_order_relaxed) != 0) {
            cec_engine_fail_fast(L"client terminal accounting", ERROR_INVALID_STATE);
        }
    }
#endif

    const std::uint64_t echoed    = metrics.echoed.load(std::memory_order_relaxed);
    const std::uint64_t corrupted = metrics.corrupted.load(std::memory_order_relaxed);
    const std::uint64_t lost      = metrics.lost.load(std::memory_order_relaxed);
    if (!options->quiet || options->stats) {
        cec_engine_print_metrics("final", options, &metrics, GetTickCount64() - start);
    }
    // Opt-in observability: a diagnostics failure must never change the production result.
    (void) cec_write_diagnostics(snapshots.data(), initialized, options->protocol);
    return cec_classify_result(echoed, corrupted, lost, metrics.network_errors.load(std::memory_order_relaxed),
                               fatal.load(std::memory_order_acquire), stop_requested->load(std::memory_order_acquire));
}
