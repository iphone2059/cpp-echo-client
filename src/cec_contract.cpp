#include "cec_types.h"
#include <Windows.h>
#include <string>

#include <algorithm>
#include <cwchar>
#include <limits>
#include <string_view>

static bool cec_contract_equal(std::wstring_view left, std::wstring_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const wchar_t left_character =
            left[index] >= L'A' && left[index] <= L'Z' ? left[index] + (L'a' - L'A') : left[index];
        const wchar_t right_character =
            right[index] >= L'A' && right[index] <= L'Z' ? right[index] + (L'a' - L'A') : right[index];
        if (left_character != right_character) {
            return false;
        }
    }
    return true;
}

static bool cec_contract_is_switch(std::wstring_view token) noexcept {
    if (token.size() < 2 || (token[0] != L'/' && token[0] != L'-')) {
        return false;
    }
    const std::size_t offset = token.size() > 2 && token[0] == L'-' && token[1] == L'-' ? 2 : 1;
    if (offset >= token.size()) {
        return false;
    }
    const wchar_t first = token[offset];
    return (first >= L'A' && first <= L'Z') || (first >= L'a' && first <= L'z');
}

static bool cec_contract_is_value_switch(std::wstring_view name) noexcept {
    return cec_contract_equal(name, L"p") || cec_contract_equal(name, L"d") || cec_contract_equal(name, L"r") ||
           cec_contract_equal(name, L"l") || cec_contract_equal(name, L"n") || cec_contract_equal(name, L"t") ||
           cec_contract_equal(name, L"i") || cec_contract_equal(name, L"b") || cec_contract_equal(name, L"k") ||
           cec_contract_equal(name, L"z") || cec_contract_equal(name, L"zt") || cec_contract_equal(name, L"w") ||
           cec_contract_equal(name, L"rc") || cec_contract_equal(name, L"report") || cec_contract_equal(name, L"c") ||
           cec_contract_equal(name, L"threads") || cec_contract_equal(name, L"cq") ||
           cec_contract_equal(name, L"memory");
}

static void cec_contract_error(wchar_t* output, std::size_t capacity, const wchar_t* message) noexcept {
    if (output == nullptr || capacity == 0) {
        return;
    }
    output[0] = L'\0';
    if (message != nullptr) {
        wcsncpy_s(output, capacity, message, _TRUNCATE);
    }
}

static bool cec_contract_number(std::wstring_view text, std::uint64_t* value) noexcept {
    if (text.empty() || value == nullptr) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - L'0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    *value = parsed;
    return true;
}

static bool cec_contract_value(int                argc,
                               wchar_t* const*    argv,
                               int*               index,
                               std::wstring_view  inline_value,
                               std::wstring_view* value) noexcept {
    if (!inline_value.empty()) {
        *value = inline_value;
        return true;
    }
    if (*index + 1 >= argc) {
        return false;
    }
    const std::wstring_view next{ argv[*index + 1] };
    if (cec_contract_is_switch(next)) {
        return false;
    }
    ++*index;
    *value = next;
    return !value->empty();
}

static bool cec_contract_copy(std::wstring_view value, wchar_t* output, std::size_t capacity) noexcept {
    if (value.empty() || value.size() >= capacity) {
        return false;
    }
    std::wmemcpy(output, value.data(), value.size());
    output[value.size()] = L'\0';
    return true;
}

bool cec_parse_options(int             argc,
                       wchar_t* const* argv,
                       cec_options*    options,
                       wchar_t*        error,
                       std::size_t     error_capacity) noexcept {
    if (argc < 1 || argv == nullptr || options == nullptr) {
        cec_contract_error(error, error_capacity, L"invalid parser arguments");
        return false;
    }
    *options           = cec_options{ cec_protocol::none,
                                      cec_pattern_kind::default_text,
                                      {},
                                      {},
                                      static_cast<std::uint16_t>(CEC_DEFAULT_PORT),
                                      0,
                                      CEC_DEFAULT_COUNT,
                                      CEC_DEFAULT_TIMEOUT_SECONDS,
                                      0,
                                      0,
                                      1,
                                      0,
                                      0,
                                      -1,
                                      0,
                                      1,
                                      0,
                                      CEC_DEFAULT_CQ_CAPACITY,
                                      CEC_DEFAULT_MEMORY_BYTES,
                                      false,
                                      false,
                                      false };
    bool saw_host      = false;
    bool saw_pipeline  = false;
    bool saw_literal   = false;
    bool saw_binary    = false;
    bool saw_printable = false;

    for (int index = 1; index < argc; ++index) {
        const std::wstring_view token{ argv[index] };
        if (!cec_contract_is_switch(token)) {
            if (saw_host || !cec_contract_copy(token, options->host, CEC_HOST_CAPACITY)) {
                cec_contract_error(error, error_capacity, L"client requires exactly one valid target host");
                return false;
            }
            saw_host = true;
            continue;
        }

        std::size_t             offset    = token[0] == L'-' && token.size() > 1 && token[1] == L'-' ? 2U : 1U;
        std::wstring_view       body      = token.substr(offset);
        const std::size_t       separator = body.find(L'=');
        const std::wstring_view name      = body.substr(0, separator);
        const std::wstring_view inline_value =
            separator == std::wstring_view::npos ? std::wstring_view{} : body.substr(separator + 1);
        if (separator != std::wstring_view::npos && inline_value.empty()) {
            cec_contract_error(error, error_capacity, L"switch requires a non-empty inline value");
            return false;
        }

        if (cec_contract_equal(name, L"q") || cec_contract_equal(name, L"quiet") ||
            cec_contract_equal(name, L"stats") || cec_contract_equal(name, L"h") || cec_contract_equal(name, L"help")) {
            if (separator != std::wstring_view::npos) {
                cec_contract_error(error, error_capacity, L"flag switch does not accept a value");
                return false;
            }
            options->quiet = options->quiet || cec_contract_equal(name, L"q") || cec_contract_equal(name, L"quiet");
            options->stats = options->stats || cec_contract_equal(name, L"stats");
            options->help  = options->help || cec_contract_equal(name, L"h") || cec_contract_equal(name, L"help");
            continue;
        }

        if (cec_contract_equal(name, L"rc") && separator == std::wstring_view::npos &&
            (index + 1 >= argc || cec_contract_is_switch(std::wstring_view{ argv[index + 1] }))) {
            options->reconnect_seconds = 1;
            continue;
        }

        if (!cec_contract_is_value_switch(name)) {
            cec_contract_error(error, error_capacity, L"unknown switch");
            return false;
        }

        std::wstring_view value{};
        if (!cec_contract_value(argc, argv, &index, inline_value, &value)) {
            cec_contract_error(error, error_capacity, L"switch requires a non-empty value");
            return false;
        }
        std::uint64_t number = 0;
        if (cec_contract_equal(name, L"p")) {
            if (cec_contract_equal(value, L"tcp")) {
                options->protocol = cec_protocol::tcp;
            } else if (cec_contract_equal(value, L"udp")) {
                options->protocol = cec_protocol::udp;
            } else {
                cec_contract_error(error, error_capacity, L"/p requires tcp or udp");
                return false;
            }
        } else if (cec_contract_equal(name, L"d")) {
            if (!cec_contract_copy(value, options->literal_pattern, CEC_LITERAL_CAPACITY)) {
                cec_contract_error(error, error_capacity, L"literal text exceeds the Windows command-line limit");
                return false;
            }
            options->pattern_kind = cec_pattern_kind::literal_text;
            saw_literal           = true;
        } else if (!cec_contract_number(value, &number)) {
            cec_contract_error(error, error_capacity, L"numeric switch has an invalid value");
            return false;
        } else if (cec_contract_equal(name, L"r") && number >= 1 && number <= 65535) {
            options->remote_port = static_cast<std::uint16_t>(number);
        } else if (cec_contract_equal(name, L"l") && number <= 65535) {
            options->local_port = static_cast<std::uint16_t>(number);
        } else if (cec_contract_equal(name, L"n")) {
            options->echo_count = number;
        } else if (cec_contract_equal(name, L"t") && number >= 1 && number <= UINT32_MAX) {
            options->timeout_seconds = static_cast<std::uint32_t>(number);
        } else if (cec_contract_equal(name, L"i") && number <= UINT32_MAX) {
            options->interval_milliseconds = static_cast<std::uint32_t>(number);
        } else if (cec_contract_equal(name, L"b") && number <= INT32_MAX) {
            options->socket_buffer_bytes = static_cast<std::uint32_t>(number);
        } else if (cec_contract_equal(name, L"k") && number >= 1 && number <= UINT32_MAX) {
            options->pipeline_depth = static_cast<std::uint32_t>(number);
            saw_pipeline            = true;
        } else if (cec_contract_equal(name, L"z") && number >= 1 && number <= CEC_MAXIMUM_TCP_BATCH_BYTES) {
            options->pattern_kind  = cec_pattern_kind::binary_counter;
            options->pattern_bytes = static_cast<std::uint32_t>(number);
            saw_binary             = true;
        } else if (cec_contract_equal(name, L"zt") && number >= 1 && number <= CEC_MAXIMUM_TCP_BATCH_BYTES) {
            options->pattern_kind  = cec_pattern_kind::printable_counter;
            options->pattern_bytes = static_cast<std::uint32_t>(number);
            saw_printable          = true;
        } else if (cec_contract_equal(name, L"w") && number >= 1 && number <= UINT32_MAX) {
            options->run_seconds = static_cast<std::uint32_t>(number);
        } else if (cec_contract_equal(name, L"rc") && number <= INT32_MAX) {
            options->reconnect_seconds = static_cast<std::int32_t>(number);
        } else if (cec_contract_equal(name, L"report") && number >= 1 && number <= UINT32_MAX) {
            options->report_seconds = static_cast<std::uint32_t>(number);
        } else if (cec_contract_equal(name, L"c") && number >= 1 && number <= 1048576) {
            options->session_count = static_cast<std::uint32_t>(number);
        } else if (cec_contract_equal(name, L"threads") && number <= 64) {
            options->worker_count = static_cast<std::uint32_t>(number);
        } else if (cec_contract_equal(name, L"cq") && number >= 64 && number <= 1048576) {
            options->cq_capacity = static_cast<std::uint32_t>(number);
        } else if (cec_contract_equal(name, L"memory") && number >= 1048576) {
            options->memory_bytes = number;
        } else {
            cec_contract_error(error, error_capacity, L"unknown switch or value outside its valid range");
            return false;
        }
    }

    if (options->local_port != 0 && options->session_count != 1) {
        cec_contract_error(error, error_capacity, L"a fixed /l port requires /c 1");
        return false;
    }
    if (options->echo_count > std::numeric_limits<std::uint64_t>::max() / options->session_count) {
        cec_contract_error(error, error_capacity, L"echo count multiplied by sessions exceeds the finite quota limit");
        return false;
    }
    if (options->help) {
        return true;
    }
    if (!saw_host || options->protocol == cec_protocol::none) {
        cec_contract_error(error, error_capacity, L"target host and /p tcp or /p udp are required");
        return false;
    }
    if (static_cast<unsigned>(saw_literal) + static_cast<unsigned>(saw_binary) + static_cast<unsigned>(saw_printable) >
        1U) {
        cec_contract_error(error, error_capacity, L"use exactly one of /d, /z, or /zt");
        return false;
    }
    if (options->protocol == cec_protocol::udp && saw_pipeline) {
        cec_contract_error(error, error_capacity, L"/k is available only for TCP");
        return false;
    }
    if (options->protocol == cec_protocol::tcp && options->reconnect_seconds >= 0 && options->local_port != 0) {
        cec_contract_error(error, error_capacity, L"TCP reconnect cannot use a fixed /l port");
        return false;
    }
    if (options->protocol == cec_protocol::udp && options->pattern_bytes > CEC_MAXIMUM_UDP_PAYLOAD_BYTES) {
        cec_contract_error(error, error_capacity, L"UDP payload must not exceed 65507 bytes");
        return false;
    }
    if (options->pattern_bytes != 0) {
        std::size_t batch_bytes = 0;
        if (!cec_checked_product(options->pattern_bytes, options->pipeline_depth, &batch_bytes) ||
            batch_bytes > CEC_MAXIMUM_TCP_BATCH_BYTES) {
            cec_contract_error(error, error_capacity, L"TCP payload multiplied by depth must not exceed 64 MiB");
            return false;
        }
        std::size_t storage_bytes = 0;
        if (!cec_checked_storage_bytes(options->session_count, batch_bytes, options->memory_bytes, &storage_bytes)) {
            // The helper reports failure without writing its output parameter, so the
            // required size is recomputed here with the same arithmetic it uses, purely so
            // the message carries the same number as the Rust and Swift clients.
            const std::size_t  required_storage = batch_bytes * static_cast<std::size_t>(options->session_count) * 2;
            const std::wstring memory_message   = L"registered memory needs " + std::to_wstring(required_storage) +
                                                  L" bytes but /memory is " + std::to_wstring(options->memory_bytes);
            cec_contract_error(error, error_capacity, memory_message.c_str());
            return false;
        }
    }
    // Each worker has its own CQ. Check the largest shard, including its remainder.
    const std::uint32_t worker_count        = cec_resolve_worker_count(options);
    const std::uint32_t worker_sessions     = (options->session_count + worker_count - 1U) / worker_count;
    std::size_t         reserved_operations = 0;
    if (!cec_checked_product(static_cast<std::size_t>(options->pipeline_depth) + 1U, worker_sessions,
                             &reserved_operations) ||
        reserved_operations > options->cq_capacity) {
        const std::wstring cq_message = L"completion queue holds " + std::to_wstring(options->cq_capacity) +
                                        L" entries but a worker with " + std::to_wstring(worker_sessions) +
                                        L" sessions reserves " + std::to_wstring(reserved_operations);
        cec_contract_error(error, error_capacity, cq_message.c_str());
        return false;
    }
    return true;
}

std::uint32_t cec_resolve_worker_count(const cec_options* options) noexcept {
    std::uint32_t workers = options->worker_count;
    if (workers == 0) {
        workers = std::clamp(static_cast<std::uint32_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)), 1U, 32U);
    }
    return std::min(workers, options->session_count);
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

void cec_fill_binary_pattern(std::byte* output, std::size_t size) noexcept {
    if (output == nullptr) {
        return;
    }
    for (std::size_t index = 0; index < size; ++index) {
        output[index] = static_cast<std::byte>(index & 0xFFU);
    }
}

void cec_fill_printable_pattern(std::byte* output, std::size_t size) noexcept {
    if (output == nullptr) {
        return;
    }
    for (std::size_t index = 0; index < size; ++index) {
        const std::size_t record_offset = index % 9U;
        if (record_offset == 8U) {
            output[index] = std::byte{ ' ' };
            continue;
        }
        std::size_t record  = index / 9U;
        std::size_t divisor = 10000000U;
        for (std::size_t digit = 0; digit < record_offset; ++digit) {
            divisor /= 10U;
        }
        output[index] = static_cast<std::byte>('0' + static_cast<unsigned>((record / divisor) % 10U));
    }
}

std::uint64_t cec_claim_attempts(std::atomic<std::uint64_t>* claimed,
                                 std::uint64_t               limit,
                                 std::uint64_t               requested) noexcept {
    if (claimed == nullptr || requested == 0) {
        return 0;
    }
    if (limit == 0) {
        claimed->fetch_add(requested, std::memory_order_relaxed);
        return requested;
    }
    std::uint64_t observed = claimed->load(std::memory_order_relaxed);
    for (;;) {
        if (observed >= limit) {
            return 0;
        }
        const std::uint64_t granted = std::min(requested, limit - observed);
        if (claimed->compare_exchange_weak(observed, observed + granted, std::memory_order_relaxed,
                                           std::memory_order_relaxed)) {
            return granted;
        }
    }
}

std::uint64_t cec_unclaimed_echoes(std::uint64_t limit, std::uint64_t claimed, bool controlled_stop) noexcept {
    if (controlled_stop || limit == 0 || claimed >= limit) {
        return 0;
    }
    return limit - claimed;
}

std::uint64_t cec_percentile_target(std::uint64_t total, std::uint64_t numerator, std::uint64_t denominator) noexcept {
    if (total == 0 || numerator == 0 || denominator == 0 || numerator > denominator) {
        return 0;
    }
    const std::uint64_t quotient  = total / denominator;
    const std::uint64_t remainder = total % denominator;
    return quotient * numerator + (remainder * numerator + denominator - 1U) / denominator;
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
