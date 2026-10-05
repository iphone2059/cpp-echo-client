#include "cec_engine_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

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

[[noreturn]] void cec_engine_fail_fast(const wchar_t* stage, int error) noexcept {
    char text[160]{};
    (void) WideCharToMultiByte(CP_UTF8, 0, stage, -1, text, sizeof(text), nullptr, nullptr);
    std::fprintf(stderr, "%s failed: native_error=%d\n", text, error);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(cec_exit_code::internal));
    __assume(0);
}

void cec_require_rio_notify_success(int status, const wchar_t* stage) noexcept {
    const cec_rio_notify_outcome outcome = cec_rio_notify_outcome_of(status);
    if (outcome == cec_rio_notify_outcome::armed) {
        return;
    }
    if (outcome == cec_rio_notify_outcome::duplicate_arm) {
        // A correct state machine never arms a queue whose previous notification is still pending,
        // so report this distinctly instead of folding it into a generic RIONotify failure.
        cec_engine_fail_fast(L"RIONotify duplicate arm", status);
    }
    cec_engine_fail_fast(stage, status);
}

ULONG cec_require_valid_dequeue_count(ULONG count, const wchar_t* stage) noexcept {
    if (count == RIO_CORRUPT_CQ) {
        cec_engine_fail_fast(stage, ERROR_INVALID_DATA);
    }
    return count;
}

cec_socket_owner::cec_socket_owner() noexcept : value_(INVALID_SOCKET) {}

cec_socket_owner::cec_socket_owner(SOCKET value) noexcept : value_(value) {}

cec_socket_owner::~cec_socket_owner() noexcept {
    reset();
}

cec_socket_owner::cec_socket_owner(cec_socket_owner&& other) noexcept : value_(other.release()) {}

cec_socket_owner& cec_socket_owner::operator=(cec_socket_owner&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

SOCKET cec_socket_owner::get() const noexcept {
    return value_;
}

SOCKET cec_socket_owner::release() noexcept {
    const SOCKET value = value_;
    value_             = INVALID_SOCKET;
    return value;
}

void cec_socket_owner::reset(SOCKET value) noexcept {
    if (value_ != INVALID_SOCKET) {
        closesocket(value_);
    }
    value_ = value;
}

cec_handle_owner::cec_handle_owner() noexcept : value_(nullptr) {}

cec_handle_owner::cec_handle_owner(HANDLE value) noexcept : value_(value) {}

cec_handle_owner::~cec_handle_owner() noexcept {
    reset();
}

cec_handle_owner::cec_handle_owner(cec_handle_owner&& other) noexcept : value_(other.release()) {}

cec_handle_owner& cec_handle_owner::operator=(cec_handle_owner&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

HANDLE cec_handle_owner::get() const noexcept {
    return value_;
}

HANDLE cec_handle_owner::release() noexcept {
    HANDLE value = value_;
    value_       = nullptr;
    return value;
}

void cec_handle_owner::reset(HANDLE value) noexcept {
    if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
        CloseHandle(value_);
    }
    value_ = value;
}

cec_virtual_arena_owner::cec_virtual_arena_owner() noexcept : value_(nullptr) {}

cec_virtual_arena_owner::cec_virtual_arena_owner(void* value) noexcept : value_(value) {}

cec_virtual_arena_owner::~cec_virtual_arena_owner() noexcept {
    reset();
}

cec_virtual_arena_owner::cec_virtual_arena_owner(cec_virtual_arena_owner&& other) noexcept : value_(other.release()) {}

cec_virtual_arena_owner& cec_virtual_arena_owner::operator=(cec_virtual_arena_owner&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

void* cec_virtual_arena_owner::get() const noexcept {
    return value_;
}

void* cec_virtual_arena_owner::release() noexcept {
    void* value = value_;
    value_      = nullptr;
    return value;
}

void cec_virtual_arena_owner::reset(void* value) noexcept {
    if (value_ != nullptr) {
        VirtualFree(value_, 0, MEM_RELEASE);
    }
    value_ = value;
}

cec_heap_owner::cec_heap_owner() noexcept : value_(nullptr) {}

cec_heap_owner::cec_heap_owner(void* value) noexcept : value_(value) {}

cec_heap_owner::~cec_heap_owner() noexcept {
    reset();
}

cec_heap_owner::cec_heap_owner(cec_heap_owner&& other) noexcept : value_(other.release()) {}

cec_heap_owner& cec_heap_owner::operator=(cec_heap_owner&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

void* cec_heap_owner::get() const noexcept {
    return value_;
}

void* cec_heap_owner::release() noexcept {
    void* value = value_;
    value_      = nullptr;
    return value;
}

void cec_heap_owner::reset(void* value) noexcept {
    if (value_ != nullptr) {
        HeapFree(GetProcessHeap(), 0, value_);
    }
    value_ = value;
}

cec_rio_registration_owner::cec_rio_registration_owner() noexcept : rio_(nullptr), value_(RIO_INVALID_BUFFERID) {}

cec_rio_registration_owner::cec_rio_registration_owner(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                                                       RIO_BUFFERID                        value) noexcept :
    rio_(rio),
    value_(value) {}

cec_rio_registration_owner::~cec_rio_registration_owner() noexcept {
    reset();
}

cec_rio_registration_owner::cec_rio_registration_owner(cec_rio_registration_owner&& other) noexcept :
    rio_(other.rio_),
    value_(other.release()) {
    other.rio_ = nullptr;
}

cec_rio_registration_owner& cec_rio_registration_owner::operator=(cec_rio_registration_owner&& other) noexcept {
    if (this != &other) {
        reset();
        rio_       = other.rio_;
        value_     = other.release();
        other.rio_ = nullptr;
    }
    return *this;
}

RIO_BUFFERID cec_rio_registration_owner::get() const noexcept {
    return value_;
}

RIO_BUFFERID cec_rio_registration_owner::release() noexcept {
    const RIO_BUFFERID value = value_;
    value_                   = RIO_INVALID_BUFFERID;
    return value;
}

void cec_rio_registration_owner::reset(const RIO_EXTENSION_FUNCTION_TABLE* rio, RIO_BUFFERID value) noexcept {
    if (value_ != RIO_INVALID_BUFFERID && rio_ != nullptr) {
        rio_->RIODeregisterBuffer(value_);
    }
    rio_   = rio;
    value_ = value;
}

cec_rio_cq_owner::cec_rio_cq_owner() noexcept : rio_(nullptr), value_(RIO_INVALID_CQ) {}

cec_rio_cq_owner::cec_rio_cq_owner(const RIO_EXTENSION_FUNCTION_TABLE* rio, RIO_CQ value) noexcept :
    rio_(rio),
    value_(value) {}

cec_rio_cq_owner::~cec_rio_cq_owner() noexcept {
    reset();
}

cec_rio_cq_owner::cec_rio_cq_owner(cec_rio_cq_owner&& other) noexcept : rio_(other.rio_), value_(other.release()) {
    other.rio_ = nullptr;
}

cec_rio_cq_owner& cec_rio_cq_owner::operator=(cec_rio_cq_owner&& other) noexcept {
    if (this != &other) {
        reset();
        rio_       = other.rio_;
        value_     = other.release();
        other.rio_ = nullptr;
    }
    return *this;
}

RIO_CQ cec_rio_cq_owner::get() const noexcept {
    return value_;
}

RIO_CQ cec_rio_cq_owner::release() noexcept {
    const RIO_CQ value = value_;
    value_             = RIO_INVALID_CQ;
    return value;
}

void cec_rio_cq_owner::reset(const RIO_EXTENSION_FUNCTION_TABLE* rio, RIO_CQ value) noexcept {
    if (value_ != RIO_INVALID_CQ && rio_ != nullptr) {
        rio_->RIOCloseCompletionQueue(value_);
    }
    rio_   = rio;
    value_ = value;
}

bool cec_worker_may_release(const cec_worker_lifecycle* lifecycle) noexcept {
    return lifecycle != nullptr && lifecycle->phase == cec_worker_phase::stopped && lifecycle->live_sessions == 0 &&
           lifecycle->total_outstanding == 0 && lifecycle->rio_outstanding == 0;
}

std::uint64_t cec_engine_ticks_to_microseconds(std::uint64_t ticks, std::uint64_t frequency) noexcept {
    if (frequency == 0) {
        cec_engine_fail_fast(L"client latency frequency", ERROR_INVALID_DATA);
    }
    constexpr std::uint64_t million = 1'000'000ULL;
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t     seconds = ticks / frequency;
    if (seconds > maximum / million) {
        return maximum;
    }
    const std::uint64_t whole      = seconds * million;
    const std::uint64_t remainder  = ticks % frequency;
    std::uint64_t       fractional = 0;
    if (remainder <= maximum / million) {
        fractional = remainder * million / frequency;
    } else {
        // Find floor(remainder * million / frequency) without an overflowing product.
        std::uint64_t lower = 0;
        std::uint64_t upper = million - 1ULL;
        while (lower < upper) {
            const std::uint64_t middle = lower + (upper - lower + 1ULL) / 2ULL;
            const std::uint64_t threshold =
                middle * (frequency / million) + (middle * (frequency % million) + million - 1ULL) / million;
            if (threshold <= remainder) {
                lower = middle;
            } else {
                upper = middle - 1ULL;
            }
        }
        fractional = lower;
    }
    if (fractional > maximum - whole) {
        return maximum;
    }
    return std::max<std::uint64_t>(1ULL, whole + fractional);
}

bool cec_notification_packet_matches(ULONG_PTR         key,
                                     const OVERLAPPED* overlapped,
                                     ULONG_PTR         expected_key,
                                     const OVERLAPPED* expected_overlapped) noexcept {
    return key == expected_key && overlapped == expected_overlapped;
}

bool cec_timer_initialize(cec_timer_heap* heap,
                          cec_timer_node* nodes,
                          std::uint32_t*  positions,
                          std::uint32_t   capacity,
                          ULONGLONG       ticks_per_second) noexcept {
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

ULONGLONG cec_timer_deadline_after_milliseconds(ULONGLONG now,
                                                ULONGLONG milliseconds,
                                                ULONGLONG ticks_per_second) noexcept {
    if (ticks_per_second == 0) {
        cec_engine_fail_fast(L"client timer frequency", ERROR_INVALID_DATA);
    }
    const ULONGLONG maximum = std::numeric_limits<ULONGLONG>::max();
    const ULONGLONG seconds = milliseconds / 1000ULL;
    if (seconds > (maximum - now) / ticks_per_second) {
        return maximum;
    }
    const ULONGLONG whole_ticks = seconds * ticks_per_second;
    const ULONGLONG remainder   = milliseconds % 1000ULL;
    // Split the frequency before multiplication; even very large frequencies cannot wrap.
    const ULONGLONG fractional_ticks =
        remainder * (ticks_per_second / 1000ULL) + (remainder * (ticks_per_second % 1000ULL) + 999ULL) / 1000ULL;
    if (fractional_ticks > maximum - now - whole_ticks) {
        return maximum;
    }
    return now + whole_ticks + fractional_ticks;
}

bool cec_timer_insert_or_update(cec_timer_heap* heap, std::uint32_t session_index, ULONGLONG deadline) noexcept {
    if (heap == nullptr || session_index >= heap->capacity) {
        return false;
    }
    const std::uint32_t present = heap->positions[session_index];
    if (present != cec_timer_absent) {
        const ULONGLONG previous      = heap->nodes[present].deadline;
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

bool cec_timer_pop_expired(cec_timer_heap* heap, ULONGLONG now, std::uint32_t* session_index) noexcept {
    if (heap == nullptr || session_index == nullptr || heap->size == 0 || heap->nodes[0].deadline > now) {
        return false;
    }
    *session_index = heap->nodes[0].session_index;
    return cec_timer_remove(heap, *session_index);
}

DWORD cec_timer_wait_milliseconds(const cec_timer_heap* heap, ULONGLONG now) noexcept {
    if (heap == nullptr || heap->size == 0) {
        return INFINITE;
    }
    const ULONGLONG deadline = heap->nodes[0].deadline;
    if (deadline <= now) {
        return 0;
    }
    const ULONGLONG frequency = heap->ticks_per_second;
    if (frequency == 0) {
        cec_engine_fail_fast(L"client timer frequency", ERROR_INVALID_DATA);
    }
    const ULONGLONG remaining    = deadline - now;
    const ULONGLONG seconds      = remaining / frequency;
    const ULONGLONG maximum_wait = static_cast<ULONGLONG>(INFINITE) - 1ULL;
    if (seconds > maximum_wait / 1000ULL) {
        return static_cast<DWORD>(maximum_wait);
    }
    const ULONGLONG remainder               = remaining % frequency;
    ULONGLONG       fractional_milliseconds = 0;
    if (remainder <= std::numeric_limits<ULONGLONG>::max() / 1000ULL) {
        const ULONGLONG scaled  = remainder * 1000ULL;
        fractional_milliseconds = scaled / frequency + (scaled % frequency != 0 ? 1ULL : 0ULL);
    } else {
        // Find ceil(remainder * 1000 / frequency) without forming the overflowing product.
        ULONGLONG lower = 1;
        ULONGLONG upper = 1000;
        while (lower < upper) {
            const ULONGLONG middle = lower + (upper - lower) / 2ULL;
            const ULONGLONG ticks  = middle * (frequency / 1000ULL) + middle * (frequency % 1000ULL) / 1000ULL;
            if (ticks < remainder) {
                lower = middle + 1ULL;
            } else {
                upper = middle;
            }
        }
        fractional_milliseconds = lower;
    }
    return static_cast<DWORD>(std::min(seconds * 1000ULL + fractional_milliseconds, maximum_wait));
}

void cec_fill_repeated_pattern(std::byte*       destination,
                               std::size_t      destination_size,
                               const std::byte* pattern,
                               std::size_t      pattern_size) noexcept {
    if (destination == nullptr || pattern == nullptr || destination_size == 0 || pattern_size == 0) {
        return;
    }
    std::size_t filled = std::min(destination_size, pattern_size);
    std::memcpy(destination, pattern, filled);
    while (filled < destination_size) {
        const std::size_t copy_size = std::min(filled, destination_size - filled);
        std::memcpy(destination + filled, destination, copy_size);
        filled += copy_size;
    }
}
