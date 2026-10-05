#pragma once

#include <cstdint>

// Finite-attempt test model: every claim has a result. Controlled stop may leave claims unfinished.
constexpr bool cec_test_terminal_accounting_model_valid(std::uint64_t claimed,
                                                        std::uint64_t echoed,
                                                        std::uint64_t corrupted,
                                                        std::uint64_t lost) noexcept {
    return echoed <= claimed && corrupted <= claimed - echoed && lost == claimed - echoed - corrupted;
}
