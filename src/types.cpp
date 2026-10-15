//! Shared value vocabulary, capacity constants and the pure arithmetic the coordinator and the
//! engine agree on. This is a C++20 named module: no header exists for it, and every consumer
//! imports cec.types.
module;

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>

export module cec.types;

static_assert(CHAR_BIT == 8);
static_assert(sizeof(wchar_t) == 2, "Windows requires 16-bit wchar_t");
#if !defined(_MSC_VER)
#    error cpp-echo-client requires MSVC
#endif
#if !defined(_MSVC_LANG) || _MSVC_LANG < 202302L
#    error cpp-echo-client requires MSVC latest C++ language mode
#endif

export inline constexpr unsigned      CEC_WIN32_TARGET              = 0x0A00U;
export inline constexpr std::uint32_t CEC_ERROR_CAPACITY            = 256U;
export inline constexpr std::size_t   CEC_HOST_CAPACITY             = 256U;
export inline constexpr std::size_t   CEC_LITERAL_CAPACITY          = 32768U;
export inline constexpr std::uint32_t CEC_DEFAULT_PORT              = 7U;
export inline constexpr std::uint64_t CEC_DEFAULT_COUNT             = 5ULL;
export inline constexpr std::uint32_t CEC_DEFAULT_TIMEOUT_SECONDS   = 5U;
export inline constexpr std::uint32_t CEC_DEFAULT_CQ_CAPACITY       = 4096U;
export inline constexpr std::uint64_t CEC_DEFAULT_MEMORY_BYTES      = 1073741824ULL;
export inline constexpr std::uint32_t CEC_MAXIMUM_TCP_BATCH_BYTES   = 67108864U;
export inline constexpr std::uint32_t CEC_MAXIMUM_UDP_PAYLOAD_BYTES = 65507U;

export inline constexpr std::uint32_t CEC_MAX_WORKERS                  = 64U;
export inline constexpr std::uint32_t CEC_RIO_MAX_OUTSTANDING_RECEIVES = 1U;
export inline constexpr std::uint32_t CEC_RIO_MAX_OUTSTANDING_SENDS    = 1U;
export inline constexpr wchar_t       CEC_DEFAULT_PATTERN_FORMAT[]     = L"echo from %ls";

export enum class cec_protocol : std::uint8_t { none = 0, tcp = 1, udp = 2 };

export enum class cec_pattern_kind : std::uint8_t {
    default_text      = 0,
    literal_text      = 1,
    binary_counter    = 2,
    printable_counter = 3
};

export enum class cec_exit_code : int { success = 0, usage = 1, network = 2, echo_failure = 3, internal = 4 };

export struct cec_options {
    cec_protocol     protocol;
    cec_pattern_kind pattern_kind;
    wchar_t          host[CEC_HOST_CAPACITY];
    wchar_t          literal_pattern[CEC_LITERAL_CAPACITY];
    std::uint16_t    remote_port;
    std::uint16_t    local_port;
    std::uint64_t    echo_count;
    std::uint32_t    timeout_seconds;
    std::uint32_t    interval_milliseconds;
    std::uint32_t    socket_buffer_bytes;
    std::uint32_t    pipeline_depth;
    std::uint32_t    pattern_bytes;
    std::uint32_t    run_seconds;
    std::int64_t     reconnect_seconds;
    std::uint32_t    report_seconds;
    std::uint32_t    session_count;
    std::uint32_t    worker_count;
    std::uint32_t    cq_capacity;
    std::uint64_t    memory_bytes;
    bool             quiet;
    bool             stats;
    bool             help;
};

// Shared value vocabulary: pure values only; no native provider object crosses this boundary.
export enum class cec_engine_operation : std::uint8_t { receive, send };
export enum class cec_engine_state : std::uint8_t { dormant, connecting, active, pacing, reconnecting, closing, done };
export enum class cec_worker_phase : std::uint8_t { starting, running, draining, stopped };
export enum class cec_rio_notify_outcome : std::uint8_t { armed, duplicate_arm, invalid };
export enum class cec_event_kind : std::uint8_t { connected, sent, received, generation_drained, transport_failure };
export enum class cec_action_kind : std::uint8_t { connect, close, drain_timeout, receive, send };
export enum class cec_completion_kind : std::uint8_t {
    connected,
    sent,
    send_progress,
    received,
    failed,
    drained,
    ignored
};

export struct cec_event {
    cec_event_kind kind;
    std::uint32_t  index;
    std::size_t    bytes = 0;
    bool           equal = false;
};

export struct cec_action {
    cec_action_kind kind;
    std::uint32_t   index;
    std::size_t     bytes = 0;
};

export struct cec_completion {
    cec_completion_kind kind;
    std::uint32_t       index;
    std::size_t         bytes = 0;
    bool                equal = false;
};

export struct cec_worker_lifecycle {
    cec_worker_phase phase;
    std::uint32_t    live_sessions;
    std::uint32_t    total_outstanding;
    std::uint32_t    rio_outstanding;
};

export std::uint32_t cec_resolved_worker_count(std::uint32_t requested,
                                               std::uint32_t processors,
                                               std::uint32_t sessions) noexcept;
export bool          cec_checked_product(std::size_t left, std::size_t right, std::size_t* product) noexcept;
export bool          cec_checked_storage_bytes(std::size_t   sessions,
                                               std::size_t   batch_bytes,
                                               std::uint64_t memory_limit,
                                               std::size_t*  bytes) noexcept;
export cec_exit_code cec_classify_result(std::uint64_t echoed,
                                         std::uint64_t corrupted,
                                         std::uint64_t lost,
                                         std::uint64_t network_errors,
                                         bool          fatal,
                                         bool          controlled_stop) noexcept;
export bool          cec_notification_mark_delivered(bool* armed) noexcept;
export bool          cec_notification_mark_rearmed(bool* armed) noexcept;
export bool          cec_notify_should_arm(bool armed, std::uint32_t outstanding) noexcept;

// ---- definitions (the former types.cpp) ----

std::uint32_t cec_resolved_worker_count(std::uint32_t requested,
                                        std::uint32_t processors,
                                        std::uint32_t sessions) noexcept {
    return std::min(requested == 0 ? std::clamp(processors, 1U, CEC_MAX_WORKERS) : requested, sessions);
}

bool cec_checked_product(std::size_t left, std::size_t right, std::size_t* product) noexcept {
    if (product == nullptr || (right != 0 && left > std::numeric_limits<std::size_t>::max() / right)) {
        return false;
    }
    *product = left * right;
    return true;
}

bool cec_checked_storage_bytes(std::size_t   sessions,
                               std::size_t   batch_bytes,
                               std::uint64_t memory_limit,
                               std::size_t*  bytes) noexcept {
    std::size_t per_session = 0;
    std::size_t result      = 0;
    if (!cec_checked_product(batch_bytes, 2, &per_session) || !cec_checked_product(sessions, per_session, &result) ||
        result > memory_limit) {
        return false;
    }
    *bytes = result;
    return true;
}

cec_exit_code cec_classify_result(std::uint64_t echoed,
                                  std::uint64_t corrupted,
                                  std::uint64_t lost,
                                  std::uint64_t network_errors,
                                  bool          fatal,
                                  bool          controlled_stop) noexcept {
    static_cast<void>(network_errors);
    if (fatal) {
        return cec_exit_code::internal;
    }
    if (corrupted != 0 || lost != 0) {
        return cec_exit_code::echo_failure;
    }
    if (controlled_stop) {
        return cec_exit_code::success;
    }
    if (echoed == 0) {
        return cec_exit_code::network;
    }
    return cec_exit_code::success;
}

bool cec_notification_mark_delivered(bool* armed) noexcept {
    if (armed == nullptr || !*armed) {
        return false;
    }
    *armed = false;
    return true;
}

bool cec_notification_mark_rearmed(bool* armed) noexcept {
    if (armed == nullptr || *armed) {
        return false;
    }
    *armed = true;
    return true;
}

bool cec_notify_should_arm(bool armed, std::uint32_t outstanding) noexcept {
    return !armed && outstanding != 0U;
}
