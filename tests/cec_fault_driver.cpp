#include "cec_engine_internal.h"

#include <cwchar>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
    {
        return 1;
    }
    if (std::wcscmp(argv[1], L"normal") == 0)
    {
        cec_require_rio_notify_success(ERROR_SUCCESS, L"test notify success");
        return cec_require_valid_dequeue_count(0, L"test dequeue success") == 0 ? 0 : 1;
    }
    if (std::wcscmp(argv[1], L"notify_failure") == 0)
    {
        cec_require_rio_notify_success(WSAEINVAL, L"test notify failure");
    }
    else if (std::wcscmp(argv[1], L"corrupt_cq") == 0)
    {
        static_cast<void>(cec_require_valid_dequeue_count(RIO_CORRUPT_CQ, L"test corrupt CQ"));
    }
    else
    {
        return 1;
    }
    return 1;
}
