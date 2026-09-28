#pragma once

#include <climits>

#define CEC_WIN32_TARGET 0x0A00
#define CEC_ERROR_CAPACITY 256U
#define CEC_HOST_CAPACITY 256U
#define CEC_LITERAL_CAPACITY 32768U
#define CEC_DEFAULT_PORT 7U
#define CEC_DEFAULT_COUNT 5ULL
#define CEC_DEFAULT_TIMEOUT_SECONDS 5U
#define CEC_DEFAULT_CQ_CAPACITY 4096U
#define CEC_DEFAULT_MEMORY_BYTES 1073741824ULL
#define CEC_MAXIMUM_TCP_BATCH_BYTES 67108864U
#define CEC_MAXIMUM_UDP_PAYLOAD_BYTES 65507U

static_assert(CHAR_BIT == 8);
static_assert(sizeof(wchar_t) == 2, "Windows requires 16-bit wchar_t");
#if !defined(_MSC_VER)
#error cpp-echo-client requires MSVC
#endif
#if !defined(_MSVC_LANG) || _MSVC_LANG < 202302L
#error cpp-echo-client requires MSVC latest C++ language mode
#endif
