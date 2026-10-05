#include "cec_engine_internal.h"

#include <cwchar>

static int cec_fault_notify_status = ERROR_SUCCESS;

static int WINAPI cec_fault_rio_notify(RIO_CQ) noexcept {
    return cec_fault_notify_status;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        return 1;
    }
    if (std::wcscmp(argv[1], L"normal") == 0) {
        cec_require_rio_notify_success(ERROR_SUCCESS, L"test notify success");
        return cec_require_valid_dequeue_count(0, L"test dequeue success") == 0 ? 0 : 1;
    }
    if (std::wcscmp(argv[1], L"notify_provider_failure") == 0) {
        RIO_EXTENSION_FUNCTION_TABLE table{};
        table.RIONotify         = &cec_fault_rio_notify;
        bool          armed     = false;
        std::uint64_t arms      = 0;
        cec_fault_notify_status = WSAEINVAL;
        cec_notification_arm(&table, reinterpret_cast<RIO_CQ>(static_cast<std::uintptr_t>(1U)), &armed, &arms,
                             L"RIONotify(client provider)");
    } else if (std::wcscmp(argv[1], L"notify_provider_duplicate") == 0) {
        RIO_EXTENSION_FUNCTION_TABLE table{};
        table.RIONotify         = &cec_fault_rio_notify;
        bool          armed     = false;
        std::uint64_t arms      = 0;
        cec_fault_notify_status = WSAEALREADY;
        cec_notification_arm(&table, reinterpret_cast<RIO_CQ>(static_cast<std::uintptr_t>(1U)), &armed, &arms,
                             L"RIONotify(client provider duplicate)");
    } else if (std::wcscmp(argv[1], L"notify_precondition_duplicate") == 0) {
        RIO_EXTENSION_FUNCTION_TABLE table{};
        table.RIONotify         = &cec_fault_rio_notify;
        bool          armed     = true;
        std::uint64_t arms      = 0;
        cec_fault_notify_status = ERROR_SUCCESS;
        cec_notification_arm(&table, reinterpret_cast<RIO_CQ>(static_cast<std::uintptr_t>(1U)), &armed, &arms,
                             L"RIONotify(client duplicate precondition)");
    } else if (std::wcscmp(argv[1], L"notify_failure") == 0) {
        cec_require_rio_notify_success(WSAEINVAL, L"test notify failure");
    } else if (std::wcscmp(argv[1], L"notify_duplicate") == 0) {
        // WSAEALREADY means a previous RIONotify has not completed: the state machine armed twice,
        // so the process must report the duplicate-arm stage instead of the caller's stage.
        cec_require_rio_notify_success(WSAEALREADY, L"test notify duplicate");
    } else if (std::wcscmp(argv[1], L"corrupt_cq") == 0) {
        static_cast<void>(cec_require_valid_dequeue_count(RIO_CORRUPT_CQ, L"test corrupt CQ"));
    } else if (std::wcscmp(argv[1], L"zero_latency_frequency") == 0) {
        static_cast<void>(cec_engine_ticks_to_microseconds(1, 0));
    } else {
        return 1;
    }
    return 1;
}
