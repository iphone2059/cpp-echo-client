//! Shared test recorders and fixtures, merged from the four former test headers into one
//! C++20 named module so the test translation units keep only .cpp sources.

module;

// clang-format off
// The SDK headers are order sensitive: WinSock2 before MSWSock and WS2tcpip, then
// Windows.h and mmsystem.h. clang-format must not reorder this block.
#include <WinSock2.h>
#include <Windows.h>
// clang-format on

#include <array>
#include <cstdint>

export module cec.test.support;

export import cec.engine;
export import cec.internal;
export import cec.types;

// ---- former test support headers ----

export class cec_core_test_fixture {
  public:
    cec_options                     options{};
    cec_engine_metrics              metrics{};
    std::array<cec_session_core, 1> sessions{};
    std::array<cec_timer_node, 1>   nodes{};
    std::array<std::uint32_t, 1>    positions{};
    cec_worker_core                 core{};

    explicit cec_core_test_fixture(cec_protocol  protocol = cec_protocol::tcp,
                                   std::uint64_t count    = 5,
                                   std::uint32_t depth    = 2) noexcept {
        options.protocol          = protocol;
        options.echo_count        = count;
        options.pipeline_depth    = depth;
        options.timeout_seconds   = 5;
        options.reconnect_seconds = -1;
        core.options              = &options;
        core.metrics              = &metrics;
        core.sessions             = sessions.data();
        core.session_count        = 1;
        core.live_sessions        = 1;
        core.pattern_bytes        = 2;
        (void) cec_timer_initialize(&core.timers, nodes.data(), positions.data(), 1);
    }
};

export class cec_fake_clock {
  public:
    std::uint64_t sample = 100;

    std::uint64_t now() const noexcept { return sample; }
};

export class cec_fake_transport {
  public:
    std::array<cec_action, 16> journal{};
    std::uint32_t              count        = 0;
    std::uint32_t              pending      = 0;
    bool                       is_closing   = false;
    bool                       fail_receive = false;
    bool                       rejected     = false;

    bool connect(std::uint32_t index) noexcept {
        is_closing       = false;
        journal[count++] = { cec_action_kind::connect, index };
        return true;
    }

    bool receive(std::uint32_t index, std::size_t bytes) noexcept {
        journal[count++] = { cec_action_kind::receive, index, bytes };
        if (fail_receive) {
            return false;
        }
        ++pending;
        return true;
    }

    bool send(std::uint32_t index, std::size_t bytes) noexcept {
        journal[count++] = { cec_action_kind::send, index, bytes };
        ++pending;
        return true;
    }

    bool continue_send(std::uint32_t index) noexcept { return send(index, 0); }

    bool close(std::uint32_t index) noexcept {
        journal[count++] = { cec_action_kind::close, index };
        is_closing       = true;
        return pending == 0;
    }

    bool drained(std::uint32_t) const noexcept { return pending == 0; }

    bool closing(std::uint32_t) const noexcept { return is_closing; }

    void invariant(const wchar_t*) noexcept { rejected = true; }

    void drain_timeout() noexcept { rejected = true; }
};

export class cec_fake_stop_probe {
  public:
    bool stopped = false;

    bool requested() const noexcept { return stopped; }

    bool fatal() const noexcept { return false; }
};

export template <class Runtime>
class cec_fake_cq_drain {
  public:
    cec_fake_cq_drain(Runtime&             runtime,
                      cec_fake_transport&  transport,
                      cec_fake_stop_probe& stop_probe,
                      bool                 stop_after_receive) noexcept :
        runtime_(runtime),
        transport_(transport),
        stop_probe_(stop_probe),
        stop_after_receive_(stop_after_receive) {}

    void operator()(std::uint64_t) noexcept {
        --transport_.pending;
        runtime_.completion({ cec_completion_kind::received, 0, 4, true });
        if (stop_after_receive_) {
            stop_probe_.stopped = true;
        }
    }

  private:
    Runtime&             runtime_;
    cec_fake_transport&  transport_;
    cec_fake_stop_probe& stop_probe_;
    bool                 stop_after_receive_;
};

export template <class Runtime>
class cec_fake_rolling_cq_drain {
  public:
    cec_fake_rolling_cq_drain(Runtime& runtime, cec_fake_transport& transport, cec_fake_clock& clock) noexcept :
        runtime_(runtime),
        transport_(transport),
        clock_(clock) {}

    void operator()(std::uint64_t) noexcept {
        clock_.sample = 1200;
        --transport_.pending;
        runtime_.completion({ cec_completion_kind::received, 0, 4, true });
        --transport_.pending;
        runtime_.completion({ cec_completion_kind::sent, 0, 4 });
        // Other entry-owned completions consume the bounded pass after this new attempt is posted.
        clock_.sample = 2300;
    }

  private:
    Runtime&            runtime_;
    cec_fake_transport& transport_;
    cec_fake_clock&     clock_;
};

// WinSock2.h must be included before the Windows networking headers below.
// clang-format off
// clang-format on

// Deterministic model of the documented worker arming sequence. It drives the production policy
// helpers (cec_notify_should_arm and the notification mark transitions) so the contract itself is
// what gets exercised, and it counts the observations that expose a missed arm.

export struct cec_notify_status_case {
    int                    status;
    cec_rio_notify_outcome expected;
    const char*            name;
};

export struct cec_notify_model {
    bool          armed{ false };
    std::uint32_t outstanding{ 0 };
    std::uint64_t arms{ 0 };
    std::uint64_t deliveries{ 0 };
    // Deliveries observed with no pending arm. Production fails fast on these, so the model records
    // them as violations instead of tolerating them as an empty delivery.
    std::uint64_t violations{ 0 };
    std::uint64_t timeout_wakeups_while_outstanding{ 0 };
    bool          stopped{ false };
};

export inline void cec_notify_model_post(cec_notify_model* model, std::uint32_t count) noexcept {
    model->outstanding += count;
    if (cec_notify_should_arm(model->armed, model->outstanding) && cec_notification_mark_rearmed(&model->armed)) {
        ++model->arms;
    }
}

export inline void cec_notify_model_deliver(cec_notify_model* model, std::uint32_t retired) noexcept {
    if (!cec_notification_mark_delivered(&model->armed)) {
        ++model->violations;
        return;
    }
    ++model->deliveries;
    model->outstanding -= retired <= model->outstanding ? retired : model->outstanding;
    if (!model->stopped && cec_notify_should_arm(model->armed, model->outstanding) &&
        cec_notification_mark_rearmed(&model->armed)) {
        ++model->arms;
    }
}

// Work retired by RIODequeueCompletion alone. The notification that woke the worker may still be
// pending in the port queue, so this drain path does not consume a notification.
export inline void cec_notify_model_retire(cec_notify_model* model, std::uint32_t retired) noexcept {
    model->outstanding -= retired <= model->outstanding ? retired : model->outstanding;
}

export inline void cec_notify_model_timeout(cec_notify_model* model) noexcept {
    if (model->outstanding != 0 && !model->armed) {
        ++model->timeout_wakeups_while_outstanding;
    }
}

// Teardown contract: the completion queue may be closed once no RIO operation is outstanding and the
// arm state was never violated. A notification that is still in flight (arms - deliveries == 1) is
// allowed to remain unconsumed; production never waits for it and never forges it.
export inline bool cec_notify_model_may_close(const cec_notify_model* model) noexcept {
    return model->outstanding == 0 && model->violations == 0 && model->arms >= model->deliveries &&
           model->arms - model->deliveries <= 1U;
}

// v1 terminal accounting model: every attempt the run posted, plus the quota a terminal failure
// never posted, must settle as echoed, corrupted, lost or cancelled.
export constexpr bool cec_test_terminal_accounting_model_valid(std::uint64_t claimed,
                                                               std::uint64_t terminal_unclaimed,
                                                               std::uint64_t echoed,
                                                               std::uint64_t corrupted,
                                                               std::uint64_t lost,
                                                               std::uint64_t cancelled) noexcept {
    const std::uint64_t attempted = claimed + terminal_unclaimed;
    const std::uint64_t settled   = echoed + corrupted + lost + cancelled;
    if (attempted != settled) {
        return false;
    }
    return echoed <= attempted && corrupted <= attempted - echoed && lost <= attempted - echoed - corrupted;
}
