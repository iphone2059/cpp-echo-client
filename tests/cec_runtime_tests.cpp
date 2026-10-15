// clang-format off
// The SDK headers are order sensitive: WinSock2 before MSWSock and WS2tcpip, then
// Windows.h and mmsystem.h. clang-format must not reorder this block.
#include <WinSock2.h>
#include <MSWSock.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <mmsystem.h>
// clang-format on

#include <array>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>

import cec.test.support;

static int cec_runtime_failures = 0;

static void cec_runtime_expect(bool condition, const char* name) noexcept {
    if (condition) {
        std::printf("PASS %s\n", name);
    } else {
        std::fprintf(stderr, "FAIL %s\n", name);
        ++cec_runtime_failures;
    }
}

static void cec_runtime_test_partial_send() noexcept {
    cec_core_test_fixture                                  fixture(cec_protocol::tcp, 2);
    cec_fake_transport                                     transport;
    cec_fake_clock                                         clock;
    cec_worker_runtime<cec_fake_transport, cec_fake_clock> runtime{ fixture.core, transport, clock };
    runtime.execute(cec_core_start(&fixture.core, 0, clock.now()));
    runtime.completion({ cec_completion_kind::connected, 0 });
    cec_runtime_expect(transport.count == 3 && transport.journal[0].kind == cec_action_kind::connect &&
                           transport.journal[1].kind == cec_action_kind::receive &&
                           transport.journal[2].kind == cec_action_kind::send,
                       "runtime static test transport receives connect then RX/TX actions");
    --transport.pending;
    runtime.completion({ cec_completion_kind::received, 0, 4, true });
    --transport.pending;
    runtime.completion({ cec_completion_kind::send_progress, 0, 2 });
    cec_runtime_expect(
        fixture.metrics.echoed.load() == 0 && transport.pending == 1 && fixture.metrics.sent_bytes.load() == 2,
        "runtime partial send reposts without publishing sent event");
    --transport.pending;
    runtime.completion({ cec_completion_kind::sent, 0, 2 });
    cec_runtime_expect(fixture.metrics.echoed.load() == 2 && fixture.metrics.sent_bytes.load() == 4 &&
                           fixture.core.live_sessions == 0 && fixture.metrics.active.load() == 0 && !transport.rejected,
                       "runtime full send settles and drains exhausted generation through fake transport");
}

static void cec_runtime_test_nested_failure_and_cancel() noexcept {
    cec_core_test_fixture failed(cec_protocol::udp, 1);
    cec_fake_transport    failure_transport;
    failure_transport.fail_receive = true;
    cec_fake_clock                                         clock;
    cec_worker_runtime<cec_fake_transport, cec_fake_clock> failure_runtime{ failed.core, failure_transport, clock };
    failure_runtime.execute(cec_core_start(&failed.core, 0, clock.now()));
    cec_runtime_expect(failure_transport.count == 3 && failure_transport.journal[2].kind == cec_action_kind::close &&
                           failed.metrics.lost.load() == 1 && failed.metrics.network_errors.load() == 1 &&
                           failed.core.live_sessions == 0,
                       "runtime nested synchronous RX failure closes and suppresses remaining TX action");
    cec_core_test_fixture                                  stopped(cec_protocol::tcp, 0);
    cec_fake_transport                                     transport;
    cec_worker_runtime<cec_fake_transport, cec_fake_clock> runtime{ stopped.core, transport, clock };
    runtime.execute(cec_core_start(&stopped.core, 0, clock.now()));
    runtime.completion({ cec_completion_kind::connected, 0 });
    stopped.core.stopping = true;
    runtime.execute(cec_core_stop_session(&stopped.core, 0, clock.now(), false));
    cec_runtime_expect(
        transport.pending == 2 && stopped.core.live_sessions == 1 && stopped.metrics.cancelled.load() == 2,
        "runtime close requests cancellation without returning fake provider ownership");
    --transport.pending;
    runtime.completion({ cec_completion_kind::ignored, 0 });
    cec_runtime_expect(stopped.core.live_sessions == 1,
                       "runtime first cancelled completion cannot publish generation drained");
    --transport.pending;
    runtime.completion({ cec_completion_kind::drained, 0 });
    cec_runtime_expect(stopped.core.live_sessions == 0 && stopped.metrics.cancelled.load() == 2 &&
                           stopped.metrics.echoed.load() == 0 && !transport.rejected,
                       "runtime last cancelled completion returns drained event and closes ledger");
}

static void cec_runtime_test_stop_during_direct_drain() noexcept {
    for (const bool stop_after_receive : { false, true }) {
        cec_core_test_fixture fixture(cec_protocol::tcp, 0);
        fixture.options.timeout_seconds = 1;
        cec_fake_transport                                     transport;
        cec_fake_clock                                         clock;
        cec_fake_stop_probe                                    stop_probe;
        cec_worker_runtime<cec_fake_transport, cec_fake_clock> runtime{ fixture.core, transport, clock };
        runtime.execute(cec_core_start(&fixture.core, 0, clock.now()));
        runtime.completion({ cec_completion_kind::connected, 0 });
        clock.sample = 1100;
        cec_fake_cq_drain<cec_worker_runtime<cec_fake_transport, cec_fake_clock>> drain{ runtime, transport, stop_probe,
                                                                                         stop_after_receive };
        runtime.drain_before_timers(stop_probe, drain, 2);
        if (stop_after_receive) {
            cec_runtime_expect(fixture.metrics.cancelled.load() == 2 && fixture.metrics.lost.load() == 0 &&
                                   fixture.metrics.network_errors.load() == 0 && fixture.core.live_sessions == 1 &&
                                   transport.pending == 1 && transport.count == 4 &&
                                   transport.journal[3].kind == cec_action_kind::close,
                               "runtime stop published during direct CQ drain wins before cached due timeout");
        } else {
            cec_runtime_expect(fixture.metrics.cancelled.load() == 0 && fixture.metrics.lost.load() == 2 &&
                                   fixture.metrics.network_errors.load() == 1 && fixture.core.live_sessions == 1 &&
                                   transport.pending == 1,
                               "runtime same direct drain with no stop still executes expired timeout");
        }
        --transport.pending;
        runtime.completion({ cec_completion_kind::drained, 0 });
        cec_runtime_expect(fixture.core.live_sessions == 0 && fixture.metrics.active.load() == 0 && !transport.rejected,
                           "runtime direct-drain arbitration returns final provider completion before release");
    }
}

static void cec_runtime_test_new_posts_keep_decision_time() noexcept {
    cec_core_test_fixture fixture(cec_protocol::tcp, 0);
    fixture.options.timeout_seconds = 1;
    cec_fake_transport                                     transport;
    cec_fake_clock                                         clock;
    cec_fake_stop_probe                                    stop_probe;
    cec_worker_runtime<cec_fake_transport, cec_fake_clock> runtime{ fixture.core, transport, clock };
    runtime.execute(cec_core_start(&fixture.core, 0, clock.now()));
    runtime.completion({ cec_completion_kind::connected, 0 });
    clock.sample = 1100;
    cec_fake_rolling_cq_drain<cec_worker_runtime<cec_fake_transport, cec_fake_clock>> drain{ runtime, transport,
                                                                                             clock };
    runtime.drain_before_timers(stop_probe, drain, 2);
    cec_runtime_expect(clock.sample == 2300 && fixture.sessions[0].deadline == 2200 &&
                           fixture.sessions[0].state == cec_engine_state::active &&
                           fixture.metrics.echoed.load() == 2 && fixture.metrics.lost.load() == 0 &&
                           fixture.metrics.network_errors.load() == 0 && transport.pending == 2,
                       "runtime entry decision time protects newly posted attempt until next bounded acquire");
    runtime.poll_after_stop_observation(stop_probe);
    cec_runtime_expect(fixture.metrics.lost.load() == 2 && fixture.metrics.network_errors.load() == 1,
                       "runtime following decision uses current clock and expires uncompleted new attempt");
}

int main() {
    cec_runtime_test_partial_send();
    cec_runtime_test_nested_failure_and_cancel();
    cec_runtime_test_stop_during_direct_drain();
    cec_runtime_test_new_posts_keep_decision_time();
    return cec_runtime_failures == 0 ? 0 : 1;
}
