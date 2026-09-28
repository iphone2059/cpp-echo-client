#include "cec_types.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdio>

static std::atomic<bool> cec_main_stop{false};

static BOOL WINAPI cec_main_console_handler(DWORD event_type) noexcept
{
    if (event_type == CTRL_C_EVENT || event_type == CTRL_BREAK_EVENT || event_type == CTRL_CLOSE_EVENT)
    {
        cec_main_stop.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

class cec_main_console_registration
{
  public:
    explicit cec_main_console_registration(PHANDLER_ROUTINE handler) noexcept
        : handler_(handler), registered_(SetConsoleCtrlHandler(handler_, TRUE) != FALSE)
    {
    }

    ~cec_main_console_registration() noexcept
    {
        if (registered_)
        {
            (void)SetConsoleCtrlHandler(handler_, FALSE);
        }
    }

    cec_main_console_registration(const cec_main_console_registration&) = delete;
    cec_main_console_registration& operator=(const cec_main_console_registration&) = delete;
    bool registered() const noexcept
    {
        return registered_;
    }

  private:
    PHANDLER_ROUTINE handler_;
    bool registered_;
};

static void cec_main_help() noexcept
{
    std::fputws(L"Usage: cpp-echo-client target /p tcp|udp [/r port] [/l port] [/n count]\n", stdout);
    std::fputws(L"       [/t seconds] [/i ms] [/d text | /z bytes | /zt bytes] [/k tcp-depth]\n", stdout);
    std::fputws(L"       [/c sessions] [/threads workers] [/w seconds] [/rc [seconds]]\n", stdout);
    std::fputws(L"       [/report seconds] [/b bytes] [/cq capacity] [/memory bytes] [/q] [/stats]\n", stdout);
    std::fputws(L"Data I/O is always RIO; CQ notification is always IOCP. No fallback backend exists.\n", stdout);
}

int wmain(int argc, wchar_t** argv)
{
    cec_options options{};
    std::array<wchar_t, CEC_ERROR_CAPACITY> error{};
    if (!cec_parse_options(argc, argv, &options, error.data(), error.size()))
    {
        std::fwprintf(stderr, L"Invalid arguments: %ls\n", error.data());
        cec_main_help();
        return static_cast<int>(cec_exit_code::usage);
    }
    if (options.help)
    {
        cec_main_help();
        return static_cast<int>(cec_exit_code::success);
    }
    cec_main_stop.store(false, std::memory_order_release);
    cec_main_console_registration console_registration{cec_main_console_handler};
    if (!console_registration.registered())
    {
        std::fwprintf(stderr, L"SetConsoleCtrlHandler failed: %lu\n", GetLastError());
        return static_cast<int>(cec_exit_code::internal);
    }
    const cec_exit_code result = cec_run_client(&options, &cec_main_stop);
    return static_cast<int>(result);
}
