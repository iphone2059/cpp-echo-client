//! cec.native — converted from a header/implementation pair into a single C++20 named module.

module;

// clang-format off
// The SDK headers are order sensitive: WinSock2 before MSWSock and WS2tcpip, then
// Windows.h and mmsystem.h. clang-format must not reorder this block.
#include <WinSock2.h>
#include <MSWSock.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <mmsystem.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

export module cec.native;

import cec.metrics;
export import cec.types;

// ---- declarations (the former native.h) ----

export class cec_socket_owner {
  public:
    cec_socket_owner() noexcept;
    explicit cec_socket_owner(SOCKET value) noexcept;
    ~cec_socket_owner() noexcept;
    cec_socket_owner(const cec_socket_owner&)            = delete;
    cec_socket_owner& operator=(const cec_socket_owner&) = delete;
    cec_socket_owner(cec_socket_owner&& other) noexcept;
    cec_socket_owner& operator=(cec_socket_owner&& other) noexcept;
    SOCKET            get() const noexcept;
    SOCKET            release() noexcept;
    void              reset(SOCKET value = INVALID_SOCKET) noexcept;

  private:
    SOCKET value_;
};

export class cec_handle_owner {
  public:
    cec_handle_owner() noexcept;
    explicit cec_handle_owner(HANDLE value) noexcept;
    ~cec_handle_owner() noexcept;
    cec_handle_owner(const cec_handle_owner&)            = delete;
    cec_handle_owner& operator=(const cec_handle_owner&) = delete;
    cec_handle_owner(cec_handle_owner&& other) noexcept;
    cec_handle_owner& operator=(cec_handle_owner&& other) noexcept;
    HANDLE            get() const noexcept;
    HANDLE            release() noexcept;
    void              reset(HANDLE value = nullptr) noexcept;

  private:
    HANDLE value_;
};

export class cec_virtual_arena_owner {
  public:
    cec_virtual_arena_owner() noexcept;
    explicit cec_virtual_arena_owner(void* value) noexcept;
    ~cec_virtual_arena_owner() noexcept;
    cec_virtual_arena_owner(const cec_virtual_arena_owner&)            = delete;
    cec_virtual_arena_owner& operator=(const cec_virtual_arena_owner&) = delete;
    cec_virtual_arena_owner(cec_virtual_arena_owner&& other) noexcept;
    cec_virtual_arena_owner& operator=(cec_virtual_arena_owner&& other) noexcept;
    void*                    get() const noexcept;
    void*                    release() noexcept;
    void                     reset(void* value = nullptr) noexcept;

  private:
    void* value_;
};

export class cec_heap_owner {
  public:
    cec_heap_owner() noexcept;
    explicit cec_heap_owner(void* value) noexcept;
    ~cec_heap_owner() noexcept;
    cec_heap_owner(const cec_heap_owner&)            = delete;
    cec_heap_owner& operator=(const cec_heap_owner&) = delete;
    cec_heap_owner(cec_heap_owner&& other) noexcept;
    cec_heap_owner& operator=(cec_heap_owner&& other) noexcept;
    void*           get() const noexcept;
    void*           release() noexcept;
    void            reset(void* value = nullptr) noexcept;

  private:
    void* value_;
};

export class cec_rio_registration_owner {
  public:
    cec_rio_registration_owner() noexcept;
    cec_rio_registration_owner(const RIO_EXTENSION_FUNCTION_TABLE* rio, RIO_BUFFERID value) noexcept;
    ~cec_rio_registration_owner() noexcept;
    cec_rio_registration_owner(const cec_rio_registration_owner&)            = delete;
    cec_rio_registration_owner& operator=(const cec_rio_registration_owner&) = delete;
    cec_rio_registration_owner(cec_rio_registration_owner&& other) noexcept;
    cec_rio_registration_owner& operator=(cec_rio_registration_owner&& other) noexcept;
    RIO_BUFFERID                get() const noexcept;
    RIO_BUFFERID                release() noexcept;
    void reset(const RIO_EXTENSION_FUNCTION_TABLE* rio = nullptr, RIO_BUFFERID value = RIO_INVALID_BUFFERID) noexcept;

  private:
    const RIO_EXTENSION_FUNCTION_TABLE* rio_;
    RIO_BUFFERID                        value_;
};

export class cec_rio_cq_owner {
  public:
    cec_rio_cq_owner() noexcept;
    cec_rio_cq_owner(const RIO_EXTENSION_FUNCTION_TABLE* rio, RIO_CQ value) noexcept;
    ~cec_rio_cq_owner() noexcept;
    cec_rio_cq_owner(const cec_rio_cq_owner&)            = delete;
    cec_rio_cq_owner& operator=(const cec_rio_cq_owner&) = delete;
    cec_rio_cq_owner(cec_rio_cq_owner&& other) noexcept;
    cec_rio_cq_owner& operator=(cec_rio_cq_owner&& other) noexcept;
    RIO_CQ            get() const noexcept;
    RIO_CQ            release() noexcept;
    void              reset(const RIO_EXTENSION_FUNCTION_TABLE* rio = nullptr, RIO_CQ value = RIO_INVALID_CQ) noexcept;

  private:
    const RIO_EXTENSION_FUNCTION_TABLE* rio_;
    RIO_CQ                              value_;
};

export struct cec_native_slot;
export struct cec_native_worker;
export class cec_engine_worker_resources;

/// Identity of one RIO submission. The provider carries this value back, so it cannot change when
/// the slot is reused: a completion that arrives after the slot was handed to a new generation still
/// carries the generation and the submission serial of the operation that was actually posted. A
/// pointer into the reusable slot record cannot express that, because the record is rewritten at the
/// reuse point and the retire path would read the new values.
export inline constexpr unsigned cec_operation_slot_bits        = 16U;
export inline constexpr unsigned cec_operation_kind_bits        = 2U;
export inline constexpr unsigned cec_operation_generation_bits  = 16U;
export inline constexpr unsigned cec_operation_sequence_bits    = 30U;
export inline constexpr unsigned cec_operation_kind_shift       = cec_operation_slot_bits;
export inline constexpr unsigned cec_operation_generation_shift = cec_operation_slot_bits + cec_operation_kind_bits;
export inline constexpr unsigned cec_operation_sequence_shift =
    cec_operation_generation_shift + cec_operation_generation_bits;
export inline constexpr std::uint64_t cec_operation_slot_mask = (std::uint64_t{ 1 } << cec_operation_slot_bits) - 1U;
export inline constexpr std::uint64_t cec_operation_kind_mask = (std::uint64_t{ 1 } << cec_operation_kind_bits) - 1U;
export inline constexpr std::uint64_t cec_operation_generation_mask =
    (std::uint64_t{ 1 } << cec_operation_generation_bits) - 1U;
export inline constexpr std::uint64_t cec_operation_max_sequence =
    (std::uint64_t{ 1 } << cec_operation_sequence_bits) - 1U;

/// Submission serials wrap, exactly like the generation tag: uniqueness inside one generation comes
/// from the one-outstanding-operation invariant and from comparing the token with the posted lane
/// exactly, so a wrapped value can only alias a completion that stayed unretired for a further 2^30
/// submissions of the same slot. Failing fast at the limit would instead end a long-running worker.
export inline constexpr std::uint64_t cec_operation_next_sequence(std::uint32_t current) noexcept {
    const std::uint64_t next = static_cast<std::uint64_t>(current) + 1U;
    return next > cec_operation_max_sequence ? 1U : next;
}

/// Operation kinds are encoded one-based, so a valid token is never zero and a decoded kind of zero
/// is always invalid. The layout is a value, not a pointer: the provider carries it back unchanged.
export inline constexpr std::uint64_t cec_operation_token_encode(std::uint64_t        slot,
                                                                 cec_engine_operation operation,
                                                                 std::uint32_t        generation,
                                                                 std::uint64_t        sequence) noexcept {
    return ((sequence & cec_operation_max_sequence) << cec_operation_sequence_shift) |
           ((static_cast<std::uint64_t>(generation) & cec_operation_generation_mask)
            << cec_operation_generation_shift) |
           (((static_cast<std::uint64_t>(operation) + 1U) & cec_operation_kind_mask) << cec_operation_kind_shift) |
           (slot & cec_operation_slot_mask);
}

export struct cec_operation_identity {
    std::uint32_t        slot       = 0;
    std::uint32_t        generation = 0;
    std::uint64_t        sequence   = 0;
    cec_engine_operation operation  = cec_engine_operation::receive;
};

export cec_operation_identity cec_operation_token_decode(std::uint64_t token, const wchar_t* stage) noexcept;

/// Ownership ledger of one direction's currently posted RIO operation. The token is the immutable
/// identity handed to the provider, the sequence is that direction's submission serial (never reset
/// across generations), and posted_length is the extent submitted together with that token.
export struct cec_engine_request {
    std::uint64_t        token         = 0;
    std::uint32_t        posted_length = 0;
    std::uint32_t        sequence      = 0;
    cec_engine_operation operation     = cec_engine_operation::receive;
};

export struct cec_native_slot {
    cec_native_worker* owner;
    SOCKET             socket;
    RIO_RQ             request_queue;
    OVERLAPPED         connect_overlapped;
    cec_engine_request receive_request;
    cec_engine_request send_request;
    RIO_BUF            receive_buffer;
    RIO_BUF            send_buffer;
    std::uint32_t      index;
    /// Identity of the generation currently admitted into this slot. It changes only at the reuse
    /// point, after the drain precondition proved nothing of the previous generation is in flight.
    std::uint32_t      generation = 0;
    std::uint32_t      outstanding;
    std::uint32_t      rio_outstanding;
    std::size_t        attempt_bytes;
    std::size_t        send_offset;
    std::size_t        received_bytes;
    bool               closing;
    bool               connect_outstanding;
    bool               send_outstanding;
    bool               receive_outstanding;
};

export struct cec_native_worker {
    cec_engine_worker_resources*        resources      = nullptr;
    const RIO_EXTENSION_FUNCTION_TABLE* rio            = nullptr;
    LPFN_CONNECTEX                      connect_ex     = nullptr;
    const cec_options*                  options        = nullptr;
    const SOCKADDR_IN*                  remote_address = nullptr;
    HANDLE                              port           = nullptr;
    HANDLE                              thread         = nullptr;
    OVERLAPPED                          notification_overlapped{};
    RIO_CQ                              completion_queue       = RIO_INVALID_CQ;
    char*                               memory                 = nullptr;
    cec_native_slot*                    slots                  = nullptr;
    std::size_t                         maximum_attempt_bytes  = 0;
    std::uint32_t                       session_count          = 0;
    std::uint32_t                       worker_index           = 0;
    std::uint32_t                       rio_outstanding        = 0;
    std::uint64_t                       notify_arms            = 0;
    std::uint64_t                       notify_deliveries      = 0;
    std::uint64_t                       notify_timeout_wakeups = 0;
    LARGE_INTEGER                       performance_frequency{};
    bool                                notification_armed = false;
    bool                                joined             = true;
};

export class cec_engine_worker_resources {
  public:
    cec_native_worker* owner = nullptr;
    ~cec_engine_worker_resources() noexcept;
    cec_handle_owner                              port;
    cec_handle_owner                              thread;
    cec_virtual_arena_owner                       arena;
    std::unique_ptr<cec_rio_registration_owner[]> registrations;
    cec_rio_cq_owner                              completion_queue;
    cec_heap_owner                                sessions;
    cec_heap_owner                                slots;
    cec_heap_owner                                timer_nodes;
    cec_heap_owner                                timer_positions;
    std::unique_ptr<cec_socket_owner[]>           session_sockets;
};

export class cec_engine_winsock {
  public:
    bool started = false;
    int  error   = ERROR_SUCCESS;
    bool start() noexcept;
    ~cec_engine_winsock() noexcept;
};

export class cec_engine_timer_resolution {
  public:
    bool     active = false;
    MMRESULT error  = TIMERR_NOERROR;
    bool     start() noexcept;
    ~cec_engine_timer_resolution() noexcept;
};

export inline constexpr ULONG_PTR     cec_engine_stop_key          = 1U;
export inline constexpr ULONG         cec_engine_batch_size        = 256U;
export inline constexpr std::uint32_t cec_engine_max_drain_batches = 64U;
export [[noreturn]] void              cec_engine_fail_fast(const wchar_t* stage, int error) noexcept;
export void                           cec_engine_report(const wchar_t* stage, int error) noexcept;
export std::uint32_t                  cec_resolve_worker_count(const cec_options* options) noexcept;
export std::uint64_t                  cec_engine_now() noexcept;
export bool cec_engine_load_extensions(RIO_EXTENSION_FUNCTION_TABLE* rio, LPFN_CONNECTEX* connect_ex) noexcept;
export bool cec_engine_resolve_ipv4(const wchar_t* host, std::uint16_t port, SOCKADDR_IN* remote) noexcept;
export void cec_engine_session_socket_close(cec_native_slot* slot) noexcept;
export bool cec_native_connect_slot(cec_native_slot* slot) noexcept;
export bool cec_native_slot_drained(const cec_native_slot* slot) noexcept;
export void cec_native_close_slot(cec_native_slot* slot) noexcept;
export bool cec_engine_post_send(cec_native_slot* slot) noexcept;
export bool cec_engine_post_receive(cec_native_slot* slot, std::size_t bytes) noexcept;
export cec_completion         cec_native_retire_rio(cec_native_worker* transport, const RIORESULT& result) noexcept;
export cec_completion         cec_native_retire_connect(cec_native_slot* slot,
                                                        BOOL             ok,
                                                        DWORD            error,
                                                        DWORD            transferred) noexcept;
export void                   cec_native_require_release(const cec_native_worker* transport) noexcept;
export void                   cec_engine_maybe_arm(cec_native_worker* transport) noexcept;
export void                   cec_require_rio_notify_success(int status, const wchar_t* stage) noexcept;
export bool                   cec_register_windows(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                                                   char*                               memory,
                                                   std::size_t                         window_bytes,
                                                   std::uint32_t                       count,
                                                   cec_rio_registration_owner*         registrations) noexcept;
export bool                   cec_prepare_send_window(RIO_BUF* buffer, std::size_t total, std::size_t offset) noexcept;
export void                   cec_notification_arm(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                                                   RIO_CQ                              completion_queue,
                                                   bool*                               armed,
                                                   std::uint64_t*                      arm_count,
                                                   const wchar_t*                      stage) noexcept;
export cec_rio_notify_outcome cec_rio_notify_outcome_of(int status) noexcept;
export ULONG                  cec_require_valid_dequeue_count(ULONG count, const wchar_t* stage) noexcept;
export std::uint64_t          cec_require_latency_microseconds(std::uint64_t ticks, std::uint64_t frequency) noexcept;
export bool                   cec_notification_packet_matches(ULONG_PTR         key,
                                                              const OVERLAPPED* overlapped,
                                                              ULONG_PTR         expected_key,
                                                              const OVERLAPPED* expected_overlapped) noexcept;

export struct cec_notify_diagnostic_snapshot {
    std::uint32_t worker;
    std::uint64_t arms;
    std::uint64_t deliveries;
    std::uint64_t timeout_wakeups;
};

export bool cec_write_diagnostics(const cec_notify_diagnostic_snapshot* values,
                                  std::uint32_t                         count,
                                  cec_protocol                          protocol) noexcept;

// ---- definitions (the former native.cpp) ----

[[noreturn]] void cec_engine_fail_fast(const wchar_t* stage, int error) noexcept {
    char text[160]{};
    (void) WideCharToMultiByte(CP_UTF8, 0, stage, -1, text, sizeof(text), nullptr, nullptr);
    std::fprintf(stderr, "%s failed: native_error=%d\n", text, error);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(cec_exit_code::internal));
    __assume(0);
}

cec_operation_identity cec_operation_token_decode(std::uint64_t token, const wchar_t* stage) noexcept {
    cec_operation_identity identity{};
    if (token == 0) {
        cec_engine_fail_fast(stage, ERROR_INVALID_DATA);
    }
    const std::uint64_t kind = (token >> cec_operation_kind_shift) & cec_operation_kind_mask;
    identity.slot            = static_cast<std::uint32_t>(token & cec_operation_slot_mask);
    identity.generation =
        static_cast<std::uint32_t>((token >> cec_operation_generation_shift) & cec_operation_generation_mask);
    identity.sequence = token >> cec_operation_sequence_shift;
    if (kind == 0 || kind > 2U || identity.sequence == 0) {
        cec_engine_fail_fast(stage, ERROR_INVALID_DATA);
    }
    identity.operation = kind == 1U ? cec_engine_operation::receive : cec_engine_operation::send;
    return identity;
}

bool cec_write_diagnostics(const cec_notify_diagnostic_snapshot* values,
                           std::uint32_t                         count,
                           cec_protocol                          protocol) noexcept {
    if (values == nullptr) {
        return false;
    }
    const DWORD required = GetEnvironmentVariableW(L"CEC_DIAG_FILE", nullptr, 0);
    if (required == 0) {
        return true;
    }
    wchar_t path[512]{};
    if (static_cast<std::size_t>(required) > sizeof(path) / sizeof(path[0])) {
        return false;
    }
    if (GetEnvironmentVariableW(L"CEC_DIAG_FILE", path, static_cast<DWORD>(sizeof(path) / sizeof(path[0]))) == 0) {
        return false;
    }
    const HANDLE file =
        CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const char* protocol_name = protocol == cec_protocol::tcp ? "tcp" : "udp";
    bool        complete      = true;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint64_t arms       = values[index].arms;
        const std::uint64_t deliveries = values[index].deliveries;
        const std::uint64_t gap        = arms >= deliveries ? arms - deliveries : 0U;
        char                line[192]{};
        const int           length =
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "role=client protocol=%s worker=%u notify_arms=%llu notify_deliveries=%llu notify_gap=%llu "
                        "timeout_wakeups_while_outstanding=%llu\n",
                        protocol_name, values[index].worker, static_cast<unsigned long long>(arms),
                        static_cast<unsigned long long>(deliveries), static_cast<unsigned long long>(gap),
                        static_cast<unsigned long long>(values[index].timeout_wakeups));
        if (length <= 0) {
            complete = false;
            continue;
        }
        DWORD written = 0;
        if (WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr) == FALSE ||
            written != static_cast<DWORD>(length)) {
            complete = false;
            break;
        }
    }
    (void) CloseHandle(file);
    return complete;
}

void cec_notification_arm(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                          RIO_CQ                              completion_queue,
                          bool*                               armed,
                          std::uint64_t*                      arm_count,
                          const wchar_t*                      stage) noexcept {
    if (rio == nullptr || rio->RIONotify == nullptr || armed == nullptr || arm_count == nullptr || stage == nullptr) {
        cec_engine_fail_fast(L"client notification arm arguments", ERROR_INVALID_PARAMETER);
    }
    if (*armed) {
        cec_engine_fail_fast(L"client notification duplicate arm", ERROR_INVALID_STATE);
    }
    const int status = rio->RIONotify(completion_queue);
    cec_require_rio_notify_success(status, stage);
    if (!cec_notification_mark_rearmed(armed)) {
        cec_engine_fail_fast(L"client notification rearm transition", ERROR_INVALID_STATE);
    }
    ++*arm_count;
}

void cec_require_rio_notify_success(int status, const wchar_t* stage) noexcept {
    const cec_rio_notify_outcome outcome = cec_rio_notify_outcome_of(status);
    if (outcome == cec_rio_notify_outcome::armed) {
        return;
    }
    if (outcome == cec_rio_notify_outcome::duplicate_arm) {
        // A correct state machine never arms a queue whose previous notification is still pending,
        // so report this distinctly instead of folding it into a generic RIONotify failure.
        cec_engine_fail_fast(L"RIONotify duplicate arm", status);
    }
    cec_engine_fail_fast(stage, status);
}

ULONG cec_require_valid_dequeue_count(ULONG count, const wchar_t* stage) noexcept {
    if (count == RIO_CORRUPT_CQ) {
        cec_engine_fail_fast(stage, ERROR_INVALID_DATA);
    }
    return count;
}

cec_socket_owner::cec_socket_owner() noexcept : value_(INVALID_SOCKET) {}

cec_socket_owner::cec_socket_owner(SOCKET value) noexcept : value_(value) {}

cec_socket_owner::~cec_socket_owner() noexcept {
    reset();
}

cec_socket_owner::cec_socket_owner(cec_socket_owner&& other) noexcept : value_(other.release()) {}

cec_socket_owner& cec_socket_owner::operator=(cec_socket_owner&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

SOCKET cec_socket_owner::get() const noexcept {
    return value_;
}

SOCKET cec_socket_owner::release() noexcept {
    const SOCKET value = value_;
    value_             = INVALID_SOCKET;
    return value;
}

void cec_socket_owner::reset(SOCKET value) noexcept {
    if (value_ != INVALID_SOCKET) {
        closesocket(value_);
    }
    value_ = value;
}

cec_handle_owner::cec_handle_owner() noexcept : value_(nullptr) {}

cec_handle_owner::cec_handle_owner(HANDLE value) noexcept : value_(value) {}

cec_handle_owner::~cec_handle_owner() noexcept {
    reset();
}

cec_handle_owner::cec_handle_owner(cec_handle_owner&& other) noexcept : value_(other.release()) {}

cec_handle_owner& cec_handle_owner::operator=(cec_handle_owner&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

HANDLE cec_handle_owner::get() const noexcept {
    return value_;
}

HANDLE cec_handle_owner::release() noexcept {
    HANDLE value = value_;
    value_       = nullptr;
    return value;
}

void cec_handle_owner::reset(HANDLE value) noexcept {
    if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
        CloseHandle(value_);
    }
    value_ = value;
}

cec_virtual_arena_owner::cec_virtual_arena_owner() noexcept : value_(nullptr) {}

cec_virtual_arena_owner::cec_virtual_arena_owner(void* value) noexcept : value_(value) {}

cec_virtual_arena_owner::~cec_virtual_arena_owner() noexcept {
    reset();
}

cec_virtual_arena_owner::cec_virtual_arena_owner(cec_virtual_arena_owner&& other) noexcept : value_(other.release()) {}

cec_virtual_arena_owner& cec_virtual_arena_owner::operator=(cec_virtual_arena_owner&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

void* cec_virtual_arena_owner::get() const noexcept {
    return value_;
}

void* cec_virtual_arena_owner::release() noexcept {
    void* value = value_;
    value_      = nullptr;
    return value;
}

void cec_virtual_arena_owner::reset(void* value) noexcept {
    if (value_ != nullptr) {
        VirtualFree(value_, 0, MEM_RELEASE);
    }
    value_ = value;
}

cec_heap_owner::cec_heap_owner() noexcept : value_(nullptr) {}

cec_heap_owner::cec_heap_owner(void* value) noexcept : value_(value) {}

cec_heap_owner::~cec_heap_owner() noexcept {
    reset();
}

cec_heap_owner::cec_heap_owner(cec_heap_owner&& other) noexcept : value_(other.release()) {}

cec_heap_owner& cec_heap_owner::operator=(cec_heap_owner&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

void* cec_heap_owner::get() const noexcept {
    return value_;
}

void* cec_heap_owner::release() noexcept {
    void* value = value_;
    value_      = nullptr;
    return value;
}

void cec_heap_owner::reset(void* value) noexcept {
    if (value_ != nullptr) {
        HeapFree(GetProcessHeap(), 0, value_);
    }
    value_ = value;
}

cec_rio_registration_owner::cec_rio_registration_owner() noexcept : rio_(nullptr), value_(RIO_INVALID_BUFFERID) {}

cec_rio_registration_owner::cec_rio_registration_owner(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                                                       RIO_BUFFERID                        value) noexcept :
    rio_(rio),
    value_(value) {}

cec_rio_registration_owner::~cec_rio_registration_owner() noexcept {
    reset();
}

cec_rio_registration_owner::cec_rio_registration_owner(cec_rio_registration_owner&& other) noexcept :
    rio_(other.rio_),
    value_(other.release()) {
    other.rio_ = nullptr;
}

cec_rio_registration_owner& cec_rio_registration_owner::operator=(cec_rio_registration_owner&& other) noexcept {
    if (this != &other) {
        reset();
        rio_       = other.rio_;
        value_     = other.release();
        other.rio_ = nullptr;
    }
    return *this;
}

RIO_BUFFERID cec_rio_registration_owner::get() const noexcept {
    return value_;
}

RIO_BUFFERID cec_rio_registration_owner::release() noexcept {
    const RIO_BUFFERID value = value_;
    value_                   = RIO_INVALID_BUFFERID;
    return value;
}

void cec_rio_registration_owner::reset(const RIO_EXTENSION_FUNCTION_TABLE* rio, RIO_BUFFERID value) noexcept {
    if (value_ != RIO_INVALID_BUFFERID && rio_ != nullptr) {
        rio_->RIODeregisterBuffer(value_);
    }
    rio_   = rio;
    value_ = value;
}

bool cec_register_windows(const RIO_EXTENSION_FUNCTION_TABLE* rio,
                          char*                               memory,
                          std::size_t                         window_bytes,
                          std::uint32_t                       count,
                          cec_rio_registration_owner*         registrations) noexcept {
    if (rio == nullptr || rio->RIORegisterBuffer == nullptr || rio->RIODeregisterBuffer == nullptr ||
        memory == nullptr || registrations == nullptr || count == 0 || window_bytes == 0 ||
        window_bytes > std::numeric_limits<DWORD>::max() ||
        count > std::numeric_limits<std::size_t>::max() / window_bytes) {
        return false;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        const RIO_BUFFERID id = rio->RIORegisterBuffer(memory + static_cast<std::size_t>(index) * window_bytes,
                                                       static_cast<DWORD>(window_bytes));
        if (id == RIO_INVALID_BUFFERID) {
            const int error = WSAGetLastError();
            for (std::uint32_t prior = 0; prior < index; ++prior) {
                registrations[prior].reset();
            }
            WSASetLastError(error);
            return false;
        }
        registrations[index].reset(rio, id);
    }
    return true;
}

bool cec_prepare_send_window(RIO_BUF* buffer, std::size_t total, std::size_t offset) noexcept {
    if (buffer == nullptr || buffer->BufferId == RIO_INVALID_BUFFERID || total > std::numeric_limits<ULONG>::max() ||
        offset >= total) {
        return false;
    }
    buffer->Offset = static_cast<ULONG>(offset);
    buffer->Length = static_cast<ULONG>(total - offset);
    return true;
}

cec_rio_cq_owner::cec_rio_cq_owner() noexcept : rio_(nullptr), value_(RIO_INVALID_CQ) {}

cec_rio_cq_owner::cec_rio_cq_owner(const RIO_EXTENSION_FUNCTION_TABLE* rio, RIO_CQ value) noexcept :
    rio_(rio),
    value_(value) {}

cec_rio_cq_owner::~cec_rio_cq_owner() noexcept {
    reset();
}

cec_rio_cq_owner::cec_rio_cq_owner(cec_rio_cq_owner&& other) noexcept : rio_(other.rio_), value_(other.release()) {
    other.rio_ = nullptr;
}

cec_rio_cq_owner& cec_rio_cq_owner::operator=(cec_rio_cq_owner&& other) noexcept {
    if (this != &other) {
        reset();
        rio_       = other.rio_;
        value_     = other.release();
        other.rio_ = nullptr;
    }
    return *this;
}

RIO_CQ cec_rio_cq_owner::get() const noexcept {
    return value_;
}

RIO_CQ cec_rio_cq_owner::release() noexcept {
    const RIO_CQ value = value_;
    value_             = RIO_INVALID_CQ;
    return value;
}

void cec_rio_cq_owner::reset(const RIO_EXTENSION_FUNCTION_TABLE* rio, RIO_CQ value) noexcept {
    if (value_ != RIO_INVALID_CQ && rio_ != nullptr) {
        rio_->RIOCloseCompletionQueue(value_);
    }
    rio_   = rio;
    value_ = value;
}

bool cec_notification_packet_matches(ULONG_PTR         key,
                                     const OVERLAPPED* overlapped,
                                     ULONG_PTR         expected_key,
                                     const OVERLAPPED* expected_overlapped) noexcept {
    return key == expected_key && overlapped == expected_overlapped;
}

cec_rio_notify_outcome cec_rio_notify_outcome_of(int status) noexcept {
    if (status == ERROR_SUCCESS) {
        return cec_rio_notify_outcome::armed;
    }
    if (status == static_cast<int>(WSAEALREADY)) {
        return cec_rio_notify_outcome::duplicate_arm;
    }
    return cec_rio_notify_outcome::invalid;
}

void cec_engine_report(const wchar_t* stage, int error) noexcept {
    char text[160]{};
    (void) WideCharToMultiByte(CP_UTF8, 0, stage, -1, text, sizeof(text), nullptr, nullptr);
    std::fprintf(stderr, "%s failed: native_error=%d\n", text, error);
}

void cec_engine_session_socket_close(cec_native_slot* session) noexcept {
    session->owner->resources->session_sockets[session->index].reset();
    session->socket = INVALID_SOCKET;
}

static SOCKET cec_engine_registered_socket(int type, int protocol) noexcept {
    return WSASocketW(AF_INET, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_REGISTERED_IO);
}

bool cec_engine_load_extensions(RIO_EXTENSION_FUNCTION_TABLE* rio, LPFN_CONNECTEX* connect_ex) noexcept {
    cec_socket_owner probe_owner{ cec_engine_registered_socket(SOCK_STREAM, IPPROTO_TCP) };
    const SOCKET     probe = probe_owner.get();
    if (probe == INVALID_SOCKET) {
        cec_engine_report(L"WSASocketW(RIO probe)", WSAGetLastError());
        return false;
    }
    GUID  rio_identifier = WSAID_MULTIPLE_RIO;
    DWORD bytes          = 0;
    std::memset(rio, 0, sizeof(*rio));
    rio->cbSize = sizeof(*rio);
    if (WSAIoctl(probe, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER, &rio_identifier, sizeof(rio_identifier), rio,
                 sizeof(*rio), &bytes, nullptr, nullptr) != 0) {
        const int error = WSAGetLastError();
        cec_engine_report(L"SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER(RIO)", error);
        return false;
    }
    GUID connect_identifier = WSAID_CONNECTEX;
    bytes                   = 0;
    if (WSAIoctl(probe, SIO_GET_EXTENSION_FUNCTION_POINTER, &connect_identifier, sizeof(connect_identifier), connect_ex,
                 sizeof(*connect_ex), &bytes, nullptr, nullptr) != 0 ||
        *connect_ex == nullptr) {
        const int error = WSAGetLastError();
        cec_engine_report(L"SIO_GET_EXTENSION_FUNCTION_POINTER(ConnectEx)", error);
        return false;
    }
    return true;
}

static bool cec_engine_configure_socket(SOCKET socket_value, const cec_options* options, bool tcp) noexcept {
    if (options->socket_buffer_bytes != 0) {
        const int size = static_cast<int>(options->socket_buffer_bytes);
        if (setsockopt(socket_value, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof(size)) != 0 ||
            setsockopt(socket_value, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size), sizeof(size)) != 0) {
            cec_engine_report(L"setsockopt(SO_SNDBUF/SO_RCVBUF)", WSAGetLastError());
            return false;
        }
    }
    if (tcp) {
        const BOOL enabled = TRUE;
        if (setsockopt(socket_value, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled),
                       sizeof(enabled)) != 0) {
            cec_engine_report(L"setsockopt(TCP_NODELAY)", WSAGetLastError());
            return false;
        }
    }
    return true;
}

bool cec_engine_resolve_ipv4(const wchar_t* host, std::uint16_t port, SOCKADDR_IN* remote) noexcept {
    ADDRINFOW hints{};
    hints.ai_family    = AF_INET;
    hints.ai_socktype  = 0;
    hints.ai_protocol  = 0;
    ADDRINFOW* results = nullptr;
    wchar_t    service[16]{};
    _snwprintf_s(service, _countof(service), _TRUNCATE, L"%u", static_cast<unsigned>(port));
    const int                                            status = GetAddrInfoW(host, service, &hints, &results);
    std::unique_ptr<ADDRINFOW, decltype(&FreeAddrInfoW)> results_owner{ results, &FreeAddrInfoW };
    if (status != 0 || results == nullptr) {
        cec_engine_report(L"GetAddrInfoW(IPv4)", status);
        return false;
    }
    std::memcpy(remote, results->ai_addr, sizeof(*remote));
    return true;
}

static void cec_engine_arm(cec_native_worker* worker) noexcept {
    // Ownership check first: if a notification is still armed it may own the dedicated OVERLAPPED, so
    // the wrapper must refuse before it touches that storage. The shared transaction keeps its own
    // copy of the check as a second line of defence.
    if (worker->notification_armed) {
        cec_engine_fail_fast(L"client notification duplicate arm", ERROR_INVALID_STATE);
    }
    std::memset(&worker->notification_overlapped, 0, sizeof(worker->notification_overlapped));
    cec_notification_arm(worker->rio, worker->completion_queue, &worker->notification_armed, &worker->notify_arms,
                         L"RIONotify(client)");
}

// Lazy arm: the completion queue is armed exactly when RIO work is outstanding and no
// notification is pending, and every RIO post and drain calls it, so a missed arm would surface as
// blocked waits in the notification counters rather than as a hang.
void cec_engine_maybe_arm(cec_native_worker* worker) noexcept {
    if (!cec_notify_should_arm(worker->notification_armed, worker->rio_outstanding)) {
        return;
    }
    cec_engine_arm(worker);
}

std::uint64_t cec_engine_now() noexcept {
    LARGE_INTEGER value{};
    if (QueryPerformanceCounter(&value) == FALSE) {
        cec_engine_fail_fast(L"QueryPerformanceCounter(client timer)", static_cast<int>(GetLastError()));
    }
    return static_cast<ULONGLONG>(value.QuadPart);
}

static bool cec_engine_create_request_queue(cec_native_slot* session) noexcept {
    session->request_queue = session->owner->rio->RIOCreateRequestQueue(
        session->socket, CEC_RIO_MAX_OUTSTANDING_RECEIVES, 1U, CEC_RIO_MAX_OUTSTANDING_SENDS, 1U,
        session->owner->completion_queue, session->owner->completion_queue, session);
    if (session->request_queue == RIO_INVALID_RQ) {
        cec_engine_report(L"RIOCreateRequestQueue(client)", WSAGetLastError());
        return false;
    }
    return true;
}

bool cec_engine_post_send(cec_native_slot* session) noexcept {
    if (session->send_outstanding || session->send_request.token != 0) {
        cec_engine_fail_fast(L"client duplicate send post", ERROR_INVALID_STATE);
    }
    session->send_request.operation = cec_engine_operation::send;
    if (!cec_prepare_send_window(&session->send_buffer, session->attempt_bytes, session->send_offset)) {
        cec_engine_fail_fast(L"client send window", ERROR_INVALID_DATA);
    }
    if (static_cast<std::uint64_t>(session->index) > cec_operation_slot_mask) {
        cec_engine_fail_fast(L"client send operation slot", ERROR_INVALID_DATA);
    }
    const std::uint64_t sequence = cec_operation_next_sequence(session->send_request.sequence);
    const std::uint64_t token =
        cec_operation_token_encode(session->index, cec_engine_operation::send, session->generation, sequence);
    void* const context = reinterpret_cast<void*>(static_cast<std::uintptr_t>(token));
    if (session->owner->rio->RIOSend(session->request_queue, &session->send_buffer, 1, 0, context) == FALSE) {
        cec_engine_report(L"RIOSend(client)", WSAGetLastError());
        return false;
    }
    session->send_request.sequence      = static_cast<std::uint32_t>(sequence);
    session->send_request.token         = token;
    session->send_request.posted_length = session->send_buffer.Length;
    session->send_outstanding           = true;
    ++session->outstanding;
    ++session->rio_outstanding;
    ++session->owner->rio_outstanding;
    cec_engine_maybe_arm(session->owner);
    return true;
}

bool cec_engine_post_receive(cec_native_slot* session, std::size_t bytes) noexcept {
    if (session->receive_outstanding || session->receive_request.token != 0) {
        cec_engine_fail_fast(L"client duplicate receive post", ERROR_INVALID_STATE);
    }
    if (!cec_native_slot_drained(session) || bytes == 0 || bytes > session->owner->maximum_attempt_bytes) {
        cec_engine_fail_fast(L"client attempt post precondition", ERROR_IO_INCOMPLETE);
    }
    session->attempt_bytes             = bytes;
    session->send_offset               = 0;
    session->received_bytes            = 0;
    session->receive_request.operation = cec_engine_operation::receive;
    session->receive_buffer.Offset     = 0;
    session->receive_buffer.Length     = static_cast<ULONG>(session->attempt_bytes);
    const DWORD flags                  = session->owner->options->protocol == cec_protocol::tcp ? RIO_MSG_WAITALL : 0U;
    if (static_cast<std::uint64_t>(session->index) > cec_operation_slot_mask) {
        cec_engine_fail_fast(L"client receive operation slot", ERROR_INVALID_DATA);
    }
    const std::uint64_t sequence = cec_operation_next_sequence(session->receive_request.sequence);
    const std::uint64_t token =
        cec_operation_token_encode(session->index, cec_engine_operation::receive, session->generation, sequence);
    void* const context = reinterpret_cast<void*>(static_cast<std::uintptr_t>(token));
    if (session->owner->rio->RIOReceive(session->request_queue, &session->receive_buffer, 1, flags, context) == FALSE) {
        cec_engine_report(L"RIOReceive(client)", WSAGetLastError());
        return false;
    }
    session->receive_request.sequence      = static_cast<std::uint32_t>(sequence);
    session->receive_request.token         = token;
    session->receive_request.posted_length = session->receive_buffer.Length;
    session->receive_outstanding           = true;
    ++session->outstanding;
    ++session->rio_outstanding;
    ++session->owner->rio_outstanding;
    cec_engine_maybe_arm(session->owner);
    return true;
}

bool cec_engine_winsock::start() noexcept {
    WSADATA data{};
    error   = WSAStartup(MAKEWORD(2, 2), &data);
    started = error == ERROR_SUCCESS;
    return started;
}

cec_engine_winsock::~cec_engine_winsock() noexcept {
    if (started) {
        WSACleanup();
    }
}

bool cec_engine_timer_resolution::start() noexcept {
    error  = timeBeginPeriod(1U);
    active = error == TIMERR_NOERROR;
    return active;
}

cec_engine_timer_resolution::~cec_engine_timer_resolution() noexcept {
    if (active) {
        (void) timeEndPeriod(1U);
    }
}

std::uint32_t cec_resolve_worker_count(const cec_options* options) noexcept {
    return cec_resolved_worker_count(options->worker_count,
                                     static_cast<std::uint32_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)),
                                     options->session_count);
}

std::uint64_t cec_require_latency_microseconds(std::uint64_t ticks, std::uint64_t frequency) noexcept {
    if (frequency == 0) {
        cec_engine_fail_fast(L"client latency frequency", ERROR_INVALID_DATA);
    }
    return cec_engine_ticks_to_microseconds(ticks, frequency);
}

bool cec_native_connect_slot(cec_native_slot* session) noexcept {
    if (session->outstanding != 0 || session->rio_outstanding != 0 || session->connect_outstanding ||
        session->send_outstanding || session->receive_outstanding || session->receive_request.token != 0 ||
        session->send_request.token != 0) {
        cec_engine_fail_fast(L"client generation reuse precondition", ERROR_IO_INCOMPLETE);
    }
    // The slot's only reuse point, so it is where the generation changes. Every submission carries the
    // generation and its own serial inside the request context value, so a completion of the previous
    // generation can still be told apart after this record has been rewritten for the new one. The tag
    // wraps to one at the limit: it is an identity, not an ownership ledger.
    session->generation    = session->generation == UINT32_MAX ? 1U : session->generation + 1U;
    session->closing       = false;
    session->request_queue = RIO_INVALID_RQ;
    const bool tcp         = session->owner->options->protocol == cec_protocol::tcp;
    session->socket = cec_engine_registered_socket(tcp ? SOCK_STREAM : SOCK_DGRAM, tcp ? IPPROTO_TCP : IPPROTO_UDP);
    session->owner->resources->session_sockets[session->index].reset(session->socket);
    if (session->socket == INVALID_SOCKET ||
        !cec_engine_configure_socket(session->socket, session->owner->options, tcp)) {
        cec_engine_report(L"client socket creation", WSAGetLastError());
        return false;
    }
    SOCKADDR_IN local{};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port        = htons(session->owner->options->local_port);
    if (bind(session->socket, reinterpret_cast<const SOCKADDR*>(&local), sizeof(local)) != 0) {
        cec_engine_report(L"bind(client)", WSAGetLastError());
        return false;
    }
    if (!tcp) {
        if (connect(session->socket, reinterpret_cast<const SOCKADDR*>(session->owner->remote_address),
                    sizeof(*session->owner->remote_address)) != 0 ||
            !cec_engine_create_request_queue(session)) {
            cec_engine_report(L"connect/RQ(UDP client)", WSAGetLastError());
            return false;
        }
        return true;
    }

    if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(session->socket), session->owner->port,
                               static_cast<ULONG_PTR>(reinterpret_cast<std::uintptr_t>(session)),
                               0) != session->owner->port) {
        cec_engine_report(L"CreateIoCompletionPort(ConnectEx socket)", static_cast<int>(GetLastError()));
        return false;
    }
    std::memset(&session->connect_overlapped, 0, sizeof(session->connect_overlapped));
    const BOOL connected = session->owner->connect_ex(
        session->socket, reinterpret_cast<const SOCKADDR*>(session->owner->remote_address),
        sizeof(*session->owner->remote_address), nullptr, 0, nullptr, &session->connect_overlapped);
    const int connect_error = connected == FALSE ? WSAGetLastError() : ERROR_SUCCESS;
    if (connected == FALSE && connect_error != ERROR_IO_PENDING) {
        cec_engine_report(L"ConnectEx", connect_error);
        return false;
    }
    session->connect_outstanding = true;
    ++session->outstanding;
    return true;
}

bool cec_native_slot_drained(const cec_native_slot* slot) noexcept {
    return slot->outstanding == 0 && slot->rio_outstanding == 0 && !slot->connect_outstanding &&
           !slot->send_outstanding && !slot->receive_outstanding;
}

void cec_native_close_slot(cec_native_slot* slot) noexcept {
    slot->closing = true;
    cec_engine_session_socket_close(slot);
    // Closing initiates cancellation; only retire_* returns provider ownership.
    if (cec_native_slot_drained(slot)) {
        slot->request_queue = RIO_INVALID_RQ;
    }
}

cec_completion cec_native_retire_rio(cec_native_worker* worker, const RIORESULT& result) noexcept {
    const std::uint64_t          token    = static_cast<std::uint64_t>(result.RequestContext);
    const cec_operation_identity identity = cec_operation_token_decode(token, L"client RIO RequestContext");
    if (worker->slots == nullptr || identity.slot >= worker->session_count) {
        cec_engine_fail_fast(L"client RIO RequestContext", ERROR_INVALID_DATA);
    }
    cec_native_slot* slot = &worker->slots[identity.slot];
    if (slot->owner != worker || slot->index != identity.slot) {
        cec_engine_fail_fast(L"client RIO RequestContext", ERROR_INVALID_DATA);
    }
    if (static_cast<std::uintptr_t>(result.SocketContext) != reinterpret_cast<std::uintptr_t>(slot)) {
        cec_engine_fail_fast(L"client RIO SocketContext identity", ERROR_INVALID_DATA);
    }
    cec_engine_request* lane =
        identity.operation == cec_engine_operation::send ? &slot->send_request : &slot->receive_request;
    // The token is the identity the provider carried back; the lane holds the identity of the operation
    // this slot believes is posted. A completion of an earlier generation fails here even though the
    // record it used to point at has since been rewritten for the new generation.
    if (lane->token != token) {
        cec_engine_fail_fast(L"client completion identity", ERROR_INVALID_DATA);
    }
    if ((slot->generation & static_cast<std::uint32_t>(cec_operation_generation_mask)) != identity.generation) {
        cec_engine_fail_fast(L"client completion generation", ERROR_IO_INCOMPLETE);
    }
    if (slot->outstanding == 0 || slot->rio_outstanding == 0 || worker->rio_outstanding == 0) {
        cec_engine_fail_fast(L"client RIO outstanding count", ERROR_INVALID_DATA);
    }
    bool& posted =
        identity.operation == cec_engine_operation::send ? slot->send_outstanding : slot->receive_outstanding;
    if (!posted) {
        cec_engine_fail_fast(L"client duplicate RIO completion", ERROR_INVALID_DATA);
    }
    if (result.BytesTransferred > lane->posted_length) {
        cec_engine_fail_fast(L"client completion exceeds posted range", ERROR_INVALID_DATA);
    }
    // Validate the window before retiring its completion, including the subtraction below.
    if (identity.operation == cec_engine_operation::send && slot->send_offset > slot->attempt_bytes) {
        cec_engine_fail_fast(L"client send window", ERROR_INVALID_DATA);
    }
    lane->token         = 0;
    lane->posted_length = 0;
    posted              = false;
    --slot->outstanding;
    --slot->rio_outstanding;
    --worker->rio_outstanding;
    if (slot->closing) {
        if (cec_native_slot_drained(slot)) {
            slot->request_queue = RIO_INVALID_RQ;
            return { cec_completion_kind::drained, slot->index };
        }
        return { cec_completion_kind::ignored, slot->index };
    }
    if (result.Status != ERROR_SUCCESS) {
        return { cec_completion_kind::failed, slot->index };
    }
    if (identity.operation == cec_engine_operation::send) {
        if (result.BytesTransferred == 0 || result.BytesTransferred > slot->attempt_bytes - slot->send_offset) {
            return { cec_completion_kind::failed, slot->index };
        }
        slot->send_offset += result.BytesTransferred;
        return { slot->send_offset == slot->attempt_bytes ? cec_completion_kind::sent :
                                                            cec_completion_kind::send_progress,
                 slot->index, result.BytesTransferred };
    }
    slot->received_bytes     = result.BytesTransferred;
    const char* send_data    = worker->memory + slot->index * 2U * worker->maximum_attempt_bytes;
    const char* receive_data = send_data + worker->maximum_attempt_bytes;
    const bool  equal =
        slot->received_bytes == slot->attempt_bytes && std::memcmp(send_data, receive_data, slot->attempt_bytes) == 0;
    return { cec_completion_kind::received, slot->index, result.BytesTransferred, equal };
}

cec_completion cec_native_retire_connect(cec_native_slot* slot, BOOL ok, DWORD error, DWORD transferred) noexcept {
    if (slot->outstanding == 0 || !slot->connect_outstanding) {
        cec_engine_fail_fast(L"ConnectEx outstanding count", ERROR_INVALID_DATA);
    }
    if (transferred != 0) {
        cec_engine_fail_fast(L"ConnectEx completion exceeds posted range", ERROR_INVALID_DATA);
    }
    slot->connect_outstanding = false;
    --slot->outstanding;
    if (slot->closing) {
        if (cec_native_slot_drained(slot)) {
            slot->request_queue = RIO_INVALID_RQ;
            return { cec_completion_kind::drained, slot->index };
        }
        return { cec_completion_kind::ignored, slot->index };
    }
    if (ok == FALSE || setsockopt(slot->socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) != 0) {
        cec_engine_report(L"ConnectEx completion", ok == FALSE ? static_cast<int>(error) : WSAGetLastError());
        return { cec_completion_kind::failed, slot->index };
    }
    if (!cec_engine_create_request_queue(slot)) {
        return { cec_completion_kind::failed, slot->index };
    }
    return { cec_completion_kind::connected, slot->index };
}

void cec_native_require_release(const cec_native_worker* transport) noexcept {
    // This guard is also called by the resource destructor before any owner member destructs.
    if (!transport->joined || transport->rio_outstanding != 0) {
        cec_engine_fail_fast(L"client worker release precondition", ERROR_IO_INCOMPLETE);
    }
    if (transport->slots != nullptr) {
        for (std::uint32_t index = 0; index < transport->session_count; ++index) {
            if (!cec_native_slot_drained(&transport->slots[index])) {
                cec_engine_fail_fast(L"client worker release precondition", ERROR_IO_INCOMPLETE);
            }
        }
    }
    if (transport->notify_arms < transport->notify_deliveries ||
        transport->notify_arms - transport->notify_deliveries > 1U) {
        cec_engine_fail_fast(L"client notification accounting", ERROR_INVALID_STATE);
    }
}

cec_engine_worker_resources::~cec_engine_worker_resources() noexcept {
    if (owner != nullptr) {
        cec_native_require_release(owner);
    }
    // CQ, registered buffers and arena must disappear before their IOCP and pinned notify owner.
    completion_queue.reset();
    registrations.reset();
    arena.reset();
    session_sockets.reset();
    slots.reset();
    sessions.reset();
    timer_nodes.reset();
    timer_positions.reset();
    thread.reset();
    port.reset();
    if (owner != nullptr) {
        owner->completion_queue = RIO_INVALID_CQ;
        owner->memory           = nullptr;
        owner->slots            = nullptr;
        owner->thread           = nullptr;
        owner->port             = nullptr;
    }
}
