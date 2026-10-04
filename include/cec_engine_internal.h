#pragma once

#include "cec_types.h"

// WinSock2.h must be included before the Windows networking headers below.
// clang-format off
#include <WinSock2.h>
#include <MSWSock.h>
#include <WS2tcpip.h>
#include <Windows.h>
// clang-format on

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

enum class cec_worker_phase : std::uint8_t { starting = 0, running = 1, draining = 2, stopped = 3 };

struct cec_worker_lifecycle {
    cec_worker_phase phase;
    std::uint32_t    live_sessions;
    std::uint32_t    total_outstanding;
    bool             notification_armed;
};

struct cec_timer_node {
    ULONGLONG     deadline;
    std::uint32_t session_index;
};

struct cec_timer_heap {
    cec_timer_node* nodes;
    std::uint32_t*  positions;
    std::uint32_t   size;
    std::uint32_t   capacity;
};

enum class cec_engine_operation : std::uint8_t { receive, send };

enum class cec_engine_state : std::uint8_t { dormant, connecting, active, pacing, reconnecting, closing, done };

struct cec_engine_worker;
struct cec_engine_session;
class cec_engine_worker_resources;

struct cec_engine_request {
    cec_engine_session*  session;
    cec_engine_operation operation;
};

struct cec_engine_metrics {
    std::atomic<std::uint64_t>                 claimed;
    std::atomic<std::uint64_t>                 echoed;
    std::atomic<std::uint64_t>                 corrupted;
    std::atomic<std::uint64_t>                 lost;
    std::atomic<std::uint64_t>                 bytes;
    std::atomic<std::uint64_t>                 network_errors;
    std::array<std::atomic<std::uint64_t>, 64> latency_bins;
};

struct cec_engine_session {
    cec_engine_worker* owner;
    SOCKET             socket;
    RIO_RQ             request_queue;
    OVERLAPPED         connect_overlapped;
    cec_engine_request receive_request;
    cec_engine_request send_request;
    RIO_BUF            receive_buffer;
    RIO_BUF            send_buffer;
    cec_engine_state   state;
    std::uint32_t      index;
    std::uint32_t      outstanding;
    std::uint64_t      requested_echoes;
    std::uint64_t      remaining_echoes;
    std::size_t        attempt_bytes;
    std::size_t        send_offset;
    std::size_t        received_bytes;
    ULONGLONG          deadline;
    ULONGLONG          next_action;
    LARGE_INTEGER      started_at;
    bool               send_done;
    bool               receive_done;
    bool               attempt_accounted;
    bool               reconnect_after_close;
};

struct cec_engine_worker {
    cec_engine_worker_resources*        resources;
    const RIO_EXTENSION_FUNCTION_TABLE* rio;
    LPFN_CONNECTEX                      connect_ex;
    const cec_options*                  options;
    const SOCKADDR_IN*                  remote_address;
    const std::byte*                    pattern;
    std::size_t                         pattern_bytes;
    std::size_t                         maximum_attempt_bytes;
    cec_engine_metrics*                 metrics;
    std::atomic<bool>*                  external_stop;
    std::atomic<bool>*                  fatal;
    HANDLE                              port;
    HANDLE                              thread;
    OVERLAPPED                          notification_overlapped;
    RIO_CQ                              completion_queue;
    RIO_BUFFERID                        registration;
    char*                               memory;
    cec_engine_session*                 sessions;
    cec_timer_node*                     timer_nodes;
    std::uint32_t*                      timer_positions;
    cec_timer_heap                      timers;
    std::uint32_t                       session_count;
    std::uint32_t                       worker_index;
    std::uint32_t                       live_sessions;
    LARGE_INTEGER                       performance_frequency;
    bool                                notification_armed;
    bool                                stopping;
};

static_assert(std::is_trivial_v<cec_worker_lifecycle>);
static_assert(std::is_standard_layout_v<cec_worker_lifecycle>);
static_assert(std::is_trivially_copyable_v<cec_worker_lifecycle>);
static_assert(std::is_trivial_v<cec_timer_node>);
static_assert(std::is_standard_layout_v<cec_timer_node>);
static_assert(std::is_trivially_copyable_v<cec_timer_node>);
static_assert(std::is_trivial_v<cec_timer_heap>);
static_assert(std::is_standard_layout_v<cec_timer_heap>);
static_assert(std::is_trivially_copyable_v<cec_timer_heap>);

class cec_socket_owner {
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

class cec_handle_owner {
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

class cec_virtual_arena_owner {
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

class cec_heap_owner {
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

class cec_rio_registration_owner {
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

class cec_rio_cq_owner {
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

class cec_engine_worker_resources {
  public:
    cec_handle_owner                    port;
    cec_handle_owner                    thread;
    cec_virtual_arena_owner             arena;
    cec_rio_registration_owner          registration;
    cec_rio_cq_owner                    completion_queue;
    cec_heap_owner                      sessions;
    cec_heap_owner                      timer_nodes;
    cec_heap_owner                      timer_positions;
    std::unique_ptr<cec_socket_owner[]> session_sockets;
};

[[noreturn]] void cec_engine_fail_fast(const wchar_t* stage, int error) noexcept;
void              cec_require_rio_notify_success(int status, const wchar_t* stage) noexcept;
ULONG             cec_require_valid_dequeue_count(ULONG count, const wchar_t* stage) noexcept;
bool              cec_worker_may_release(const cec_worker_lifecycle* lifecycle) noexcept;
bool              cec_session_terminal_accounting_valid(std::uint64_t claimed,
                                                        std::uint64_t echoed,
                                                        std::uint64_t corrupted,
                                                        std::uint64_t lost) noexcept;
bool              cec_notification_packet_matches(ULONG_PTR         key,
                                                  const OVERLAPPED* overlapped,
                                                  ULONG_PTR         expected_key,
                                                  const OVERLAPPED* expected_overlapped) noexcept;
bool              cec_timer_initialize(cec_timer_heap* heap,
                                       cec_timer_node* nodes,
                                       std::uint32_t*  positions,
                                       std::uint32_t   capacity) noexcept;
bool  cec_timer_insert_or_update(cec_timer_heap* heap, std::uint32_t session_index, ULONGLONG deadline) noexcept;
bool  cec_timer_remove(cec_timer_heap* heap, std::uint32_t session_index) noexcept;
bool  cec_timer_pop_expired(cec_timer_heap* heap, ULONGLONG now, std::uint32_t* session_index) noexcept;
DWORD cec_timer_wait_milliseconds(const cec_timer_heap* heap, ULONGLONG now) noexcept;
void  cec_fill_repeated_pattern(std::byte*       destination,
                                std::size_t      destination_size,
                                const std::byte* pattern,
                                std::size_t      pattern_size) noexcept;
