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
#include <memory>

import cec.test.support;

static int cec_core_failures = 0;

static void cec_core_expect(bool condition, const char* label) noexcept {
    if (condition) {
        std::printf("PASS %s\n", label);
    } else {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++cec_core_failures;
    }
}

static bool cec_core_action(const cec_action_batch& actions,
                            std::uint32_t           at,
                            cec_action_kind         kind,
                            std::size_t             bytes = 0) noexcept {
    return at < actions.count && actions.actions[at].kind == kind && actions.actions[at].index == 0 &&
           actions.actions[at].bytes == bytes;
}

static void cec_core_test_finite_pipeline() noexcept {
    cec_core_test_fixture fixture;
    cec_action_batch      actions = cec_core_start(&fixture.core, 0, 100);
    cec_core_expect(actions.count == 1 && cec_core_action(actions, 0, cec_action_kind::connect) &&
                        fixture.sessions[0].state == cec_engine_state::connecting,
                    "pure start emits connect and anchors timeout");
    actions = cec_core_on_event(&fixture.core, { cec_event_kind::connected, 0 }, 101);
    cec_core_expect(actions.count == 2 && cec_core_action(actions, 0, cec_action_kind::receive, 4) &&
                        cec_core_action(actions, 1, cec_action_kind::send, 4) && fixture.metrics.claimed.load() == 2,
                    "pure connect emits receive before send with per-session quota");
    for (std::uint32_t batch = 0; batch < 3; ++batch) {
        const std::size_t   bytes = batch == 2 ? 2U : 4U;
        const std::uint64_t now   = 102U + batch;
        actions                   = cec_core_on_event(&fixture.core, { cec_event_kind::received, 0, bytes, true }, now);
        cec_core_expect(actions.count == 0 && fixture.metrics.echoed.load() == batch * 2U,
                        "pure receive cannot settle before send completion");
        actions = cec_core_on_event(&fixture.core, { cec_event_kind::sent, 0 }, now);
        if (batch < 2) {
            const std::size_t next_bytes = batch == 1 ? 2U : 4U;
            cec_core_expect(actions.count == 2 && cec_core_action(actions, 0, cec_action_kind::receive, next_bytes) &&
                                cec_core_action(actions, 1, cec_action_kind::send, next_bytes),
                            "pure settlement emits next receive/send and final short batch");
        }
    }
    cec_core_expect(actions.count == 1 && cec_core_action(actions, 0, cec_action_kind::close) &&
                        fixture.sessions[0].state == cec_engine_state::closing && fixture.core.live_sessions == 1,
                    "pure exhausted quota closes generation before releasing session");
    actions = cec_core_on_event(&fixture.core, { cec_event_kind::generation_drained, 0 }, 105);
    cec_core_expect(actions.count == 0 && fixture.sessions[0].state == cec_engine_state::done &&
                        fixture.core.live_sessions == 0 && fixture.core.timers.size == 0 &&
                        fixture.metrics.active.load() == 0 && fixture.metrics.claimed.load() == 5 &&
                        fixture.metrics.echoed.load() == 5 && fixture.metrics.bytes.load() == 10 &&
                        fixture.metrics.latency_samples.load() == 3 && fixture.sessions[0].settled == 5,
                    "pure drained event closes exact ledger and removes timer");
}

static void cec_core_test_protocol_and_stop() noexcept {
    cec_core_test_fixture tcp;
    (void) cec_core_start(&tcp.core, 0, 0);
    (void) cec_core_on_event(&tcp.core, { cec_event_kind::connected, 0 }, 1);
    cec_action_batch actions = cec_core_on_event(&tcp.core, { cec_event_kind::received, 0, 1, false }, 2);
    cec_core_expect(actions.count == 1 && cec_core_action(actions, 0, cec_action_kind::close) &&
                        tcp.metrics.lost.load() == 2 && tcp.metrics.network_errors.load() == 1,
                    "pure TCP short receive is transport failure and consumes claimed quota");
    cec_core_test_fixture udp(cec_protocol::udp, 1);
    (void) cec_core_start(&udp.core, 0, 0);
    actions = cec_core_on_event(&udp.core, { cec_event_kind::connected, 0 }, 1);
    cec_core_expect(actions.count == 2 && cec_core_action(actions, 0, cec_action_kind::receive, 2),
                    "pure UDP claim has one echo regardless of configured pipeline");
    (void) cec_core_on_event(&udp.core, { cec_event_kind::sent, 0 }, 2);
    actions = cec_core_on_event(&udp.core, { cec_event_kind::received, 0, 1, false }, 2);
    cec_core_expect(actions.count == 1 && udp.metrics.corrupted.load() == 1 && udp.metrics.lost.load() == 0,
                    "pure UDP short receive settles corruption after sent");
    cec_core_test_fixture stopped(cec_protocol::tcp, 0);
    (void) cec_core_start(&stopped.core, 0, 0);
    (void) cec_core_on_event(&stopped.core, { cec_event_kind::connected, 0 }, 1);
    stopped.core.stopping        = true;
    actions                      = cec_core_stop_session(&stopped.core, 0, 2, false);
    const std::uint64_t deadline = stopped.sessions[0].deadline;
    (void) cec_core_stop_session(&stopped.core, 0, 3, false);
    cec_core_expect(actions.count == 1 && stopped.metrics.cancelled.load() == 2 && stopped.metrics.lost.load() == 0 &&
                        stopped.sessions[0].deadline == deadline && stopped.core.live_sessions == 1,
                    "pure controlled stop cancels once and preserves existing drain grace");
    (void) cec_core_on_event(&stopped.core, { cec_event_kind::sent, 0 }, 4);
    cec_core_expect(stopped.metrics.echoed.load() == 0 && stopped.metrics.cancelled.load() == 2,
                    "pure completion after stop cannot settle cancelled attempt");
    (void) cec_core_on_event(&stopped.core, { cec_event_kind::generation_drained, 0 }, 4);
    cec_core_expect(stopped.core.live_sessions == 0 && stopped.metrics.active.load() == 0,
                    "pure stop needs explicit drained event before done");
}

static void cec_core_test_reconnect_timer() noexcept {
    cec_core_test_fixture fixture;
    fixture.options.reconnect_seconds = 1;
    (void) cec_core_start(&fixture.core, 0, 100);
    (void) cec_core_on_event(&fixture.core, { cec_event_kind::connected, 0 }, 101);
    cec_action_batch actions = cec_core_on_event(&fixture.core, { cec_event_kind::transport_failure, 0 }, 102);
    cec_core_expect(actions.count == 1 && fixture.sessions[0].state == cec_engine_state::closing &&
                        fixture.metrics.reconnects.load() == 1,
                    "pure failed attempt defers reconnect until drained event");
    (void) cec_core_on_event(&fixture.core, { cec_event_kind::generation_drained, 0 }, 103);
    cec_core_expect(
        fixture.sessions[0].state == cec_engine_state::reconnecting && cec_core_poll(&fixture.core, 1102).count == 0,
        "pure reconnect waits complete configured interval");
    actions = cec_core_poll(&fixture.core, 1103);
    cec_core_expect(actions.count == 1 && cec_core_action(actions, 0, cec_action_kind::connect),
                    "pure reconnect timer emits connect without reading native slot");
    (void) cec_core_on_event(&fixture.core, { cec_event_kind::connected, 0 }, 1104);
    (void) cec_core_on_event(&fixture.core, { cec_event_kind::received, 0, 4, true }, 6104);
    actions = cec_core_on_event(&fixture.core, { cec_event_kind::sent, 0 }, 6104);
    cec_core_expect(
        actions.count == 2 && fixture.metrics.echoed.load() == 2 && cec_core_poll(&fixture.core, 6104).count == 0,
        "pure acquired completions before timer replace expired attempt deadline");
    actions = cec_core_poll(&fixture.core, 11104);
    cec_core_expect(actions.count == 1 && fixture.metrics.lost.load() == 3,
                    "pure timeout loses only current final short attempt");
    actions = cec_core_poll(&fixture.core, 16104);
    cec_core_expect(actions.count == 1 && cec_core_action(actions, 0, cec_action_kind::drain_timeout),
                    "pure closing timer requests transport drain check");
}

int cec_pure_engine_tests_main() {
    cec_core_test_finite_pipeline();
    cec_core_test_protocol_and_stop();
    cec_core_test_reconnect_timer();
    std::printf("client_core_failures=%d\n", cec_core_failures);
    return cec_core_failures == 0 ? 0 : 1;
}
