#pragma once

#include <cstdint>

// v1 terminal accounting model: every attempt the run posted, plus the quota a terminal failure
// never posted, must settle as echoed, corrupted, lost or cancelled.
constexpr bool cec_test_terminal_accounting_model_valid(std::uint64_t claimed,
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
