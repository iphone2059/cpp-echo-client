#include "cec_types.h"

#include <Windows.h>

#include <fcntl.h>
#include <io.h>

#include <array>
#include <atomic>
#include <cstdio>

static std::atomic<bool> cec_main_stop{ false };

static BOOL WINAPI cec_main_console_handler(DWORD event_type) noexcept {
    if (event_type == CTRL_C_EVENT || event_type == CTRL_BREAK_EVENT || event_type == CTRL_CLOSE_EVENT) {
        cec_main_stop.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

class cec_main_console_registration {
  public:
    explicit cec_main_console_registration(PHANDLER_ROUTINE handler) noexcept :
        handler_(handler),
        registered_(SetConsoleCtrlHandler(handler_, TRUE) != FALSE) {}

    ~cec_main_console_registration() noexcept {
        if (registered_) {
            (void) SetConsoleCtrlHandler(handler_, FALSE);
        }
    }

    cec_main_console_registration(const cec_main_console_registration&)            = delete;
    cec_main_console_registration& operator=(const cec_main_console_registration&) = delete;

    bool registered() const noexcept { return registered_; }

  private:
    PHANDLER_ROUTINE handler_;
    bool             registered_;
};

static void cec_main_help(FILE* stream) noexcept {
    std::fputs(
        "Usage: cpp-echo-client target /p tcp|udp [/r port] [/l port] [/n count] [/t seconds] [/i ms]\n       [/d "
        "text | /z bytes | /zt bytes] [/k depth] [/c sessions]\n       [/threads workers] [/w seconds] [/rc "
        "[seconds]] [/report seconds]\n       [/b bytes] [/cq capacity] [/memory bytes] [/q] [/stats] [/h]\n/n "
        "count: attempts per session; 0 means unlimited.\n/k depth: TCP requests in flight per session; UDP "
        "rejects /k.\n/threads 0: automatic workers, min(sessions, active processors, 64).\n/rc [seconds]: "
        "reconnect after failure; bare /rc means 1 second.\n/w 0: no run limit. /report 0: no periodic "
        "reports.\n/q suppresses the default final; /stats forces final; /h shows help.\n",
        stream);
}

int wmain(int argc, wchar_t** argv) {
    (void) _setmode(_fileno(stdout), _O_BINARY);
    (void) _setmode(_fileno(stderr), _O_BINARY);
    cec_options                             options{};
    std::array<wchar_t, CEC_ERROR_CAPACITY> error{};
    if (!cec_parse_options(argc, argv, &options, error.data(), error.size())) {
        char code[256]{};
        WideCharToMultiByte(CP_UTF8, 0, error.data(), -1, code, sizeof(code), nullptr, nullptr);
        std::fprintf(stderr, "Invalid arguments: %s\n", code);
        cec_main_help(stderr);
        return static_cast<int>(cec_exit_code::usage);
    }
    if (options.help) {
        cec_main_help(stdout);
        return static_cast<int>(cec_exit_code::success);
    }
    cec_main_stop.store(false, std::memory_order_release);
    cec_main_console_registration console_registration{ cec_main_console_handler };
    if (!console_registration.registered()) {
        std::fprintf(stderr, "SetConsoleCtrlHandler failed: %lu\n", GetLastError());
        return static_cast<int>(cec_exit_code::internal);
    }
    const cec_exit_code result = cec_run_client(&options, &cec_main_stop);
    return static_cast<int>(result);
}
