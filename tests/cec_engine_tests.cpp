#include "cec_engine_internal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <utility>

static int cec_engine_test_failures = 0;

static std::uint32_t cec_engine_test_xorshift(std::uint32_t* state) noexcept
{
    std::uint32_t value = *state;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    *state = value;
    return value;
}

static void cec_engine_test_expect(bool condition, const char* name) noexcept
{
    if (condition)
    {
        std::printf("PASS %s\n", name);
        return;
    }
    std::fprintf(stderr, "FAIL %s\n", name);
    ++cec_engine_test_failures;
}

static void cec_engine_test_lifecycle() noexcept
{
    cec_worker_lifecycle lifecycle{cec_worker_phase::draining, 0, 0, true};
    cec_engine_test_expect(!cec_worker_may_release(&lifecycle), "client release requires stopped phase");
    lifecycle.phase = cec_worker_phase::stopped;
    cec_engine_test_expect(!cec_worker_may_release(&lifecycle), "client release requires notification resolved");
    lifecycle.notification_armed = false;
    lifecycle.live_sessions = 1;
    cec_engine_test_expect(!cec_worker_may_release(&lifecycle), "client release requires no live sessions");
    lifecycle.live_sessions = 0;
    lifecycle.total_outstanding = 1;
    cec_engine_test_expect(!cec_worker_may_release(&lifecycle), "client release requires no outstanding operations");
    lifecycle.total_outstanding = 0;
    cec_engine_test_expect(cec_worker_may_release(&lifecycle), "client release accepts terminal lifecycle");

    cec_engine_test_expect(cec_session_terminal_accounting_valid(10, 8, 1, 1),
                           "client finite accounting accepts exact partition");
    cec_engine_test_expect(!cec_session_terminal_accounting_valid(10, 8, 1, 0),
                           "client finite accounting rejects missing result");
    OVERLAPPED expected{};
    OVERLAPPED other{};
    cec_engine_test_expect(cec_notification_packet_matches(7, &expected, 7, &expected),
                           "client notification identity requires matching key and OVERLAPPED");
    cec_engine_test_expect(!cec_notification_packet_matches(8, &expected, 7, &expected) &&
                               !cec_notification_packet_matches(7, &other, 7, &expected),
                           "client notification identity rejects wrong packet fields");
}

static void cec_engine_test_owner() noexcept
{
    HANDLE first = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    cec_handle_owner source{first};
    cec_handle_owner destination{std::move(source)};
    cec_engine_test_expect(source.get() == nullptr && destination.get() == first,
                           "client handle owner move transfers once");
    cec_handle_owner released_handle{destination.release()};
    cec_engine_test_expect(destination.get() == nullptr && released_handle.get() == first,
                           "client handle owner release transfers ownership");
    void* allocation = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    cec_virtual_arena_owner first_arena{allocation};
    cec_virtual_arena_owner second_arena{std::move(first_arena)};
    cec_engine_test_expect(first_arena.get() == nullptr && second_arena.get() == allocation,
                           "client virtual arena owner move transfers allocation");
    cec_virtual_arena_owner released_arena{second_arena.release()};
    cec_engine_test_expect(second_arena.get() == nullptr && released_arena.get() == allocation,
                           "client virtual arena owner release transfers ownership");
    void* heap_allocation = HeapAlloc(GetProcessHeap(), 0, 64);
    cec_heap_owner first_heap{heap_allocation};
    cec_heap_owner second_heap{std::move(first_heap)};
    cec_engine_test_expect(first_heap.get() == nullptr && second_heap.get() == heap_allocation,
                           "client heap owner move transfers allocation");
}

static void cec_engine_test_timer() noexcept
{
    std::array<cec_timer_node, 4> nodes{};
    std::array<std::uint32_t, 4> positions{};
    cec_timer_heap heap{};
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
    cec_engine_test_expect(cec_timer_insert_or_update(&heap, 2, 110) && cec_timer_pop_expired(&heap, 120, &index) &&
                               index == 2,
                           "client timer updates to earlier deadline");
    cec_engine_test_expect(cec_timer_remove(&heap, 3) && heap.size == 0, "client timer removes indexed entry");
    cec_engine_test_expect(cec_timer_insert_or_update(&heap, 0, std::numeric_limits<ULONGLONG>::max()) &&
                               cec_timer_wait_milliseconds(&heap, 0) == INFINITE - 1U,
                           "client timer wait saturates below INFINITE");
    cec_engine_test_expect(cec_timer_wait_milliseconds(&heap, std::numeric_limits<ULONGLONG>::max() - 5ULL) == 5,
                           "client timer handles maximum deadline without wraparound");
}

static void cec_engine_test_timer_model() noexcept
{
    constexpr std::uint32_t capacity = 64;
    constexpr std::uint32_t steps = 100000;
    std::array<cec_timer_node, capacity> nodes{};
    std::array<std::uint32_t, capacity> positions{};
    std::array<bool, capacity> active{};
    std::array<ULONGLONG, capacity> deadlines{};
    cec_timer_heap heap{};
    bool valid = cec_timer_initialize(&heap, nodes.data(), positions.data(), capacity);
    std::uint32_t random_state = 0xC0FFEE11U;

    for (std::uint32_t step = 0; step < steps && valid; ++step)
    {
        const std::uint32_t operation = cec_engine_test_xorshift(&random_state) & 3U;
        const std::uint32_t session_index = cec_engine_test_xorshift(&random_state) % capacity;
        const ULONGLONG now = static_cast<ULONGLONG>(step % 4096U);
        if (operation <= 1U)
        {
            const ULONGLONG deadline = static_cast<ULONGLONG>(cec_engine_test_xorshift(&random_state) % 4096U);
            valid = cec_timer_insert_or_update(&heap, session_index, deadline);
            active[session_index] = true;
            deadlines[session_index] = deadline;
        }
        else if (operation == 2U)
        {
            const bool expected = active[session_index];
            valid = cec_timer_remove(&heap, session_index) == expected;
            if (expected)
            {
                active[session_index] = false;
            }
        }
        else
        {
            bool expected_found = false;
            std::uint32_t expected_index = UINT32_MAX;
            ULONGLONG expected_deadline = 0;
            for (std::uint32_t index = 0; index < capacity; ++index)
            {
                if (active[index] && deadlines[index] <= now &&
                    (!expected_found || deadlines[index] < expected_deadline ||
                     (deadlines[index] == expected_deadline && index < expected_index)))
                {
                    expected_found = true;
                    expected_index = index;
                    expected_deadline = deadlines[index];
                }
            }
            std::uint32_t popped_index = UINT32_MAX;
            const bool popped = cec_timer_pop_expired(&heap, now, &popped_index);
            valid = popped == expected_found && (!popped || popped_index == expected_index);
            if (expected_found)
            {
                active[expected_index] = false;
            }
        }

        std::uint32_t active_count = 0;
        bool minimum_found = false;
        std::uint32_t minimum_index = UINT32_MAX;
        ULONGLONG minimum_deadline = 0;
        for (std::uint32_t index = 0; index < capacity && valid; ++index)
        {
            if (!active[index])
            {
                valid = positions[index] == UINT32_MAX;
                continue;
            }
            ++active_count;
            const std::uint32_t position = positions[index];
            valid = position < heap.size && nodes[position].session_index == index &&
                    nodes[position].deadline == deadlines[index];
            if (!minimum_found || deadlines[index] < minimum_deadline ||
                (deadlines[index] == minimum_deadline && index < minimum_index))
            {
                minimum_found = true;
                minimum_index = index;
                minimum_deadline = deadlines[index];
            }
        }
        valid = valid && heap.size == active_count;
        if (valid && minimum_found)
        {
            valid = nodes[0].session_index == minimum_index && nodes[0].deadline == minimum_deadline;
        }
        const DWORD expected_wait =
            !minimum_found ? INFINITE
            : minimum_deadline <= now
                ? 0
                : static_cast<DWORD>(std::min(minimum_deadline - now, static_cast<ULONGLONG>(INFINITE) - 1ULL));
        valid = valid && cec_timer_wait_milliseconds(&heap, now) == expected_wait;
    }

    cec_engine_test_expect(valid, "client timer matches fixed-seed reference model for 100000 operations");
}

static void cec_engine_test_pattern() noexcept
{
    const std::array<std::byte, 3> seed{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    std::array<std::byte, 11> output{};
    cec_fill_repeated_pattern(output.data(), output.size(), seed.data(), seed.size());
    const std::array<std::byte, 11> expected{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}, std::byte{'a'},
                                             std::byte{'b'}, std::byte{'c'}, std::byte{'a'}, std::byte{'b'},
                                             std::byte{'c'}, std::byte{'a'}, std::byte{'b'}};
    cec_engine_test_expect(output == expected, "client repeated pattern handles uneven sizes");
}

int main()
{
    cec_engine_test_lifecycle();
    cec_engine_test_owner();
    cec_engine_test_timer();
    cec_engine_test_timer_model();
    cec_engine_test_pattern();
    std::printf("client_engine_failures=%d\n", cec_engine_test_failures);
    return cec_engine_test_failures == 0 ? 0 : 1;
}
