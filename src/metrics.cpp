//! Attempt accounting, the latency histogram and the reference report line. A C++20 named module:
//! consumers import cec.metrics, and the exported surface is exactly the former metrics.h.
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <limits>

export module cec.metrics;

export import cec.types;

export struct cec_engine_metrics {
    std::atomic<std::uint64_t>                 claimed;
    std::atomic<std::uint64_t>                 echoed;
    std::atomic<std::uint64_t>                 corrupted;
    std::atomic<std::uint64_t>                 lost;
    // Attempts the operator stopped before they could complete, and attempts that a terminal
    // failure ended the run before it could post. Together with the claims they close the
    // attempted = pending + echoed + corrupted + lost + cancelled identity.
    std::atomic<std::uint64_t>                 cancelled;
    std::atomic<std::uint64_t>                 terminal_unclaimed;
    std::atomic<std::uint64_t>                 bytes;
    std::atomic<std::uint64_t>                 sent_bytes;
    std::atomic<std::uint64_t>                 received_bytes;
    std::atomic<std::uint64_t>                 connections;
    std::atomic<std::uint64_t>                 reconnects;
    std::atomic<std::uint64_t>                 active;
    std::atomic<std::uint64_t>                 network_errors;
    std::atomic<std::uint64_t>                 latency_sum_us;
    std::atomic<std::uint64_t>                 latency_samples;
    std::array<std::atomic<std::uint64_t>, 64> latency_bins;
};

export std::uint64_t cec_claim_attempts(std::atomic<std::uint64_t>* claimed,
                                        std::uint64_t               limit,
                                        std::uint64_t               requested) noexcept;
export std::uint64_t cec_unclaimed_echoes(std::uint64_t limit, std::uint64_t claimed, bool controlled_stop) noexcept;
export std::uint64_t cec_percentile_target(std::uint64_t total,
                                           std::uint64_t numerator,
                                           std::uint64_t denominator) noexcept;
export std::uint64_t cec_engine_ticks_to_microseconds(std::uint64_t ticks, std::uint64_t frequency) noexcept;

export constexpr std::uint64_t cec_latency_bin_lower_bound(unsigned index) noexcept {
    return 1ULL << (index < 63U ? index : 63U);
}

export void cec_engine_record_latency(cec_engine_metrics* metrics,
                                      std::uint64_t       ticks,
                                      std::uint64_t       frequency) noexcept;
export void cec_engine_print_metrics(const char*               phase,
                                     const cec_options*        options,
                                     const cec_engine_metrics* metrics,
                                     std::uint64_t             elapsed_milliseconds) noexcept;

// ---- definitions (the former metrics.cpp) ----

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

std::uint64_t cec_engine_ticks_to_microseconds(std::uint64_t ticks, std::uint64_t frequency) noexcept {
    if (frequency == 0) {
        return std::numeric_limits<std::uint64_t>::max();
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

void cec_engine_record_latency(cec_engine_metrics* metrics, std::uint64_t ticks, std::uint64_t frequency) noexcept {
    const std::uint64_t microseconds = cec_engine_ticks_to_microseconds(ticks, frequency);
    const unsigned      bin          = std::min<unsigned>(std::bit_width(microseconds) - 1U, 63U);
    metrics->latency_bins[bin].fetch_add(1, std::memory_order_relaxed);
    metrics->latency_sum_us.fetch_add(microseconds, std::memory_order_relaxed);
    metrics->latency_samples.fetch_add(1, std::memory_order_relaxed);
}

static std::uint64_t cec_engine_percentile(const cec_engine_metrics* metrics,
                                           std::uint64_t             total,
                                           std::uint64_t             numerator,
                                           std::uint64_t             denominator) noexcept {
    if (total == 0) {
        return 0;
    }
    const std::uint64_t target     = cec_percentile_target(total, numerator, denominator);
    std::uint64_t       cumulative = 0;
    for (unsigned index = 0; index < metrics->latency_bins.size(); ++index) {
        cumulative += metrics->latency_bins[index].load(std::memory_order_relaxed);
        if (cumulative >= target) {
            return cec_latency_bin_lower_bound(index);
        }
    }
    return 0;
}

static std::uint64_t cec_engine_sample_count(const cec_engine_metrics* metrics) noexcept {
    std::uint64_t count = 0;
    for (const std::atomic<std::uint64_t>& bin : metrics->latency_bins) {
        count += bin.load(std::memory_order_relaxed);
    }
    return count;
}

void cec_engine_print_metrics(const char*               phase,
                              const cec_options*        options,
                              const cec_engine_metrics* metrics,
                              std::uint64_t             elapsed_milliseconds) noexcept {
    const std::uint64_t attempted =
        metrics->claimed.load(std::memory_order_relaxed) + metrics->terminal_unclaimed.load(std::memory_order_relaxed);
    const std::uint64_t echoed          = metrics->echoed.load(std::memory_order_relaxed);
    const std::uint64_t corrupted       = metrics->corrupted.load(std::memory_order_relaxed);
    const std::uint64_t lost            = metrics->lost.load(std::memory_order_relaxed);
    const std::uint64_t cancelled       = metrics->cancelled.load(std::memory_order_relaxed);
    const std::uint64_t bytes           = metrics->bytes.load(std::memory_order_relaxed);
    const std::uint64_t samples         = cec_engine_sample_count(metrics);
    const std::uint64_t latency_samples = metrics->latency_samples.load(std::memory_order_relaxed);
    const std::uint64_t settled         = echoed + corrupted + lost + cancelled;
    // attempted = pending + echoed + corrupted + lost + cancelled; a clamped difference keeps the
    // report meaningful if a counter ever moves ahead of the attempts it belongs to.
    const std::uint64_t pending         = attempted > settled ? attempted - settled : 0U;
    const double elapsed_seconds      = static_cast<double>(std::max<std::uint64_t>(elapsed_milliseconds, 1U)) / 1000.0;
    const double echoes_per_second    = static_cast<double>(echoed) / elapsed_seconds;
    const double mebibytes_per_second = static_cast<double>(bytes) / (1024.0 * 1024.0) / elapsed_seconds;
    const std::uint64_t mean_us =
        latency_samples == 0U ? 0U : metrics->latency_sum_us.load(std::memory_order_relaxed) / latency_samples;
    std::printf(
        "%s elapsed_ms=%llu sessions=%u active=%llu attempted=%llu pending=%llu echoed=%llu corrupted=%llu lost=%llu "
        "cancelled=%llu sent_bytes=%llu received_bytes=%llu bytes=%llu connections=%llu reconnects=%llu "
        "network_errors=%llu echo_per_sec=%.2f MiB_per_sec=%.2f p50_us~%llu p99_us~%llu p999_us~%llu mean_us=%llu "
        "max_us~%llu latency_sample=batch\n",
        phase, static_cast<unsigned long long>(elapsed_milliseconds), options->session_count,
        static_cast<unsigned long long>(metrics->active.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(attempted), static_cast<unsigned long long>(pending),
        static_cast<unsigned long long>(echoed), static_cast<unsigned long long>(corrupted),
        static_cast<unsigned long long>(lost), static_cast<unsigned long long>(cancelled),
        static_cast<unsigned long long>(metrics->sent_bytes.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(metrics->received_bytes.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(bytes),
        static_cast<unsigned long long>(metrics->connections.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(metrics->reconnects.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(metrics->network_errors.load(std::memory_order_relaxed)), echoes_per_second,
        mebibytes_per_second, static_cast<unsigned long long>(cec_engine_percentile(metrics, samples, 50, 100)),
        static_cast<unsigned long long>(cec_engine_percentile(metrics, samples, 99, 100)),
        static_cast<unsigned long long>(cec_engine_percentile(metrics, samples, 999, 1000)),
        static_cast<unsigned long long>(mean_us),
        static_cast<unsigned long long>(cec_engine_percentile(metrics, samples, 1, 1)));
}
