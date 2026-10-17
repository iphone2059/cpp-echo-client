// clang-format off
// The SDK headers are order sensitive: WinSock2 before MSWSock and WS2tcpip, then
// Windows.h and mmsystem.h. clang-format must not reorder this block.
#include <WinSock2.h>
#include <MSWSock.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <mmsystem.h>
// clang-format on

#include <array>
#include <cstdint>
#include <cwchar>
#include <memory>

import cec.engine;
import cec.internal;
import cec.native;
import cec.payload;

static int cec_fault_notify_status = ERROR_SUCCESS;

static int WINAPI cec_fault_rio_notify(RIO_CQ) noexcept {
    return cec_fault_notify_status;
}

int cec_fault_driver_main(int argc, wchar_t** argv) {
    if (argc != 2) {
        return 1;
    }
    if (std::wcscmp(argv[1], L"normal") == 0) {
        cec_require_rio_notify_success(ERROR_SUCCESS, L"test notify success");
        return cec_require_valid_dequeue_count(0, L"test dequeue success") == 0 ? 0 : 1;
    }
    if (std::wcscmp(argv[1], L"resource_deinit_live") == 0) {
        // The slot is on the stack: freeing before the internal guard cannot produce its named boundary.
        cec_native_slot slot{};
        slot.outstanding         = 1;
        slot.connect_outstanding = true;
        cec_native_worker native{};
        native.slots         = &slot;
        native.session_count = 1;
        cec_engine_worker_resources resources{};
        resources.owner = &native;
        resources.slots.reset(&slot);
        return 1;
    }
    if (std::wcscmp(argv[1], L"resource_deinit_drained") == 0) {
        cec_native_worker native{};
        void*             arena = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (arena == nullptr) {
            return 1;
        }
        {
            cec_engine_worker_resources resources{};
            resources.owner = &native;
            resources.arena.reset(arena);
        }
        MEMORY_BASIC_INFORMATION information{};
        return VirtualQuery(arena, &information, sizeof(information)) != 0 && information.State == MEM_FREE ? 0 : 1;
    }
    if (std::wcscmp(argv[1], L"transport_deinit_live") == 0) {
        cec_native_slot slot{};
        slot.outstanding         = 1;
        slot.connect_outstanding = true;
        cec_rio_transport transport{};
        transport.native.slots         = &slot;
        transport.native.session_count = 1;
        return 1;
    }
    if (std::wcscmp(argv[1], L"transport_deinit_drained") == 0) {
        cec_rio_transport transport{};
        return 0;
    }
    if (std::wcscmp(argv[1], L"transport_deinit_notify") == 0) {
        cec_rio_transport transport{};
        transport.native.port               = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(1));
        transport.native.notification_armed = true;
        return 1;
    }
    if (std::wcscmp(argv[1], L"completion_extent") == 0 || std::wcscmp(argv[1], L"send_offset_underflow") == 0 ||
        std::wcscmp(argv[1], L"completion_valid") == 0 || std::wcscmp(argv[1], L"completion_socket_context") == 0) {
        const bool        valid              = std::wcscmp(argv[1], L"completion_valid") == 0;
        const bool        bad_offset         = std::wcscmp(argv[1], L"send_offset_underflow") == 0;
        const bool        bad_socket_context = std::wcscmp(argv[1], L"completion_socket_context") == 0;
        cec_native_worker native{};
        cec_native_slot   slot{};
        native.slots                    = &slot;
        native.session_count            = 1;
        native.rio_outstanding          = 1;
        slot.owner                      = &native;
        slot.index                      = 0;
        slot.outstanding                = 1;
        slot.rio_outstanding            = 1;
        slot.send_outstanding           = true;
        slot.attempt_bytes              = 4;
        slot.send_offset                = bad_offset ? 5U : 3U;
        slot.send_buffer.Length         = 1;
        slot.send_request.operation     = cec_engine_operation::send;
        slot.send_request.sequence      = 1;
        slot.send_request.posted_length = 1;
        slot.send_request.token         = cec_operation_token_encode(0, cec_engine_operation::send, 0, 1);
        RIORESULT result{};
        result.SocketContext            = bad_socket_context ? 0U : reinterpret_cast<ULONG_PTR>(&slot);
        result.RequestContext           = static_cast<ULONG_PTR>(slot.send_request.token);
        result.BytesTransferred         = valid || bad_offset ? 1U : 2U;
        const cec_completion completion = cec_native_retire_rio(&native, result);
        return valid && completion.kind == cec_completion_kind::sent && slot.send_offset == 4 &&
                       slot.outstanding == 0 && native.rio_outstanding == 0 && slot.send_request.token == 0 ?
                   0 :
                   1;
    }
    if (std::wcscmp(argv[1], L"completion_slot_identity") == 0) {
        cec_native_worker              native{};
        std::array<cec_native_slot, 2> slots{};
        native.slots                        = slots.data();
        native.session_count                = 2;
        native.rio_outstanding              = 1;
        slots[0].owner                      = &native;
        slots[0].index                      = 1;
        slots[0].outstanding                = 1;
        slots[0].rio_outstanding            = 1;
        slots[0].send_outstanding           = true;
        slots[0].send_request.operation     = cec_engine_operation::send;
        slots[0].send_request.sequence      = 1;
        slots[0].send_request.posted_length = 1;
        slots[0].send_request.token         = cec_operation_token_encode(0, cec_engine_operation::send, 0, 1);
        slots[0].send_buffer.Length         = 1;
        RIORESULT result{};
        result.SocketContext    = reinterpret_cast<ULONG_PTR>(&slots[0]);
        result.RequestContext   = static_cast<ULONG_PTR>(slots[0].send_request.token);
        result.BytesTransferred = 1;
        (void) cec_native_retire_rio(&native, result);
        return 1;
    }
    if (std::wcscmp(argv[1], L"completion_stale_generation") == 0) {
        // The token was posted by generation one and the slot is now generation two: the lane matches
        // the token, so only the generation carried by value inside the context can reject it.
        cec_native_worker native{};
        cec_native_slot   slot{};
        native.slots                    = &slot;
        native.session_count            = 1;
        native.rio_outstanding          = 1;
        slot.owner                      = &native;
        slot.index                      = 0;
        slot.generation                 = 2;
        slot.outstanding                = 1;
        slot.rio_outstanding            = 1;
        slot.send_outstanding           = true;
        slot.attempt_bytes              = 4;
        slot.send_offset                = 0;
        slot.send_buffer.Length         = 1;
        slot.send_request.operation     = cec_engine_operation::send;
        slot.send_request.sequence      = 1;
        slot.send_request.posted_length = 1;
        slot.send_request.token         = cec_operation_token_encode(0, cec_engine_operation::send, 1, 1);
        RIORESULT result{};
        result.SocketContext  = reinterpret_cast<ULONG_PTR>(&slot);
        result.RequestContext = static_cast<ULONG_PTR>(slot.send_request.token);
        (void) cec_native_retire_rio(&native, result);
        return 1;
    }
    if (std::wcscmp(argv[1], L"completion_stale_token") == 0) {
        // A completion whose token does not match the posted lane: a stale retirement or a duplicate.
        cec_native_worker native{};
        cec_native_slot   slot{};
        native.slots                    = &slot;
        native.session_count            = 1;
        native.rio_outstanding          = 1;
        slot.owner                      = &native;
        slot.index                      = 0;
        slot.outstanding                = 1;
        slot.rio_outstanding            = 1;
        slot.send_outstanding           = true;
        slot.attempt_bytes              = 4;
        slot.send_offset                = 0;
        slot.send_buffer.Length         = 1;
        slot.send_request.operation     = cec_engine_operation::send;
        slot.send_request.sequence      = 2;
        slot.send_request.posted_length = 1;
        slot.send_request.token         = cec_operation_token_encode(0, cec_engine_operation::send, 0, 2);
        RIORESULT result{};
        result.SocketContext  = reinterpret_cast<ULONG_PTR>(&slot);
        result.RequestContext = static_cast<ULONG_PTR>(cec_operation_token_encode(0, cec_engine_operation::send, 0, 1));
        (void) cec_native_retire_rio(&native, result);
        return 1;
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
        static_cast<void>(cec_require_latency_microseconds(1, 0));
    } else {
        return 1;
    }
    return 1;
}
