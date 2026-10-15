//! Payload generation for the default, literal, binary and printable patterns. A C++20 named
//! module: consumers import cec.payload, and the exported surface is exactly the former payload.h.
module;

// clang-format off
// The SDK headers are order sensitive: WinSock2 before MSWSock and WS2tcpip, then
// Windows.h and mmsystem.h. clang-format must not reorder this block.
#include <Windows.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <memory>
#include <new>

export module cec.payload;

export import cec.types;

export void                         cec_fill_binary_pattern(std::byte* output, std::size_t size) noexcept;
export void                         cec_fill_printable_pattern(std::byte* output, std::size_t size) noexcept;
export void                         cec_fill_repeated_pattern(std::byte*       destination,
                                                              std::size_t      destination_size,
                                                              const std::byte* pattern,
                                                              std::size_t      pattern_size) noexcept;
export std::unique_ptr<std::byte[]> cec_build_pattern(const cec_options* options, std::size_t* size) noexcept;

// ---- definitions (the former payload.cpp) ----

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

static std::byte* cec_build_pattern_storage(const cec_options*            options,
                                            std::size_t*                  size,
                                            std::unique_ptr<std::byte[]>* owner) noexcept {
    if (options->pattern_kind == cec_pattern_kind::binary_counter ||
        options->pattern_kind == cec_pattern_kind::printable_counter) {
        *size              = options->pattern_bytes;
        std::byte* pattern = new (std::nothrow) std::byte[*size];
        owner->reset(pattern);
        if (pattern == nullptr) {
            return nullptr;
        }
        if (options->pattern_kind == cec_pattern_kind::binary_counter) {
            cec_fill_binary_pattern(pattern, *size);
        } else {
            cec_fill_printable_pattern(pattern, *size);
        }
        return pattern;
    }

    std::array<wchar_t, 512> default_pattern{};
    const wchar_t*           text = options->literal_pattern;
    if (text[0] == L'\0') {
        _snwprintf_s(default_pattern.data(), default_pattern.size(), _TRUNCATE, CEC_DEFAULT_PATTERN_FORMAT,
                     options->host);
        text = default_pattern.data();
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return nullptr;
    }
    *size              = static_cast<std::size_t>(bytes - 1);
    std::byte* pattern = new (std::nothrow) std::byte[static_cast<std::size_t>(bytes)];
    owner->reset(pattern);
    if (pattern == nullptr || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                                                  reinterpret_cast<char*>(pattern), bytes, nullptr, nullptr) != bytes) {
        owner->reset();
        return nullptr;
    }
    return pattern;
}

std::unique_ptr<std::byte[]> cec_build_pattern(const cec_options* options, std::size_t* size) noexcept {
    std::unique_ptr<std::byte[]> owner;
    (void) cec_build_pattern_storage(options, size, &owner);
    return owner;
}
