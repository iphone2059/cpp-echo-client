#include "cec_engine_internal.h"
#include "cec_test_models.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <utility>

static int cec_engine_test_failures = 0;

static std::uint32_t cec_engine_test_xorshift(std::uint32_t* state) noexcept {
    std::uint32_t value = *state;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    *state = value;
    return value;
}

static void cec_engine_test_expect(bool condition, const char* name) noexcept {
    if (condition) {
        std::printf("PASS %s\n", name);
        return;
    }
    std::fprintf(stderr, "FAIL %s\n", name);
    ++cec_engine_test_failures;
}

static void cec_engine_test_lifecycle() noexcept {
    cec_worker_lifecycle lifecycle{ cec_worker_phase::draining, 0, 0, 0 };
    cec_engine_test_expect(!cec_worker_may_release(&lifecycle), "client release requires stopped phase");
    lifecycle.phase           = cec_worker_phase::stopped;
    lifecycle.rio_outstanding = 1;
    cec_engine_test_expect(!cec_worker_may_release(&lifecycle), "client release requires no outstanding RIO work");
    lifecycle.rio_outstanding = 0;
    lifecycle.live_sessions   = 1;
    cec_engine_test_expect(!cec_worker_may_release(&lifecycle), "client release requires no live sessions");
    lifecycle.live_sessions     = 0;
    lifecycle.total_outstanding = 1;
    cec_engine_test_expect(!cec_worker_may_release(&lifecycle), "client release requires no outstanding operations");
    lifecycle.total_outstanding = 0;
    cec_engine_test_expect(cec_worker_may_release(&lifecycle), "client release accepts terminal lifecycle");

    OVERLAPPED expected{};
    OVERLAPPED other{};
    cec_engine_test_expect(cec_notification_packet_matches(7, &expected, 7, &expected),
                           "client notification identity requires matching key and OVERLAPPED");
    cec_engine_test_expect(!cec_notification_packet_matches(8, &expected, 7, &expected) &&
                               !cec_notification_packet_matches(7, &other, 7, &expected),
                           "client notification identity rejects wrong packet fields");
}

static void cec_engine_test_accounting_model() noexcept {
    cec_engine_test_expect(cec_test_terminal_accounting_model_valid(10, 8, 1, 1),
                           "client finite-attempt model accepts exact partition");
    cec_engine_test_expect(!cec_test_terminal_accounting_model_valid(10, 8, 1, 0),
                           "client finite-attempt model excludes an unfinished controlled stop");
    cec_engine_test_expect(!cec_test_terminal_accounting_model_valid(10, 11, 0, 0),
                           "client finite-attempt model rejects more echoes than claims");
    cec_engine_test_expect(!cec_test_terminal_accounting_model_valid(10, 8, 3, 0),
                           "client finite-attempt model rejects an excessive corruption count");
    cec_engine_test_expect(cec_test_terminal_accounting_model_valid(0, 0, 0, 0),
                           "client finite-attempt model accepts an empty workload");
    cec_engine_test_expect(cec_test_terminal_accounting_model_valid(UINT64_MAX, UINT64_MAX - 2ULL, 1, 1) &&
                               !cec_test_terminal_accounting_model_valid(UINT64_MAX, UINT64_MAX, 1, UINT64_MAX),
                           "client finite-attempt model handles extreme counts without wrapped sums");
}

static void cec_engine_test_owner() noexcept {
    HANDLE           first = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    cec_handle_owner source{ first };
    cec_handle_owner destination{ std::move(source) };
    cec_engine_test_expect(source.get() == nullptr && destination.get() == first,
                           "client handle owner move transfers once");
    cec_handle_owner released_handle{ destination.release() };
    cec_engine_test_expect(destination.get() == nullptr && released_handle.get() == first,
                           "client handle owner release transfers ownership");
    void*                   allocation = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    cec_virtual_arena_owner first_arena{ allocation };
    cec_virtual_arena_owner second_arena{ std::move(first_arena) };
    cec_engine_test_expect(first_arena.get() == nullptr && second_arena.get() == allocation,
                           "client virtual arena owner move transfers allocation");
    cec_virtual_arena_owner released_arena{ second_arena.release() };
    cec_engine_test_expect(second_arena.get() == nullptr && released_arena.get() == allocation,
                           "client virtual arena owner release transfers ownership");
    void*          heap_allocation = HeapAlloc(GetProcessHeap(), 0, 64);
    cec_heap_owner first_heap{ heap_allocation };
    cec_heap_owner second_heap{ std::move(first_heap) };
    cec_engine_test_expect(first_heap.get() == nullptr && second_heap.get() == heap_allocation,
                           "client heap owner move transfers allocation");
}

static void cec_engine_test_timer() noexcept {
    std::array<cec_timer_node, 4> nodes{};
    std::array<std::uint32_t, 4>  positions{};
    cec_timer_heap                heap{};
    cec_engine_test_expect(cec_timer_initialize(&heap, nodes.data(), positions.data(), 4),
                           "client timer initializes fixed storage");
    cec_engine_test_expect(cec_timer_wait_milliseconds(&heap, 100) == INFINITE,
                           "client empty timer waits indefinitely");
    cec_engine_test_expect(cec_timer_insert_or_update(&heap, 2, 150) && cec_timer_insert_or_update(&heap, 1, 120) &&
                               cec_timer_insert_or_update(&heap, 3, 120),
                           "client timer inserts deadlines");
    cec_engine_test_expect(cec_timer_wait_milliseconds(&heap, 100) == 20, "client timer waits for nearest deadline");
    std::uint32_t index = UINT32_MAX;
    cec_engine_test_expect(cec_timer_pop_expired(&heap, 120, &index) && index == 1,
                           "client timer orders equal deadlines by index");
    cec_engine_test_expect(
        cec_timer_insert_or_update(&heap, 2, 110) && cec_timer_pop_expired(&heap, 120, &index) && index == 2,
        "client timer updates to earlier deadline");
    cec_engine_test_expect(cec_timer_remove(&heap, 3) && heap.size == 0, "client timer removes indexed entry");
    cec_engine_test_expect(cec_timer_insert_or_update(&heap, 0, std::numeric_limits<ULONGLONG>::max()) &&
                               cec_timer_wait_milliseconds(&heap, 0) == INFINITE - 1U,
                           "client timer wait saturates below INFINITE");
    cec_engine_test_expect(cec_timer_wait_milliseconds(&heap, std::numeric_limits<ULONGLONG>::max() - 5ULL) == 5,
                           "client timer handles maximum deadline without wraparound");
}

static void cec_engine_test_timer_model() noexcept {
    constexpr std::uint32_t              capacity = 64;
    constexpr std::uint32_t              steps    = 100000;
    std::array<cec_timer_node, capacity> nodes{};
    std::array<std::uint32_t, capacity>  positions{};
    std::array<bool, capacity>           active{};
    std::array<ULONGLONG, capacity>      deadlines{};
    cec_timer_heap                       heap{};
    bool                                 valid = cec_timer_initialize(&heap, nodes.data(), positions.data(), capacity);
    std::uint32_t                        random_state = 0xC0FFEE11U;

    for (std::uint32_t step = 0; step < steps && valid; ++step) {
        const std::uint32_t operation     = cec_engine_test_xorshift(&random_state) & 3U;
        const std::uint32_t session_index = cec_engine_test_xorshift(&random_state) % capacity;
        const ULONGLONG     now           = static_cast<ULONGLONG>(step % 4096U);
        if (operation <= 1U) {
            const ULONGLONG deadline = static_cast<ULONGLONG>(cec_engine_test_xorshift(&random_state) % 4096U);
            valid                    = cec_timer_insert_or_update(&heap, session_index, deadline);
            active[session_index]    = true;
            deadlines[session_index] = deadline;
        } else if (operation == 2U) {
            const bool expected = active[session_index];
            valid               = cec_timer_remove(&heap, session_index) == expected;
            if (expected) {
                active[session_index] = false;
            }
        } else {
            bool          expected_found    = false;
            std::uint32_t expected_index    = UINT32_MAX;
            ULONGLONG     expected_deadline = 0;
            for (std::uint32_t index = 0; index < capacity; ++index) {
                if (active[index] && deadlines[index] <= now &&
                    (!expected_found || deadlines[index] < expected_deadline ||
                     (deadlines[index] == expected_deadline && index < expected_index))) {
                    expected_found    = true;
                    expected_index    = index;
                    expected_deadline = deadlines[index];
                }
            }
            std::uint32_t popped_index = UINT32_MAX;
            const bool    popped       = cec_timer_pop_expired(&heap, now, &popped_index);
            valid                      = popped == expected_found && (!popped || popped_index == expected_index);
            if (expected_found) {
                active[expected_index] = false;
            }
        }

        std::uint32_t active_count     = 0;
        bool          minimum_found    = false;
        std::uint32_t minimum_index    = UINT32_MAX;
        ULONGLONG     minimum_deadline = 0;
        for (std::uint32_t index = 0; index < capacity && valid; ++index) {
            if (!active[index]) {
                valid = positions[index] == UINT32_MAX;
                continue;
            }
            ++active_count;
            const std::uint32_t position = positions[index];
            valid                        = position < heap.size && nodes[position].session_index == index &&
                    nodes[position].deadline == deadlines[index];
            if (!minimum_found || deadlines[index] < minimum_deadline ||
                (deadlines[index] == minimum_deadline && index < minimum_index)) {
                minimum_found    = true;
                minimum_index    = index;
                minimum_deadline = deadlines[index];
            }
        }
        valid = valid && heap.size == active_count;
        if (valid && minimum_found) {
            valid = nodes[0].session_index == minimum_index && nodes[0].deadline == minimum_deadline;
        }
        const DWORD expected_wait =
            !minimum_found ?
                INFINITE :
            minimum_deadline <= now ?
                0 :
                static_cast<DWORD>(std::min(minimum_deadline - now, static_cast<ULONGLONG>(INFINITE) - 1ULL));
        valid = valid && cec_timer_wait_milliseconds(&heap, now) == expected_wait;
    }

    cec_engine_test_expect(valid, "client timer matches fixed-seed reference model for 100000 operations");
}

static void cec_engine_test_qpc_timer() noexcept {
    constexpr ULONGLONG frequency = 10'000'000ULL;
    constexpr ULONGLONG now       = 100'000ULL;
    constexpr ULONGLONG maximum   = std::numeric_limits<ULONGLONG>::max();
    cec_engine_test_expect(cec_timer_deadline_after_milliseconds(now, 0, frequency) == 100'000ULL,
                           "client QPC timer preserves a zero delay");
    cec_engine_test_expect(cec_timer_deadline_after_milliseconds(now, 1, frequency) == 110'000ULL,
                           "client QPC timer converts 1 ms at 10 MHz");
    cec_engine_test_expect(cec_timer_deadline_after_milliseconds(now, 10, frequency) == 200'000ULL,
                           "client QPC timer converts 10 ms at 10 MHz");
    cec_engine_test_expect(cec_timer_deadline_after_milliseconds(now, 1001, frequency) == 10'110'000ULL,
                           "client QPC timer combines whole seconds and fractional ticks");
    cec_engine_test_expect(cec_timer_deadline_after_milliseconds(now, 1, 1001) == 100'002ULL &&
                               cec_timer_deadline_after_milliseconds(now, 10, 1001) == 100'011ULL &&
                               cec_timer_deadline_after_milliseconds(0, 1001, 1001) == 1003ULL,
                           "client QPC timer rounds non-divisible frequencies upward");
    cec_engine_test_expect(cec_timer_deadline_after_milliseconds(0, maximum, frequency) == maximum,
                           "client QPC timer saturates a whole-second multiplication overflow");
    cec_engine_test_expect(cec_timer_deadline_after_milliseconds(maximum - 9999ULL, 1, frequency) == maximum &&
                               cec_timer_deadline_after_milliseconds(maximum - 10'000ULL, 1, frequency) == maximum,
                           "client QPC timer saturates deadline addition without wrapping");
    cec_engine_test_expect(cec_timer_deadline_after_milliseconds(0, 500, maximum) == 9'223'372'036'854'775'808ULL,
                           "client QPC timer converts a fractional second at the maximum frequency");

    std::array<cec_timer_node, 1> nodes{};
    std::array<std::uint32_t, 1>  positions{};
    cec_timer_heap                heap{};
    cec_engine_test_expect(!cec_timer_initialize(&heap, nodes.data(), positions.data(), 1, 0),
                           "client QPC timer rejects a zero frequency");
    cec_engine_test_expect(cec_timer_initialize(&heap, nodes.data(), positions.data(), 1, frequency),
                           "client QPC timer initializes a 10 MHz heap");
    constexpr std::array<ULONGLONG, 8>   deadlines{ 99'999ULL,  100'000ULL, 100'001ULL, 105'000ULL,
                                                  110'000ULL, 110'001ULL, 200'000ULL, 200'001ULL };
    constexpr std::array<DWORD, 8>       waits{ 0U, 0U, 1U, 1U, 1U, 2U, 10U, 11U };
    constexpr std::array<const char*, 8> names{
        "client QPC wait returns zero after expiry", "client QPC wait returns zero at its deadline",
        "client QPC wait rounds one tick upward",    "client QPC wait rounds half a millisecond upward",
        "client QPC wait preserves exactly 1 ms",    "client QPC wait rounds just over 1 ms upward",
        "client QPC wait preserves exactly 10 ms",   "client QPC wait rounds just over 10 ms upward"
    };
    for (std::size_t index = 0; index < deadlines.size(); ++index) {
        cec_engine_test_expect(cec_timer_insert_or_update(&heap, 0, deadlines[index]) &&
                                   cec_timer_wait_milliseconds(&heap, now) == waits[index],
                               names[index]);
    }
    std::uint32_t expired = UINT32_MAX;
    cec_engine_test_expect(cec_timer_insert_or_update(&heap, 0, 110'000ULL) &&
                               !cec_timer_pop_expired(&heap, 109'999ULL, &expired) &&
                               cec_timer_pop_expired(&heap, 110'000ULL, &expired) && expired == 0,
                           "client QPC timer expires at the tick deadline without an early millisecond wake");
    cec_engine_test_expect(
        cec_timer_insert_or_update(&heap, 0, maximum) && cec_timer_wait_milliseconds(&heap, 0) == INFINITE - 1U,
        "client QPC wait clamps an extreme duration below INFINITE");

    cec_engine_test_expect(cec_timer_initialize(&heap, nodes.data(), positions.data(), 1, 1001) &&
                               cec_timer_insert_or_update(&heap, 0, 2) && cec_timer_wait_milliseconds(&heap, 0) == 2,
                           "client QPC wait rounds ticks at a non-divisible frequency");
    cec_engine_test_expect(cec_timer_initialize(&heap, nodes.data(), positions.data(), 1, maximum) &&
                               cec_timer_insert_or_update(&heap, 0, 9'223'372'036'854'775'807ULL) &&
                               cec_timer_wait_milliseconds(&heap, 0) == 500,
                           "client QPC wait handles a sub-half-second overflowing numerator");
    cec_engine_test_expect(cec_timer_insert_or_update(&heap, 0, 9'223'372'036'854'775'808ULL) &&
                               cec_timer_wait_milliseconds(&heap, 0) == 501,
                           "client QPC wait rounds an overflowing numerator just beyond half a second");
    cec_engine_test_expect(
        cec_timer_insert_or_update(&heap, 0, maximum - 1ULL) && cec_timer_wait_milliseconds(&heap, 0) == 1000,
        "client QPC wait handles an overflowing numerator near one second");
}

static void cec_engine_test_latency() noexcept {
    cec_engine_test_expect(cec_engine_ticks_to_microseconds(0, 10'000'000ULL) == 1 &&
                               cec_engine_ticks_to_microseconds(1, 10'000'000ULL) == 1 &&
                               cec_engine_ticks_to_microseconds(15, 10'000'000ULL) == 1,
                           "client latency has a 1 us floor for zero and sub-microsecond samples");
    cec_engine_test_expect(cec_engine_ticks_to_microseconds(9999, 10'000'000ULL) == 999 &&
                               cec_engine_ticks_to_microseconds(10'000, 10'000'000ULL) == 1000 &&
                               cec_engine_ticks_to_microseconds(10'000'005, 10'000'000ULL) == 1'000'000ULL,
                           "client latency truncates fractional microseconds at the actual QPC frequency");
    cec_engine_test_expect(cec_engine_ticks_to_microseconds(1000, 1001) == 999'000ULL &&
                               cec_engine_ticks_to_microseconds(2, 3) == 666'666ULL,
                           "client latency handles non-divisible frequencies");
    cec_engine_test_expect(
        cec_engine_ticks_to_microseconds(100'000'000'000'000'000ULL, 10'000'000ULL) == 10'000'000'000'000'000ULL,
        "client latency converts long durations without overflowing tick multiplication");
    cec_engine_test_expect(cec_engine_ticks_to_microseconds(UINT64_MAX, UINT64_MAX) == 1'000'000ULL &&
                               cec_engine_ticks_to_microseconds(UINT64_MAX - 1ULL, UINT64_MAX) == 999'999ULL,
                           "client latency handles maximum frequency with and without a whole second");
    cec_engine_test_expect(cec_engine_ticks_to_microseconds(9'223'372'036'854'775'807ULL, UINT64_MAX) == 499'999ULL &&
                               cec_engine_ticks_to_microseconds(9'223'372'036'854'775'808ULL, UINT64_MAX) == 500'000ULL,
                           "client latency floors extreme fractional products on either side of half a second");
    cec_engine_test_expect(cec_engine_ticks_to_microseconds(20'000'000'000'000ULL, 30'000'000'000'000ULL) == 666'666ULL,
                           "client latency handles an overflowing fractional product at a non-divisible frequency");
    cec_engine_test_expect(cec_engine_ticks_to_microseconds(UINT64_MAX, 1) == UINT64_MAX &&
                               cec_engine_ticks_to_microseconds(184'467'440'737'099ULL, 10) == UINT64_MAX,
                           "client latency saturates whole and fractional addition overflow");
    cec_engine_test_expect(
        cec_engine_ticks_to_microseconds(184'467'440'737'095ULL, 10) == 18'446'744'073'709'500'000ULL &&
            cec_engine_ticks_to_microseconds(UINT64_MAX, 1'000'000ULL) == UINT64_MAX,
        "client latency preserves large representable values below saturation");
    cec_engine_test_expect(cec_latency_bin_lower_bound(0) == 1 && cec_latency_bin_lower_bound(1) == 2 &&
                               cec_latency_bin_lower_bound(62) == 4'611'686'018'427'387'904ULL,
                           "client latency histogram reports ordinary bucket lower bounds");
    cec_engine_test_expect(cec_latency_bin_lower_bound(63) == 9'223'372'036'854'775'808ULL &&
                               cec_latency_bin_lower_bound(64) == 9'223'372'036'854'775'808ULL &&
                               cec_latency_bin_lower_bound(UINT_MAX) == 9'223'372'036'854'775'808ULL,
                           "client latency histogram reports the true final bucket lower bound without invalid shifts");
}

static void cec_engine_test_pattern() noexcept {
    const std::array<std::byte, 3> seed{ std::byte{ 'a' }, std::byte{ 'b' }, std::byte{ 'c' } };
    std::array<std::byte, 11>      output{};
    cec_fill_repeated_pattern(output.data(), output.size(), seed.data(), seed.size());
    const std::array<std::byte, 11> expected{ std::byte{ 'a' }, std::byte{ 'b' }, std::byte{ 'c' }, std::byte{ 'a' },
                                              std::byte{ 'b' }, std::byte{ 'c' }, std::byte{ 'a' }, std::byte{ 'b' },
                                              std::byte{ 'c' }, std::byte{ 'a' }, std::byte{ 'b' } };
    cec_engine_test_expect(output == expected, "client repeated pattern handles uneven sizes");
}

int main() {
    cec_engine_test_lifecycle();
    cec_engine_test_accounting_model();
    cec_engine_test_owner();
    cec_engine_test_timer();
    cec_engine_test_timer_model();
    cec_engine_test_qpc_timer();
    cec_engine_test_latency();
    cec_engine_test_pattern();
    std::printf("client_engine_failures=%d\n", cec_engine_test_failures);
    return cec_engine_test_failures == 0 ? 0 : 1;
}
