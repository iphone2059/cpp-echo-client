#pragma once

#include "cec_compiler_contract.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

enum class cec_protocol : std::uint8_t
{
    none = 0,
    tcp = 1,
    udp = 2
};

enum class cec_pattern_kind : std::uint8_t
{
    default_text = 0,
    literal_text = 1,
    binary_counter = 2,
    printable_counter = 3
};

enum class cec_exit_code : int
{
    success = 0,
    usage = 1,
    network = 2,
    echo_failure = 3,
    internal = 4
};

struct cec_options
{
    cec_protocol protocol;
    cec_pattern_kind pattern_kind;
    wchar_t host[CEC_HOST_CAPACITY];
    wchar_t literal_pattern[CEC_LITERAL_CAPACITY];
    std::uint16_t remote_port;
    std::uint16_t local_port;
    std::uint64_t echo_count;
    std::uint32_t timeout_seconds;
    std::uint32_t interval_milliseconds;
    std::uint32_t socket_buffer_bytes;
    std::uint32_t pipeline_depth;
    std::uint32_t pattern_bytes;
    std::uint32_t run_seconds;
    std::int32_t reconnect_seconds;
    std::uint32_t report_seconds;
    std::uint32_t session_count;
    std::uint32_t worker_count;
    std::uint32_t cq_capacity;
    std::uint64_t memory_bytes;
    bool quiet;
    bool stats;
    bool help;
};

bool cec_parse_options(int argc, wchar_t* const* argv, cec_options* options, wchar_t* error,
                       std::size_t error_capacity) noexcept;
bool cec_checked_product(std::size_t left, std::size_t right, std::size_t* product) noexcept;
bool cec_checked_storage_bytes(std::size_t sessions, std::size_t batch_bytes, std::uint64_t memory_limit,
                               std::size_t* bytes) noexcept;
void cec_fill_binary_pattern(std::byte* output, std::size_t size) noexcept;
void cec_fill_printable_pattern(std::byte* output, std::size_t size) noexcept;
std::uint64_t cec_claim_attempts(std::atomic<std::uint64_t>* claimed, std::uint64_t limit,
                                 std::uint64_t requested) noexcept;
std::uint64_t cec_unclaimed_echoes(std::uint64_t limit, std::uint64_t claimed, bool controlled_stop) noexcept;
std::uint64_t cec_percentile_target(std::uint64_t total, std::uint64_t numerator, std::uint64_t denominator) noexcept;
bool cec_notification_mark_delivered(bool* armed) noexcept;
bool cec_notification_mark_rearmed(bool* armed) noexcept;
cec_exit_code cec_classify_result(std::uint64_t echoed, std::uint64_t corrupted, std::uint64_t lost,
                                  std::uint64_t network_errors, bool fatal, bool controlled_stop) noexcept;
cec_exit_code cec_run_client(const cec_options* options, std::atomic<bool>* stop_requested) noexcept;
