#include "cec_types.h"
#include "cec_engine_internal.h"

#include <WinSock2.h>
#include <MSWSock.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

class cec_engine_winsock
{
  public:
    bool started = false;

    bool start() noexcept
    {
        WSADATA data{};
        started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        return started;
    }

    ~cec_engine_winsock() noexcept
    {
        if (started)
        {
            WSACleanup();
        }
    }
};

static constexpr ULONG_PTR cec_engine_stop_key = 1U;
static constexpr ULONG cec_engine_batch_size = 256U;

static void cec_engine_report(const wchar_t* stage, int error) noexcept
{
    std::fwprintf(stderr, L"%ls failed: native_error=%d\n", stage, error);
}

static void cec_engine_session_socket_close(cec_engine_session* session) noexcept
{
    session->owner->resources->session_sockets[session->index].reset();
    session->socket = INVALID_SOCKET;
}

static SOCKET cec_engine_registered_socket(int type, int protocol) noexcept
{
    return WSASocketW(AF_INET, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_REGISTERED_IO);
}

static bool cec_engine_load_extensions(RIO_EXTENSION_FUNCTION_TABLE* rio, LPFN_CONNECTEX* connect_ex) noexcept
{
    cec_socket_owner probe_owner{cec_engine_registered_socket(SOCK_STREAM, IPPROTO_TCP)};
    const SOCKET probe = probe_owner.get();
    if (probe == INVALID_SOCKET)
    {
        cec_engine_report(L"WSASocketW(RIO probe)", WSAGetLastError());
        return false;
    }
    GUID rio_identifier = WSAID_MULTIPLE_RIO;
    DWORD bytes = 0;
    std::memset(rio, 0, sizeof(*rio));
    rio->cbSize = sizeof(*rio);
    if (WSAIoctl(probe, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER, &rio_identifier, sizeof(rio_identifier), rio,
                 sizeof(*rio), &bytes, nullptr, nullptr) != 0)
    {
        const int error = WSAGetLastError();
        cec_engine_report(L"SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER(RIO)", error);
        return false;
    }
    GUID connect_identifier = WSAID_CONNECTEX;
    bytes = 0;
    if (WSAIoctl(probe, SIO_GET_EXTENSION_FUNCTION_POINTER, &connect_identifier, sizeof(connect_identifier), connect_ex,
                 sizeof(*connect_ex), &bytes, nullptr, nullptr) != 0 ||
        *connect_ex == nullptr)
    {
        const int error = WSAGetLastError();
        cec_engine_report(L"SIO_GET_EXTENSION_FUNCTION_POINTER(ConnectEx)", error);
        return false;
    }
    return true;
}

static bool cec_engine_configure_socket(SOCKET socket_value, const cec_options* options, bool tcp) noexcept
{
    if (options->socket_buffer_bytes != 0)
    {
        const int size = static_cast<int>(options->socket_buffer_bytes);
        if (setsockopt(socket_value, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof(size)) != 0 ||
            setsockopt(socket_value, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size), sizeof(size)) != 0)
        {
            cec_engine_report(L"setsockopt(SO_SNDBUF/SO_RCVBUF)", WSAGetLastError());
            return false;
        }
    }
    if (tcp)
    {
        const BOOL enabled = TRUE;
        if (setsockopt(socket_value, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled),
                       sizeof(enabled)) != 0)
        {
            cec_engine_report(L"setsockopt(TCP_NODELAY)", WSAGetLastError());
            return false;
        }
    }
    return true;
}

static bool cec_engine_resolve_ipv4(const wchar_t* host, std::uint16_t port, SOCKADDR_IN* remote) noexcept
{
    ADDRINFOW hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = 0;
    hints.ai_protocol = 0;
    ADDRINFOW* results = nullptr;
    wchar_t service[16]{};
    _snwprintf_s(service, _countof(service), _TRUNCATE, L"%u", static_cast<unsigned>(port));
    const int status = GetAddrInfoW(host, service, &hints, &results);
    std::unique_ptr<ADDRINFOW, decltype(&FreeAddrInfoW)> results_owner{results, &FreeAddrInfoW};
    if (status != 0 || results == nullptr)
    {
        cec_engine_report(L"GetAddrInfoW(IPv4)", status);
        return false;
    }
    std::memcpy(remote, results->ai_addr, sizeof(*remote));
    return true;
}

static std::byte* cec_engine_build_pattern(const cec_options* options, std::size_t* size,
                                           cec_heap_owner* owner) noexcept
{
    if (options->pattern_kind == cec_pattern_kind::binary_counter ||
        options->pattern_kind == cec_pattern_kind::printable_counter)
    {
        *size = options->pattern_bytes;
        std::byte* pattern = static_cast<std::byte*>(HeapAlloc(GetProcessHeap(), 0, *size));
        owner->reset(pattern);
        if (pattern == nullptr)
        {
            return nullptr;
        }
        if (options->pattern_kind == cec_pattern_kind::binary_counter)
        {
            cec_fill_binary_pattern(pattern, *size);
        }
        else
        {
            cec_fill_printable_pattern(pattern, *size);
        }
        return pattern;
    }

    std::array<wchar_t, 512> default_pattern{};
    const wchar_t* text = options->literal_pattern;
    if (text[0] == L'\0')
    {
        _snwprintf_s(default_pattern.data(), default_pattern.size(), _TRUNCATE, L"C++ echo from %ls", options->host);
        text = default_pattern.data();
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
    {
        return nullptr;
    }
    *size = static_cast<std::size_t>(bytes - 1);
    std::byte* pattern = static_cast<std::byte*>(HeapAlloc(GetProcessHeap(), 0, static_cast<std::size_t>(bytes)));
    owner->reset(pattern);
    if (pattern == nullptr || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                                                  reinterpret_cast<char*>(pattern), bytes, nullptr, nullptr) != bytes)
    {
        owner->reset();
        return nullptr;
    }
    return pattern;
}

static void cec_engine_record_latency(cec_engine_metrics* metrics, const LARGE_INTEGER& start,
                                      const LARGE_INTEGER& finish, const LARGE_INTEGER& frequency) noexcept
{
    const std::uint64_t ticks = static_cast<std::uint64_t>(finish.QuadPart - start.QuadPart);
    const std::uint64_t microseconds =
        std::max<std::uint64_t>(1, ticks * 1000000ULL / static_cast<std::uint64_t>(frequency.QuadPart));
    const unsigned bin = std::min<unsigned>(std::bit_width(microseconds) - 1U, 63U);
    metrics->latency_bins[bin].fetch_add(1, std::memory_order_relaxed);
}

static void cec_engine_arm(cec_engine_worker* worker) noexcept
{
    if (worker->notification_armed)
    {
        cec_engine_fail_fast(L"client notification duplicate arm", ERROR_INVALID_STATE);
    }
    std::memset(&worker->notification_overlapped, 0, sizeof(worker->notification_overlapped));
    const int status = worker->rio->RIONotify(worker->completion_queue);
    cec_require_rio_notify_success(status, L"RIONotify(client)");
    if (!cec_notification_mark_rearmed(&worker->notification_armed))
    {
        cec_engine_fail_fast(L"client notification rearm transition", ERROR_INVALID_STATE);
    }
}

static void cec_engine_schedule(cec_engine_session* session, ULONGLONG deadline) noexcept
{
    if (!cec_timer_insert_or_update(&session->owner->timers, session->index, deadline))
    {
        cec_engine_fail_fast(L"client timer insert/update", ERROR_INVALID_DATA);
    }
}

static void cec_engine_unschedule(cec_engine_session* session) noexcept
{
    (void)cec_timer_remove(&session->owner->timers, session->index);
}

static void cec_engine_mark_done(cec_engine_session* session) noexcept
{
    cec_engine_unschedule(session);
    cec_engine_session_socket_close(session);
    session->request_queue = RIO_INVALID_RQ;
    if (session->state != cec_engine_state::done)
    {
        session->state = cec_engine_state::done;
        --session->owner->live_sessions;
    }
}

static void cec_engine_finish_close(cec_engine_session* session) noexcept
{
    if (session->reconnect_after_close && !session->owner->stopping)
    {
        session->state = cec_engine_state::reconnecting;
        session->next_action =
            GetTickCount64() + static_cast<ULONGLONG>(session->owner->options->reconnect_seconds) * 1000ULL;
        cec_engine_schedule(session, session->next_action);
    }
    else
    {
        cec_engine_mark_done(session);
    }
}

static bool cec_engine_create_request_queue(cec_engine_session* session) noexcept
{
    session->request_queue = session->owner->rio->RIOCreateRequestQueue(
        session->socket, 1, 1, 1, 1, session->owner->completion_queue, session->owner->completion_queue, session);
    if (session->request_queue == RIO_INVALID_RQ)
    {
        cec_engine_report(L"RIOCreateRequestQueue(client)", WSAGetLastError());
        return false;
    }
    return true;
}

static void cec_engine_close_attempt(cec_engine_session* session, bool reconnect) noexcept
{
    if (session->state == cec_engine_state::closing || session->state == cec_engine_state::done)
    {
        return;
    }
    cec_engine_unschedule(session);
    session->reconnect_after_close = reconnect;
    session->state = cec_engine_state::closing;
    cec_engine_session_socket_close(session);
    if (session->outstanding == 0)
    {
        cec_engine_finish_close(session);
    }
}

static bool cec_engine_post_send(cec_engine_session* session) noexcept
{
    session->send_request.operation = cec_engine_operation::send;
    session->send_buffer.Offset =
        static_cast<ULONG>(session->index * 2U * session->owner->maximum_attempt_bytes + session->send_offset);
    session->send_buffer.Length = static_cast<ULONG>(session->attempt_bytes - session->send_offset);
    if (session->owner->rio->RIOSend(session->request_queue, &session->send_buffer, 1, 0, &session->send_request) ==
        FALSE)
    {
        cec_engine_report(L"RIOSend(client)", WSAGetLastError());
        return false;
    }
    ++session->outstanding;
    return true;
}

static bool cec_engine_post_receive(cec_engine_session* session) noexcept
{
    session->receive_request.operation = cec_engine_operation::receive;
    session->receive_buffer.Offset = static_cast<ULONG>(session->index * 2U * session->owner->maximum_attempt_bytes +
                                                        session->owner->maximum_attempt_bytes);
    session->receive_buffer.Length = static_cast<ULONG>(session->attempt_bytes);
    const DWORD flags = session->owner->options->protocol == cec_protocol::tcp ? RIO_MSG_WAITALL : 0U;
    if (session->owner->rio->RIOReceive(session->request_queue, &session->receive_buffer, 1, flags,
                                        &session->receive_request) == FALSE)
    {
        cec_engine_report(L"RIOReceive(client)", WSAGetLastError());
        return false;
    }
    ++session->outstanding;
    return true;
}

static bool cec_engine_begin_attempt(cec_engine_session* session) noexcept
{
    const std::uint64_t requested =
        session->owner->options->protocol == cec_protocol::tcp ? session->owner->options->pipeline_depth : 1U;
    const std::uint64_t granted =
        cec_claim_attempts(&session->owner->metrics->claimed, session->owner->options->echo_count, requested);
    if (granted == 0)
    {
        cec_engine_mark_done(session);
        return true;
    }
    session->requested_echoes = granted;
    session->attempt_bytes = session->owner->pattern_bytes * static_cast<std::size_t>(granted);
    session->send_offset = 0;
    session->received_bytes = 0;
    session->send_done = false;
    session->receive_done = false;
    session->attempt_accounted = false;
    session->state = cec_engine_state::active;
    QueryPerformanceCounter(&session->started_at);
    session->deadline = GetTickCount64() + static_cast<ULONGLONG>(session->owner->options->timeout_seconds) * 1000ULL;
    cec_engine_schedule(session, session->deadline);
    if (!cec_engine_post_receive(session))
    {
        return false;
    }
    if (!cec_engine_post_send(session))
    {
        return false;
    }
    return true;
}

static bool cec_engine_start_socket(cec_engine_session* session) noexcept
{
    const bool tcp = session->owner->options->protocol == cec_protocol::tcp;
    session->socket = cec_engine_registered_socket(tcp ? SOCK_STREAM : SOCK_DGRAM, tcp ? IPPROTO_TCP : IPPROTO_UDP);
    session->owner->resources->session_sockets[session->index].reset(session->socket);
    if (session->socket == INVALID_SOCKET ||
        !cec_engine_configure_socket(session->socket, session->owner->options, tcp))
    {
        cec_engine_report(L"client socket creation", WSAGetLastError());
        return false;
    }
    SOCKADDR_IN local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(session->owner->options->local_port);
    if (bind(session->socket, reinterpret_cast<const SOCKADDR*>(&local), sizeof(local)) != 0)
    {
        cec_engine_report(L"bind(client)", WSAGetLastError());
        return false;
    }
    if (!tcp)
    {
        if (connect(session->socket, reinterpret_cast<const SOCKADDR*>(session->owner->remote_address),
                    sizeof(*session->owner->remote_address)) != 0 ||
            !cec_engine_create_request_queue(session))
        {
            cec_engine_report(L"connect/RQ(UDP client)", WSAGetLastError());
            return false;
        }
        return cec_engine_begin_attempt(session);
    }

    if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(session->socket), session->owner->port,
                               static_cast<ULONG_PTR>(reinterpret_cast<std::uintptr_t>(session)),
                               0) != session->owner->port)
    {
        cec_engine_report(L"CreateIoCompletionPort(ConnectEx socket)", static_cast<int>(GetLastError()));
        return false;
    }
    std::memset(&session->connect_overlapped, 0, sizeof(session->connect_overlapped));
    session->state = cec_engine_state::connecting;
    session->deadline = GetTickCount64() + static_cast<ULONGLONG>(session->owner->options->timeout_seconds) * 1000ULL;
    cec_engine_schedule(session, session->deadline);
    ++session->outstanding;
    const BOOL connected = session->owner->connect_ex(
        session->socket, reinterpret_cast<const SOCKADDR*>(session->owner->remote_address),
        sizeof(*session->owner->remote_address), nullptr, 0, nullptr, &session->connect_overlapped);
    if (connected == FALSE && WSAGetLastError() != ERROR_IO_PENDING)
    {
        --session->outstanding;
        cec_engine_report(L"ConnectEx", WSAGetLastError());
        return false;
    }
    return true;
}

static void cec_engine_connection_failed(cec_engine_session* session) noexcept
{
    if (session->state == cec_engine_state::active && !session->attempt_accounted)
    {
        session->owner->metrics->lost.fetch_add(session->requested_echoes, std::memory_order_relaxed);
        session->attempt_accounted = true;
    }
    session->owner->metrics->network_errors.fetch_add(1, std::memory_order_relaxed);
    const bool reconnect = session->owner->options->reconnect_seconds >= 0 && !session->owner->stopping;
    cec_engine_close_attempt(session, reconnect);
}

static void cec_engine_complete_attempt(cec_engine_session* session) noexcept
{
    if (!session->send_done || !session->receive_done || session->outstanding != 0)
    {
        return;
    }
    const char* send_data = session->owner->memory + session->index * 2U * session->owner->maximum_attempt_bytes;
    const char* receive_data = send_data + session->owner->maximum_attempt_bytes;
    const bool equal = session->received_bytes == session->attempt_bytes &&
                       std::memcmp(send_data, receive_data, session->attempt_bytes) == 0;
    LARGE_INTEGER finish{};
    QueryPerformanceCounter(&finish);
    cec_engine_record_latency(session->owner->metrics, session->started_at, finish,
                              session->owner->performance_frequency);
    if (equal)
    {
        session->owner->metrics->echoed.fetch_add(session->requested_echoes, std::memory_order_relaxed);
        session->owner->metrics->bytes.fetch_add(session->attempt_bytes, std::memory_order_relaxed);
    }
    else
    {
        session->owner->metrics->corrupted.fetch_add(session->requested_echoes, std::memory_order_relaxed);
    }
    session->attempt_accounted = true;
    if (session->owner->options->interval_milliseconds != 0)
    {
        session->state = cec_engine_state::pacing;
        session->next_action = GetTickCount64() + session->owner->options->interval_milliseconds;
        cec_engine_schedule(session, session->next_action);
    }
    else if (!cec_engine_begin_attempt(session))
    {
        cec_engine_connection_failed(session);
    }
}

static void cec_engine_process_rio_result(cec_engine_worker* worker, const RIORESULT& result) noexcept
{
    cec_engine_request* request =
        reinterpret_cast<cec_engine_request*>(static_cast<std::uintptr_t>(result.RequestContext));
    if (request == nullptr || request->session == nullptr || request->session->owner != worker)
    {
        cec_engine_fail_fast(L"client RIO RequestContext", ERROR_INVALID_DATA);
    }
    cec_engine_session* session = request->session;
    if (session->outstanding == 0)
    {
        cec_engine_fail_fast(L"client RIO outstanding count", ERROR_INVALID_DATA);
    }
    --session->outstanding;
    if (session->state == cec_engine_state::closing)
    {
        if (session->outstanding == 0)
        {
            cec_engine_finish_close(session);
        }
        return;
    }
    if (result.Status != ERROR_SUCCESS)
    {
        if (!session->attempt_accounted)
        {
            worker->metrics->lost.fetch_add(session->requested_echoes, std::memory_order_relaxed);
            session->attempt_accounted = true;
        }
        cec_engine_connection_failed(session);
        return;
    }

    if (request->operation == cec_engine_operation::send)
    {
        if (result.BytesTransferred == 0 || result.BytesTransferred > session->attempt_bytes - session->send_offset)
        {
            cec_engine_connection_failed(session);
            return;
        }
        session->send_offset += result.BytesTransferred;
        if (session->send_offset < session->attempt_bytes)
        {
            if (!cec_engine_post_send(session))
            {
                cec_engine_connection_failed(session);
            }
            return;
        }
        session->send_done = true;
    }
    else
    {
        if (worker->options->protocol == cec_protocol::tcp && result.BytesTransferred != session->attempt_bytes)
        {
            cec_engine_connection_failed(session);
            return;
        }
        session->received_bytes = result.BytesTransferred;
        session->receive_done = true;
    }
    cec_engine_complete_attempt(session);
}

static void cec_engine_drain(cec_engine_worker* worker) noexcept
{
    std::array<RIORESULT, cec_engine_batch_size> results{};
    for (;;)
    {
        const ULONG count =
            cec_require_valid_dequeue_count(worker->rio->RIODequeueCompletion(worker->completion_queue, results.data(),
                                                                              static_cast<ULONG>(results.size())),
                                            L"RIODequeueCompletion(client)");
        if (count == 0)
        {
            return;
        }
        for (ULONG index = 0; index < count; ++index)
        {
            cec_engine_process_rio_result(worker, results[index]);
        }
    }
}

static void cec_engine_process_connect(cec_engine_session* session, BOOL completion_ok, DWORD error) noexcept
{
    if (session->outstanding == 0)
    {
        cec_engine_fail_fast(L"ConnectEx outstanding count", ERROR_INVALID_DATA);
    }
    --session->outstanding;
    if (session->state == cec_engine_state::closing)
    {
        if (session->outstanding == 0)
        {
            cec_engine_finish_close(session);
        }
        return;
    }
    if (completion_ok == FALSE || setsockopt(session->socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) != 0)
    {
        cec_engine_report(L"ConnectEx completion",
                          completion_ok == FALSE ? static_cast<int>(error) : WSAGetLastError());
        cec_engine_connection_failed(session);
        return;
    }
    if (!cec_engine_create_request_queue(session) || !cec_engine_begin_attempt(session))
    {
        cec_engine_connection_failed(session);
    }
}

static void cec_engine_process_deadlines(cec_engine_worker* worker) noexcept
{
    const ULONGLONG now = GetTickCount64();
    std::uint32_t index = 0;
    while (cec_timer_pop_expired(&worker->timers, now, &index))
    {
        cec_engine_session* session = &worker->sessions[index];
        if (session->state == cec_engine_state::active || session->state == cec_engine_state::connecting)
        {
            cec_engine_connection_failed(session);
        }
        else if (session->state == cec_engine_state::pacing)
        {
            if (!cec_engine_begin_attempt(session))
            {
                cec_engine_connection_failed(session);
            }
        }
        else if (session->state == cec_engine_state::reconnecting)
        {
            session->request_queue = RIO_INVALID_RQ;
            if (!cec_engine_start_socket(session))
            {
                cec_engine_connection_failed(session);
            }
        }
    }
}

static void cec_engine_stop_worker(cec_engine_worker* worker) noexcept
{
    if (worker->stopping)
    {
        return;
    }
    worker->stopping = true;
    for (std::uint32_t index = 0; index < worker->session_count; ++index)
    {
        cec_engine_session* session = &worker->sessions[index];
        if (session->state == cec_engine_state::done)
        {
            continue;
        }
        if (worker->fatal->load(std::memory_order_acquire) && session->state == cec_engine_state::active &&
            !session->attempt_accounted)
        {
            worker->metrics->lost.fetch_add(session->requested_echoes, std::memory_order_relaxed);
            session->attempt_accounted = true;
        }
        session->reconnect_after_close = false;
        cec_engine_unschedule(session);
        if (session->outstanding == 0)
        {
            cec_engine_mark_done(session);
        }
        else
        {
            session->state = cec_engine_state::closing;
            cec_engine_session_socket_close(session);
        }
    }
}

static DWORD WINAPI cec_engine_worker_thread(void* parameter) noexcept
{
    cec_engine_worker* worker = static_cast<cec_engine_worker*>(parameter);
    cec_engine_arm(worker);
    for (std::uint32_t index = 0; index < worker->session_count; ++index)
    {
        if (!cec_engine_start_socket(&worker->sessions[index]))
        {
            cec_engine_connection_failed(&worker->sessions[index]);
        }
    }
    while (worker->live_sessions != 0)
    {
        DWORD transferred = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* overlapped = nullptr;
        const DWORD wait_milliseconds = cec_timer_wait_milliseconds(&worker->timers, GetTickCount64());
        const BOOL ok = GetQueuedCompletionStatus(worker->port, &transferred, &key, &overlapped, wait_milliseconds);
        const DWORD error = ok == FALSE ? GetLastError() : ERROR_SUCCESS;
        if (overlapped == &worker->notification_overlapped)
        {
            if (ok == FALSE)
            {
                cec_engine_fail_fast(L"GetQueuedCompletionStatus(client notification)", static_cast<int>(error));
            }
            if (!cec_notification_packet_matches(key, overlapped,
                                                 static_cast<ULONG_PTR>(reinterpret_cast<std::uintptr_t>(worker)),
                                                 &worker->notification_overlapped))
            {
                cec_engine_fail_fast(L"client RIO notification key", ERROR_INVALID_DATA);
            }
            if (!cec_notification_mark_delivered(&worker->notification_armed))
            {
                cec_engine_fail_fast(L"client notification delivery transition", ERROR_INVALID_STATE);
            }
            cec_engine_drain(worker);
            cec_engine_arm(worker);
        }
        else if (overlapped == nullptr && key == cec_engine_stop_key)
        {
            cec_engine_stop_worker(worker);
        }
        else if (overlapped != nullptr && key > cec_engine_stop_key)
        {
            const std::uintptr_t candidate = static_cast<std::uintptr_t>(key);
            const std::uintptr_t first = reinterpret_cast<std::uintptr_t>(worker->sessions);
            const std::uintptr_t last = first + sizeof(cec_engine_session) * worker->session_count;
            if (candidate < first || candidate >= last || (candidate - first) % sizeof(cec_engine_session) != 0)
            {
                cec_engine_fail_fast(L"unexpected ConnectEx completion key", ERROR_INVALID_DATA);
            }
            cec_engine_session* session = reinterpret_cast<cec_engine_session*>(candidate);
            if (overlapped != &session->connect_overlapped)
            {
                cec_engine_fail_fast(L"unexpected ConnectEx completion identity", ERROR_INVALID_DATA);
            }
            else
            {
                cec_engine_process_connect(session, ok, error);
            }
        }
        else if (ok == FALSE && error != WAIT_TIMEOUT)
        {
            cec_engine_fail_fast(L"GetQueuedCompletionStatus(client)", static_cast<int>(error));
        }
        else if (!(ok == FALSE && error == WAIT_TIMEOUT && overlapped == nullptr))
        {
            cec_engine_fail_fast(L"unexpected client IOCP packet", ERROR_INVALID_DATA);
        }
        cec_engine_process_deadlines(worker);
        if (worker->external_stop->load(std::memory_order_acquire))
        {
            cec_engine_stop_worker(worker);
        }
    }
    if (worker->notification_armed)
    {
        if (PostQueuedCompletionStatus(worker->port, 0, 0, &worker->notification_overlapped) == FALSE)
        {
            cec_engine_fail_fast(L"PostQueuedCompletionStatus(client notification shutdown)",
                                 static_cast<int>(GetLastError()));
        }
        DWORD transferred = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* overlapped = nullptr;
        const BOOL shutdown_ok = GetQueuedCompletionStatus(worker->port, &transferred, &key, &overlapped, 1000);
        if (shutdown_ok == FALSE)
        {
            cec_engine_fail_fast(L"GetQueuedCompletionStatus(client notification shutdown)",
                                 static_cast<int>(GetLastError()));
        }
        if (!cec_notification_packet_matches(key, overlapped, 0, &worker->notification_overlapped))
        {
            cec_engine_fail_fast(L"client notification shutdown packet", ERROR_INVALID_DATA);
        }
        if (!cec_notification_mark_delivered(&worker->notification_armed))
        {
            cec_engine_fail_fast(L"client notification shutdown transition", ERROR_INVALID_STATE);
        }
    }
    return worker->fatal->load(std::memory_order_acquire) ? 1U : 0U;
}

static bool cec_engine_worker_initialize(cec_engine_worker* worker, const RIO_EXTENSION_FUNCTION_TABLE* rio,
                                         LPFN_CONNECTEX connect_ex, const cec_options* options,
                                         const SOCKADDR_IN* remote, const std::byte* pattern, std::size_t pattern_bytes,
                                         std::size_t maximum_attempt_bytes, cec_engine_metrics* metrics,
                                         std::atomic<bool>* external_stop, std::atomic<bool>* fatal,
                                         std::uint32_t worker_index, std::uint32_t session_count,
                                         std::uint64_t memory_share, cec_engine_worker_resources* resources) noexcept
{
    std::memset(worker, 0, sizeof(*worker));
    worker->resources = resources;
    worker->completion_queue = RIO_INVALID_CQ;
    worker->registration = RIO_INVALID_BUFFERID;
    worker->rio = rio;
    worker->connect_ex = connect_ex;
    worker->options = options;
    worker->remote_address = remote;
    worker->pattern = pattern;
    worker->pattern_bytes = pattern_bytes;
    worker->maximum_attempt_bytes = maximum_attempt_bytes;
    worker->metrics = metrics;
    worker->external_stop = external_stop;
    worker->fatal = fatal;
    worker->worker_index = worker_index;
    worker->session_count = session_count;
    worker->live_sessions = session_count;
    if (QueryPerformanceFrequency(&worker->performance_frequency) == FALSE ||
        worker->performance_frequency.QuadPart <= 0)
    {
        cec_engine_report(L"QueryPerformanceFrequency", static_cast<int>(GetLastError()));
        return false;
    }
    worker->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    resources->port.reset(worker->port);
    std::size_t arena_bytes = 0;
    if (worker->port == nullptr || options->cq_capacity < session_count * 2U ||
        !cec_checked_storage_bytes(session_count, maximum_attempt_bytes, memory_share, &arena_bytes) ||
        arena_bytes > std::numeric_limits<DWORD>::max())
    {
        cec_engine_report(L"client worker IOCP/CQ/arena capacity", ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    worker->memory = static_cast<char*>(VirtualAlloc(nullptr, arena_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    resources->arena.reset(worker->memory);
    worker->sessions = static_cast<cec_engine_session*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(cec_engine_session) * session_count));
    resources->sessions.reset(worker->sessions);
    worker->timer_nodes =
        static_cast<cec_timer_node*>(HeapAlloc(GetProcessHeap(), 0, sizeof(cec_timer_node) * session_count));
    resources->timer_nodes.reset(worker->timer_nodes);
    worker->timer_positions =
        static_cast<std::uint32_t*>(HeapAlloc(GetProcessHeap(), 0, sizeof(std::uint32_t) * session_count));
    resources->timer_positions.reset(worker->timer_positions);
    resources->session_sockets.reset(new (std::nothrow) cec_socket_owner[session_count]);
    if (worker->sessions != nullptr)
    {
        for (std::uint32_t index = 0; index < session_count; ++index)
        {
            worker->sessions[index].owner = worker;
            worker->sessions[index].socket = INVALID_SOCKET;
            worker->sessions[index].request_queue = RIO_INVALID_RQ;
            worker->sessions[index].index = index;
        }
    }
    if (worker->memory == nullptr || worker->sessions == nullptr || worker->timer_nodes == nullptr ||
        worker->timer_positions == nullptr || !resources->session_sockets ||
        !cec_timer_initialize(&worker->timers, worker->timer_nodes, worker->timer_positions, session_count))
    {
        cec_engine_report(L"client worker allocation", ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    worker->registration = rio->RIORegisterBuffer(worker->memory, static_cast<DWORD>(arena_bytes));
    resources->registration.reset(rio, worker->registration);
    if (worker->registration == RIO_INVALID_BUFFERID)
    {
        cec_engine_report(L"RIORegisterBuffer(client)", WSAGetLastError());
        return false;
    }
    RIO_NOTIFICATION_COMPLETION notification{};
    notification.Type = RIO_IOCP_COMPLETION;
    notification.Iocp.IocpHandle = worker->port;
    notification.Iocp.CompletionKey = worker;
    notification.Iocp.Overlapped = &worker->notification_overlapped;
    worker->completion_queue = rio->RIOCreateCompletionQueue(options->cq_capacity, &notification);
    resources->completion_queue.reset(rio, worker->completion_queue);
    if (worker->completion_queue == RIO_INVALID_CQ)
    {
        cec_engine_report(L"RIOCreateCompletionQueue(client)", WSAGetLastError());
        return false;
    }

    for (std::uint32_t index = 0; index < session_count; ++index)
    {
        cec_engine_session* session = &worker->sessions[index];
        session->owner = worker;
        session->socket = INVALID_SOCKET;
        session->request_queue = RIO_INVALID_RQ;
        session->state = cec_engine_state::dormant;
        session->index = index;
        session->receive_request = cec_engine_request{session, cec_engine_operation::receive};
        session->send_request = cec_engine_request{session, cec_engine_operation::send};
        session->send_buffer.BufferId = worker->registration;
        session->receive_buffer.BufferId = worker->registration;
        char* send_data = worker->memory + index * 2U * maximum_attempt_bytes;
        cec_fill_repeated_pattern(reinterpret_cast<std::byte*>(send_data), maximum_attempt_bytes, pattern,
                                  pattern_bytes);
    }
    worker->thread = CreateThread(nullptr, 0, cec_engine_worker_thread, worker, 0, nullptr);
    resources->thread.reset(worker->thread);
    if (worker->thread == nullptr)
    {
        cec_engine_report(L"CreateThread(client worker)", static_cast<int>(GetLastError()));
        return false;
    }
    return true;
}

static void cec_engine_worker_destroy(cec_engine_worker* worker) noexcept
{
    const bool had_thread = worker->thread != nullptr;
    if (worker->thread != nullptr)
    {
        WaitForSingleObject(worker->thread, INFINITE);
        worker->resources->thread.reset();
        worker->thread = nullptr;
    }
    if (had_thread)
    {
        std::uint32_t outstanding = 0;
        for (std::uint32_t index = 0; index < worker->session_count; ++index)
        {
            outstanding += worker->sessions[index].outstanding;
        }
        const cec_worker_lifecycle lifecycle{cec_worker_phase::stopped, worker->live_sessions, outstanding,
                                             worker->notification_armed};
        if (!cec_worker_may_release(&lifecycle) || worker->timers.size != 0)
        {
            cec_engine_fail_fast(L"client worker release precondition", ERROR_INVALID_STATE);
        }
    }
    if (worker->sessions != nullptr && worker->resources->session_sockets)
    {
        for (std::uint32_t index = 0; index < worker->session_count; ++index)
        {
            cec_engine_session_socket_close(&worker->sessions[index]);
        }
    }
    worker->resources->completion_queue.reset();
    worker->completion_queue = RIO_INVALID_CQ;
    worker->resources->registration.reset();
    worker->registration = RIO_INVALID_BUFFERID;
    worker->resources->arena.reset();
    worker->memory = nullptr;
    worker->resources->session_sockets.reset();
    worker->resources->sessions.reset();
    worker->sessions = nullptr;
    worker->resources->timer_nodes.reset();
    worker->timer_nodes = nullptr;
    worker->resources->timer_positions.reset();
    worker->timer_positions = nullptr;
    worker->resources->port.reset();
    worker->port = nullptr;
}

static std::uint64_t cec_engine_percentile(const cec_engine_metrics* metrics, std::uint64_t total,
                                           std::uint64_t numerator, std::uint64_t denominator) noexcept
{
    if (total == 0)
    {
        return 0;
    }
    const std::uint64_t target = cec_percentile_target(total, numerator, denominator);
    std::uint64_t cumulative = 0;
    for (unsigned index = 0; index < metrics->latency_bins.size(); ++index)
    {
        cumulative += metrics->latency_bins[index].load(std::memory_order_relaxed);
        if (cumulative >= target)
        {
            return index == 63U ? std::numeric_limits<std::uint64_t>::max() : 1ULL << index;
        }
    }
    return 0;
}

static std::uint64_t cec_engine_sample_count(const cec_engine_metrics* metrics) noexcept
{
    std::uint64_t count = 0;
    for (const std::atomic<std::uint64_t>& bin : metrics->latency_bins)
    {
        count += bin.load(std::memory_order_relaxed);
    }
    return count;
}

static void cec_engine_print_metrics(const wchar_t* phase, const cec_options* options,
                                     const cec_engine_metrics* metrics, ULONGLONG elapsed_milliseconds) noexcept
{
    const std::uint64_t echoed = metrics->echoed.load(std::memory_order_relaxed);
    const std::uint64_t bytes = metrics->bytes.load(std::memory_order_relaxed);
    const std::uint64_t samples = cec_engine_sample_count(metrics);
    const double elapsed_seconds = static_cast<double>(std::max<ULONGLONG>(elapsed_milliseconds, 1U)) / 1000.0;
    const double echoes_per_second = static_cast<double>(echoed) / elapsed_seconds;
    const double mebibytes_per_second = static_cast<double>(bytes) / (1024.0 * 1024.0) / elapsed_seconds;
    std::fwprintf(stdout,
                  L"%ls elapsed_ms=%llu sessions=%u echoed=%llu corrupted=%llu lost=%llu network_errors=%llu "
                  L"bytes=%llu echo_per_sec=%.2f MiB_per_sec=%.2f p50_us~%llu p99_us~%llu p999_us~%llu "
                  L"max_us~%llu latency_sample=batch\n",
                  phase, static_cast<unsigned long long>(elapsed_milliseconds), options->session_count,
                  static_cast<unsigned long long>(echoed),
                  static_cast<unsigned long long>(metrics->corrupted.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(metrics->lost.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(metrics->network_errors.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(bytes), echoes_per_second, mebibytes_per_second,
                  static_cast<unsigned long long>(cec_engine_percentile(metrics, samples, 50, 100)),
                  static_cast<unsigned long long>(cec_engine_percentile(metrics, samples, 99, 100)),
                  static_cast<unsigned long long>(cec_engine_percentile(metrics, samples, 999, 1000)),
                  static_cast<unsigned long long>(cec_engine_percentile(metrics, samples, 1, 1)));
}

cec_exit_code cec_run_client(const cec_options* options, std::atomic<bool>* stop_requested) noexcept
{
    if (options == nullptr || stop_requested == nullptr)
    {
        return cec_exit_code::internal;
    }
    cec_engine_winsock winsock{};
    if (!winsock.start())
    {
        cec_engine_report(L"WSAStartup", WSAGetLastError());
        return cec_exit_code::network;
    }
    RIO_EXTENSION_FUNCTION_TABLE rio{};
    LPFN_CONNECTEX connect_ex = nullptr;
    if (!cec_engine_load_extensions(&rio, &connect_ex))
    {
        return cec_exit_code::network;
    }
    SOCKADDR_IN remote{};
    if (!cec_engine_resolve_ipv4(options->host, options->remote_port, &remote))
    {
        return cec_exit_code::network;
    }
    std::size_t pattern_bytes = 0;
    cec_heap_owner pattern_owner{};
    std::byte* pattern = cec_engine_build_pattern(options, &pattern_bytes, &pattern_owner);
    if (pattern == nullptr || pattern_bytes == 0 ||
        (options->protocol == cec_protocol::udp && pattern_bytes > CEC_MAXIMUM_UDP_PAYLOAD_BYTES))
    {
        cec_engine_report(L"payload pattern", ERROR_INVALID_DATA);
        return cec_exit_code::usage;
    }
    std::size_t maximum_attempt_bytes = 0;
    const std::size_t depth = options->protocol == cec_protocol::tcp ? options->pipeline_depth : 1U;
    if (!cec_checked_product(pattern_bytes, depth, &maximum_attempt_bytes) ||
        maximum_attempt_bytes > CEC_MAXIMUM_TCP_BATCH_BYTES)
    {
        cec_engine_report(L"payload batch size", ERROR_ARITHMETIC_OVERFLOW);
        return cec_exit_code::usage;
    }
    std::size_t total_storage = 0;
    if (!cec_checked_storage_bytes(options->session_count, maximum_attempt_bytes, options->memory_bytes,
                                   &total_storage))
    {
        cec_engine_report(L"registered storage /memory limit", ERROR_NOT_ENOUGH_MEMORY);
        return cec_exit_code::usage;
    }

    std::uint32_t worker_count = options->worker_count;
    if (worker_count == 0)
    {
        worker_count = std::clamp(static_cast<std::uint32_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)), 1U, 32U);
    }
    worker_count = std::min(worker_count, options->session_count);
    cec_engine_worker* workers = static_cast<cec_engine_worker*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(cec_engine_worker) * worker_count));
    cec_heap_owner workers_owner{workers};
    std::unique_ptr<cec_engine_worker_resources[]> worker_resources{new (std::nothrow)
                                                                        cec_engine_worker_resources[worker_count]};
    if (workers == nullptr || !worker_resources)
    {
        return cec_exit_code::network;
    }
    cec_engine_metrics metrics{};
    std::atomic<bool> fatal{false};
    std::uint32_t initialized = 0;
    std::uint32_t remaining_sessions = options->session_count;
    for (; initialized < worker_count; ++initialized)
    {
        const std::uint32_t workers_left = worker_count - initialized;
        const std::uint32_t sessions = (remaining_sessions + workers_left - 1U) / workers_left;
        const std::uint64_t memory_share =
            (options->memory_bytes / options->session_count) * static_cast<std::uint64_t>(sessions);
        if (!cec_engine_worker_initialize(&workers[initialized], &rio, connect_ex, options, &remote, pattern,
                                          pattern_bytes, maximum_attempt_bytes, &metrics, stop_requested, &fatal,
                                          initialized, sessions, memory_share, &worker_resources[initialized]))
        {
            fatal.store(true, std::memory_order_release);
            cec_engine_worker_destroy(&workers[initialized]);
            break;
        }
        remaining_sessions -= sessions;
    }

    const ULONGLONG start = GetTickCount64();
    ULONGLONG next_report = options->report_seconds == 0
                                ? std::numeric_limits<ULONGLONG>::max()
                                : start + static_cast<ULONGLONG>(options->report_seconds) * 1000ULL;
    bool all_done = false;
    bool stop_posts_sent = false;
    while (!all_done)
    {
        const ULONGLONG now = GetTickCount64();
        if (options->run_seconds != 0 && now - start >= options->run_seconds * 1000ULL)
        {
            stop_requested->store(true, std::memory_order_release);
        }
        if (now >= next_report)
        {
            cec_engine_print_metrics(L"report", options, &metrics, now - start);
            next_report = now + static_cast<ULONGLONG>(options->report_seconds) * 1000ULL;
        }
        if (!stop_posts_sent &&
            (fatal.load(std::memory_order_acquire) || stop_requested->load(std::memory_order_acquire)))
        {
            for (std::uint32_t index = 0; index < initialized; ++index)
            {
                if (PostQueuedCompletionStatus(workers[index].port, 0, cec_engine_stop_key, nullptr) == FALSE)
                {
                    cec_engine_fail_fast(L"PostQueuedCompletionStatus(client stop)", static_cast<int>(GetLastError()));
                }
            }
            stop_posts_sent = true;
        }
        all_done = true;
        for (std::uint32_t index = 0; index < initialized; ++index)
        {
            if (WaitForSingleObject(workers[index].thread, 0) == WAIT_TIMEOUT)
            {
                all_done = false;
            }
        }
        if (!all_done)
        {
            Sleep(10);
        }
    }
    for (std::uint32_t index = 0; index < initialized; ++index)
    {
        cec_engine_worker_destroy(&workers[index]);
    }
    const std::uint64_t never_claimed =
        cec_unclaimed_echoes(options->echo_count, metrics.claimed.load(std::memory_order_relaxed),
                             stop_requested->load(std::memory_order_acquire));
    if (never_claimed != 0)
    {
        metrics.lost.fetch_add(never_claimed, std::memory_order_relaxed);
    }

    const std::uint64_t echoed = metrics.echoed.load(std::memory_order_relaxed);
    const std::uint64_t corrupted = metrics.corrupted.load(std::memory_order_relaxed);
    const std::uint64_t lost = metrics.lost.load(std::memory_order_relaxed);
    if (!options->quiet || options->stats)
    {
        cec_engine_print_metrics(L"final", options, &metrics, GetTickCount64() - start);
    }
    return cec_classify_result(echoed, corrupted, lost, metrics.network_errors.load(std::memory_order_relaxed),
                               fatal.load(std::memory_order_acquire), stop_requested->load(std::memory_order_acquire));
}
