//! cec.internal — converted from a header/implementation pair into a single C++20 named module.

module;

#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>
#include <utility>

export module cec.internal;

export import cec.metrics;
export import cec.types;

// ---- declarations (the former internal.h) ----

export struct cec_timer_node {
    // Deadlines and the matching "now" use the heap's ticks_per_second.
    std::uint64_t deadline;
    std::uint32_t session_index;
};

export struct cec_timer_heap {
    cec_timer_node* nodes;
    std::uint32_t*  positions;
    std::uint32_t   size;
    std::uint32_t   capacity;
    std::uint64_t   ticks_per_second;
};

export bool          cec_timer_initialize(cec_timer_heap* heap,
                                          cec_timer_node* nodes,
                                          std::uint32_t*  positions,
                                          std::uint32_t   capacity,
                                          std::uint64_t   ticks_per_second = 1000ULL) noexcept;
export std::uint64_t cec_timer_deadline_after_milliseconds(std::uint64_t now,
                                                           std::uint64_t milliseconds,
                                                           std::uint64_t ticks_per_second) noexcept;
export bool          cec_timer_insert_or_update(cec_timer_heap* heap,
                                                std::uint32_t   session_index,
                                                std::uint64_t   deadline) noexcept;
export bool          cec_timer_remove(cec_timer_heap* heap, std::uint32_t session_index) noexcept;
export bool cec_timer_pop_expired(cec_timer_heap* heap, std::uint64_t now, std::uint32_t* session_index) noexcept;
export std::uint32_t cec_timer_wait_milliseconds(const cec_timer_heap* heap, std::uint64_t now) noexcept;

export bool cec_worker_may_release(const cec_worker_lifecycle* lifecycle) noexcept;

export struct cec_session_core {
    cec_engine_state state                 = cec_engine_state::dormant;
    std::uint32_t    index                 = 0;
    std::uint64_t    claimed               = 0;
    std::uint64_t    settled               = 0;
    std::uint64_t    requested_echoes      = 0;
    std::size_t      attempt_bytes         = 0;
    std::uint64_t    deadline              = 0;
    std::uint64_t    next_action           = 0;
    std::uint64_t    started_at            = 0;
    bool             send_done             = false;
    bool             receive_done          = false;
    bool             receive_posted        = false;
    bool             receive_equal         = false;
    bool             attempt_accounted     = true;
    bool             active_counted        = false;
    bool             reconnect_after_close = false;
};

export struct cec_action_batch {
    std::array<cec_action, 3> actions{};
    std::uint32_t             count = 0;
    void                      append(cec_action action) noexcept;
};

export struct cec_worker_core {
    const cec_options*  options  = nullptr;
    cec_session_core*   sessions = nullptr;
    cec_engine_metrics* metrics  = nullptr;
    cec_timer_heap      timers{};
    std::uint32_t       session_count = 0;
    std::uint32_t       live_sessions = 0;
    std::size_t         pattern_bytes = 0;
    bool                stopping      = false;
    bool                failed        = false;
    const wchar_t*      failure_stage = nullptr;
};

export bool cec_engine_session_enter_active(cec_session_core* session, cec_engine_metrics* metrics) noexcept;
export bool cec_engine_session_leave_active(cec_session_core* session, cec_engine_metrics* metrics) noexcept;
export cec_action_batch cec_core_start(cec_worker_core* core, std::uint32_t index, std::uint64_t now) noexcept;
export cec_action_batch cec_core_on_event(cec_worker_core* core, cec_event event, std::uint64_t now) noexcept;
export cec_action_batch cec_core_stop_session(cec_worker_core* core,
                                              std::uint32_t    index,
                                              std::uint64_t    now,
                                              bool             fatal) noexcept;
export cec_action_batch cec_core_poll(cec_worker_core* core, std::uint64_t now) noexcept;
export void             cec_core_anchor_attempt(cec_worker_core* core, std::uint32_t index, std::uint64_t now) noexcept;
export void             cec_core_anchor_close(cec_worker_core* core, std::uint32_t index, std::uint64_t now) noexcept;
static_assert(std::is_standard_layout_v<cec_session_core>);
static_assert(std::is_trivially_copyable_v<cec_session_core>);

// ---- definitions (the former internal.cpp) ----

static constexpr std::uint32_t cec_timer_absent = UINT32_MAX;

static bool cec_timer_less(const cec_timer_node& left, const cec_timer_node& right) noexcept {
    return left.deadline < right.deadline ||
           (left.deadline == right.deadline && left.session_index < right.session_index);
}

static void cec_timer_swap(cec_timer_heap* heap, std::uint32_t left, std::uint32_t right) noexcept {
    std::swap(heap->nodes[left], heap->nodes[right]);
    heap->positions[heap->nodes[left].session_index]  = left;
    heap->positions[heap->nodes[right].session_index] = right;
}

static void cec_timer_sift_up(cec_timer_heap* heap, std::uint32_t position) noexcept {
    while (position != 0) {
        const std::uint32_t parent = (position - 1U) / 2U;
        if (!cec_timer_less(heap->nodes[position], heap->nodes[parent])) {
            return;
        }
        cec_timer_swap(heap, position, parent);
        position = parent;
    }
}

static void cec_timer_sift_down(cec_timer_heap* heap, std::uint32_t position) noexcept {
    for (;;) {
        const std::uint32_t left = position * 2U + 1U;
        if (left >= heap->size) {
            return;
        }
        const std::uint32_t right    = left + 1U;
        std::uint32_t       smallest = left;
        if (right < heap->size && cec_timer_less(heap->nodes[right], heap->nodes[left])) {
            smallest = right;
        }
        if (!cec_timer_less(heap->nodes[smallest], heap->nodes[position])) {
            return;
        }
        cec_timer_swap(heap, position, smallest);
        position = smallest;
    }
}

bool cec_engine_session_enter_active(cec_session_core* session, cec_engine_metrics* metrics) noexcept {
    if (session == nullptr || metrics == nullptr || session->active_counted) {
        return false;
    }
    session->active_counted = true;
    metrics->active.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool cec_engine_session_leave_active(cec_session_core* session, cec_engine_metrics* metrics) noexcept {
    if (session == nullptr || metrics == nullptr || !session->active_counted) {
        return false;
    }
    session->active_counted = false;
    metrics->active.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

bool cec_worker_may_release(const cec_worker_lifecycle* lifecycle) noexcept {
    return lifecycle != nullptr && lifecycle->phase == cec_worker_phase::stopped && lifecycle->live_sessions == 0 &&
           lifecycle->total_outstanding == 0 && lifecycle->rio_outstanding == 0;
}

bool cec_timer_initialize(cec_timer_heap* heap,
                          cec_timer_node* nodes,
                          std::uint32_t*  positions,
                          std::uint32_t   capacity,
                          std::uint64_t   ticks_per_second) noexcept {
    if (heap == nullptr || nodes == nullptr || positions == nullptr || capacity == 0 || ticks_per_second == 0) {
        return false;
    }
    heap->nodes            = nodes;
    heap->positions        = positions;
    heap->size             = 0;
    heap->capacity         = capacity;
    heap->ticks_per_second = ticks_per_second;
    std::fill_n(positions, capacity, cec_timer_absent);
    return true;
}

std::uint64_t cec_timer_deadline_after_milliseconds(std::uint64_t now,
                                                    std::uint64_t milliseconds,
                                                    std::uint64_t ticks_per_second) noexcept {
    if (ticks_per_second == 0) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t seconds = milliseconds / 1000ULL;
    if (seconds > (maximum - now) / ticks_per_second) {
        return maximum;
    }
    const std::uint64_t whole_ticks = seconds * ticks_per_second;
    const std::uint64_t remainder   = milliseconds % 1000ULL;
    // Split the frequency before multiplication; even very large frequencies cannot wrap.
    const std::uint64_t fractional_ticks =
        remainder * (ticks_per_second / 1000ULL) + (remainder * (ticks_per_second % 1000ULL) + 999ULL) / 1000ULL;
    if (fractional_ticks > maximum - now - whole_ticks) {
        return maximum;
    }
    return now + whole_ticks + fractional_ticks;
}

bool cec_timer_insert_or_update(cec_timer_heap* heap, std::uint32_t session_index, std::uint64_t deadline) noexcept {
    if (heap == nullptr || session_index >= heap->capacity) {
        return false;
    }
    const std::uint32_t present = heap->positions[session_index];
    if (present != cec_timer_absent) {
        const std::uint64_t previous  = heap->nodes[present].deadline;
        heap->nodes[present].deadline = deadline;
        if (deadline < previous) {
            cec_timer_sift_up(heap, present);
        } else {
            cec_timer_sift_down(heap, present);
        }
        return true;
    }
    if (heap->size == heap->capacity) {
        return false;
    }
    const std::uint32_t position   = heap->size++;
    heap->nodes[position]          = cec_timer_node{ deadline, session_index };
    heap->positions[session_index] = position;
    cec_timer_sift_up(heap, position);
    return true;
}

bool cec_timer_remove(cec_timer_heap* heap, std::uint32_t session_index) noexcept {
    if (heap == nullptr || session_index >= heap->capacity) {
        return false;
    }
    const std::uint32_t position = heap->positions[session_index];
    if (position == cec_timer_absent) {
        return false;
    }
    heap->positions[session_index] = cec_timer_absent;
    --heap->size;
    if (position == heap->size) {
        return true;
    }
    heap->nodes[position]                                = heap->nodes[heap->size];
    heap->positions[heap->nodes[position].session_index] = position;
    if (position != 0 && cec_timer_less(heap->nodes[position], heap->nodes[(position - 1U) / 2U])) {
        cec_timer_sift_up(heap, position);
    } else {
        cec_timer_sift_down(heap, position);
    }
    return true;
}

bool cec_timer_pop_expired(cec_timer_heap* heap, std::uint64_t now, std::uint32_t* session_index) noexcept {
    if (heap == nullptr || session_index == nullptr || heap->size == 0 || heap->nodes[0].deadline > now) {
        return false;
    }
    *session_index = heap->nodes[0].session_index;
    return cec_timer_remove(heap, *session_index);
}

std::uint32_t cec_timer_wait_milliseconds(const cec_timer_heap* heap, std::uint64_t now) noexcept {
    if (heap == nullptr || heap->size == 0) {
        return UINT32_MAX;
    }
    const std::uint64_t deadline = heap->nodes[0].deadline;
    if (deadline <= now) {
        return 0;
    }
    const std::uint64_t frequency = heap->ticks_per_second;
    if (frequency == 0) {
        return UINT32_MAX - 1U;
    }
    const std::uint64_t remaining    = deadline - now;
    const std::uint64_t seconds      = remaining / frequency;
    const std::uint64_t maximum_wait = static_cast<std::uint64_t>(UINT32_MAX) - 1ULL;
    if (seconds > maximum_wait / 1000ULL) {
        return static_cast<std::uint32_t>(maximum_wait);
    }
    const std::uint64_t remainder               = remaining % frequency;
    std::uint64_t       fractional_milliseconds = 0;
    if (remainder <= std::numeric_limits<std::uint64_t>::max() / 1000ULL) {
        const std::uint64_t scaled = remainder * 1000ULL;
        fractional_milliseconds    = scaled / frequency + (scaled % frequency != 0 ? 1ULL : 0ULL);
    } else {
        // Find ceil(remainder * 1000 / frequency) without forming the overflowing product.
        std::uint64_t lower = 1;
        std::uint64_t upper = 1000;
        while (lower < upper) {
            const std::uint64_t middle = lower + (upper - lower) / 2ULL;
            const std::uint64_t ticks  = middle * (frequency / 1000ULL) + middle * (frequency % 1000ULL) / 1000ULL;
            if (ticks < remainder) {
                lower = middle + 1ULL;
            } else {
                upper = middle;
            }
        }
        fractional_milliseconds = lower;
    }
    return static_cast<std::uint32_t>(std::min(seconds * 1000ULL + fractional_milliseconds, maximum_wait));
}

void cec_action_batch::append(cec_action action) noexcept {
    // A single transition emits at most receive + send, or one close/connect/drain action.
    actions[count++] = action;
}

static void cec_core_fail_invariant(cec_worker_core* core, const wchar_t* stage) noexcept {
    core->failed        = true;
    core->failure_stage = stage;
}

static std::uint64_t cec_core_deadline(const cec_worker_core* core,
                                       std::uint64_t          now,
                                       std::uint64_t          milliseconds) noexcept {
    return cec_timer_deadline_after_milliseconds(now, milliseconds, core->timers.ticks_per_second);
}

static void cec_core_schedule(cec_worker_core* core, std::uint32_t index, std::uint64_t deadline) noexcept {
    if (!cec_timer_insert_or_update(&core->timers, index, deadline)) {
        cec_core_fail_invariant(core, L"client timer insert/update");
    }
}

static void cec_core_mark_done(cec_worker_core* core, std::uint32_t index) noexcept {
    cec_session_core& session = core->sessions[index];
    (void) cec_timer_remove(&core->timers, index);
    if (session.state != cec_engine_state::done) {
        if (core->live_sessions == 0) {
            cec_core_fail_invariant(core, L"client live session count");
            return;
        }
        --core->live_sessions;
    }
    (void) cec_engine_session_leave_active(&session, core->metrics);
    session.state       = cec_engine_state::done;
    session.deadline    = 0;
    session.next_action = 0;
}

cec_action_batch cec_core_start(cec_worker_core* core, std::uint32_t index, std::uint64_t now) noexcept {
    cec_action_batch actions{};
    if (index >= core->session_count) {
        cec_core_fail_invariant(core, L"client session index");
        return actions;
    }
    cec_session_core& session = core->sessions[index];
    if (session.state != cec_engine_state::dormant) {
        return actions;
    }
    session.state = cec_engine_state::connecting;
    session.deadline =
        cec_core_deadline(core, now, static_cast<std::uint64_t>(core->options->timeout_seconds) * 1000ULL);
    cec_core_schedule(core, index, session.deadline);
    actions.append({ cec_action_kind::connect, index });
    return actions;
}

void cec_core_anchor_attempt(cec_worker_core* core, std::uint32_t index, std::uint64_t now) noexcept {
    if (index >= core->session_count) {
        cec_core_fail_invariant(core, L"client attempt session index");
        return;
    }
    cec_session_core& session = core->sessions[index];
    session.started_at        = now;
    session.deadline =
        cec_core_deadline(core, now, static_cast<std::uint64_t>(core->options->timeout_seconds) * 1000ULL);
    cec_core_schedule(core, index, session.deadline);
}

void cec_core_anchor_close(cec_worker_core* core, std::uint32_t index, std::uint64_t now) noexcept {
    if (index >= core->session_count) {
        cec_core_fail_invariant(core, L"client close session index");
        return;
    }
    cec_session_core& session = core->sessions[index];
    if (session.state != cec_engine_state::closing) {
        return;
    }
    session.deadline = cec_core_deadline(core, now, 5000ULL);
    cec_core_schedule(core, index, session.deadline);
}

static cec_action_batch cec_core_begin_attempt(cec_worker_core* core, std::uint32_t index, std::uint64_t now) noexcept {
    cec_action_batch  actions{};
    cec_session_core& session = core->sessions[index];
    (void) cec_timer_remove(&core->timers, index);
    const std::uint64_t requested = core->options->protocol == cec_protocol::tcp ? core->options->pipeline_depth : 1U;
    if (core->options->echo_count != 0 && session.claimed > core->options->echo_count) {
        cec_core_fail_invariant(core, L"client per-session quota");
        return actions;
    }
    const std::uint64_t granted =
        core->options->echo_count == 0 ? requested : std::min(requested, core->options->echo_count - session.claimed);
    if (granted == 0) {
        session.reconnect_after_close = false;
        session.state                 = cec_engine_state::closing;
        session.deadline              = cec_core_deadline(core, now, 5000ULL);
        cec_core_schedule(core, index, session.deadline);
        actions.append({ cec_action_kind::close, index });
        return actions;
    }
    std::size_t bytes = 0;
    if (!cec_checked_product(core->pattern_bytes, static_cast<std::size_t>(granted), &bytes) || bytes == 0 ||
        bytes > UINT32_MAX) {
        cec_core_fail_invariant(core, L"client attempt extent");
        return actions;
    }
    session.claimed += granted;
    core->metrics->claimed.fetch_add(granted, std::memory_order_relaxed);
    session.requested_echoes  = granted;
    session.attempt_bytes     = bytes;
    session.send_done         = false;
    session.receive_done      = false;
    session.receive_posted    = true;
    session.receive_equal     = false;
    session.attempt_accounted = false;
    session.state             = cec_engine_state::active;
    cec_core_anchor_attempt(core, index, now);
    actions.append({ cec_action_kind::receive, index, bytes });
    actions.append({ cec_action_kind::send, index, bytes });
    return actions;
}

static cec_action_batch cec_core_fail(cec_worker_core* core, std::uint32_t index, std::uint64_t now) noexcept {
    cec_action_batch  actions{};
    cec_session_core& session = core->sessions[index];
    if (session.state == cec_engine_state::active && !session.attempt_accounted) {
        core->metrics->lost.fetch_add(session.requested_echoes, std::memory_order_relaxed);
        session.settled += session.requested_echoes;
        session.attempt_accounted = true;
    }
    core->metrics->network_errors.fetch_add(1, std::memory_order_relaxed);
    session.reconnect_after_close = core->options->reconnect_seconds >= 0 && !core->stopping;
    if (session.reconnect_after_close) {
        core->metrics->reconnects.fetch_add(1, std::memory_order_relaxed);
    }
    session.state = cec_engine_state::closing;
    cec_core_anchor_close(core, index, now);
    actions.append({ cec_action_kind::close, index });
    return actions;
}

static cec_action_batch cec_core_settle(cec_worker_core* core, std::uint32_t index, std::uint64_t now) noexcept {
    cec_session_core& session = core->sessions[index];
    if (!session.send_done || !session.receive_done || session.receive_posted) {
        return {};
    }
    if (session.receive_equal) {
        core->metrics->echoed.fetch_add(session.requested_echoes, std::memory_order_relaxed);
        core->metrics->bytes.fetch_add(session.attempt_bytes, std::memory_order_relaxed);
    } else {
        core->metrics->corrupted.fetch_add(session.requested_echoes, std::memory_order_relaxed);
    }
    session.settled += session.requested_echoes;
    session.attempt_accounted = true;
    cec_engine_record_latency(core->metrics, now >= session.started_at ? now - session.started_at : 0,
                              core->timers.ticks_per_second);
    (void) cec_timer_remove(&core->timers, index);
    if (core->options->interval_milliseconds != 0) {
        session.state       = cec_engine_state::pacing;
        session.next_action = cec_core_deadline(core, now, core->options->interval_milliseconds);
        cec_core_schedule(core, index, session.next_action);
        return {};
    }
    return cec_core_begin_attempt(core, index, now);
}

cec_action_batch cec_core_on_event(cec_worker_core* core, cec_event event, std::uint64_t now) noexcept {
    if (event.index >= core->session_count) {
        cec_core_fail_invariant(core, L"client event session index");
        return {};
    }
    cec_session_core& session = core->sessions[event.index];
    switch (event.kind) {
        case cec_event_kind::connected:
            if (session.state != cec_engine_state::connecting) {
                return {};
            }
            session.state = cec_engine_state::active;
            (void) cec_engine_session_enter_active(&session, core->metrics);
            core->metrics->connections.fetch_add(1, std::memory_order_relaxed);
            return cec_core_begin_attempt(core, event.index, now);
        case cec_event_kind::sent:
            if (session.state != cec_engine_state::active) {
                return {};
            }
            if (session.send_done || session.attempt_bytes == 0) {
                cec_core_fail_invariant(core, L"client sent event");
                return {};
            }
            session.send_done = true;
            return cec_core_settle(core, event.index, now);
        case cec_event_kind::received:
            if (session.state != cec_engine_state::active) {
                return {};
            }
            if (!session.receive_posted || session.receive_done || session.attempt_bytes == 0) {
                cec_core_fail_invariant(core, L"client received event");
                return {};
            }
            session.receive_posted = false;
            if (core->options->protocol == cec_protocol::tcp && event.bytes != session.attempt_bytes) {
                return cec_core_fail(core, event.index, now);
            }
            core->metrics->received_bytes.fetch_add(event.bytes, std::memory_order_relaxed);
            session.receive_done  = true;
            session.receive_equal = event.equal && event.bytes == session.attempt_bytes;
            return cec_core_settle(core, event.index, now);
        case cec_event_kind::transport_failure:
            if (session.state == cec_engine_state::closing || session.state == cec_engine_state::done) {
                return {};
            }
            return cec_core_fail(core, event.index, now);
        case cec_event_kind::generation_drained:
            if (session.state != cec_engine_state::closing) {
                return {};
            }
            (void) cec_timer_remove(&core->timers, event.index);
            if (session.reconnect_after_close && !core->stopping) {
                session.state       = cec_engine_state::reconnecting;
                session.next_action = cec_core_deadline(
                    core, now, static_cast<std::uint64_t>(core->options->reconnect_seconds) * 1000ULL);
                cec_core_schedule(core, event.index, session.next_action);
            } else {
                cec_core_mark_done(core, event.index);
            }
            return {};
    }
    return {};
}

cec_action_batch cec_core_stop_session(cec_worker_core* core,
                                       std::uint32_t    index,
                                       std::uint64_t    now,
                                       bool             fatal) noexcept {
    cec_action_batch actions{};
    if (index >= core->session_count) {
        return actions;
    }
    cec_session_core& session = core->sessions[index];
    if (session.state == cec_engine_state::done) {
        return actions;
    }
    if (session.state == cec_engine_state::active && !session.attempt_accounted) {
        if (fatal) {
            core->metrics->lost.fetch_add(session.requested_echoes, std::memory_order_relaxed);
        } else {
            core->metrics->cancelled.fetch_add(session.requested_echoes, std::memory_order_relaxed);
        }
        session.settled += session.requested_echoes;
        session.attempt_accounted = true;
    }
    session.reconnect_after_close = false;
    if (session.state != cec_engine_state::closing) {
        session.state = cec_engine_state::closing;
        cec_core_anchor_close(core, index, now);
    }
    actions.append({ cec_action_kind::close, index });
    return actions;
}

cec_action_batch cec_core_poll(cec_worker_core* core, std::uint64_t now) noexcept {
    std::uint32_t index = 0;
    if (!cec_timer_pop_expired(&core->timers, now, &index)) {
        return {};
    }
    cec_session_core& session = core->sessions[index];
    switch (session.state) {
        case cec_engine_state::active:
        case cec_engine_state::connecting:
            return cec_core_fail(core, index, now);
        case cec_engine_state::closing:
            {
                cec_action_batch actions{};
                actions.append({ cec_action_kind::drain_timeout, index });
                return actions;
            }
        case cec_engine_state::pacing:
            return cec_core_begin_attempt(core, index, now);
        case cec_engine_state::reconnecting:
            session.state = cec_engine_state::dormant;
            return cec_core_start(core, index, now);
        default:
            return {};
    }
}
