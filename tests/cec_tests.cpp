#include "cec_types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <limits>

static int cec_test_failures = 0;

static void cec_test_expect(bool condition, const char* name) noexcept {
    if (condition) {
        std::printf("PASS %s\n", name);
        return;
    }
    std::fprintf(stderr, "FAIL %s\n", name);
    ++cec_test_failures;
}

static wchar_t* cec_test_arg(const wchar_t* value) noexcept {
    return const_cast<wchar_t*>(value);
}

static void cec_test_parser() noexcept {
    std::array<wchar_t*, 17> args{ cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"),
                                   cec_test_arg(L"tcp"),    cec_test_arg(L"/r"),        cec_test_arg(L"4578"),
                                   cec_test_arg(L"/n"),     cec_test_arg(L"1000"),      cec_test_arg(L"/k"),
                                   cec_test_arg(L"8"),      cec_test_arg(L"/z"),        cec_test_arg(L"64"),
                                   cec_test_arg(L"/c"),     cec_test_arg(L"32"),        cec_test_arg(L"/threads"),
                                   cec_test_arg(L"4"),      cec_test_arg(L"/q") };
    cec_options              options{};
    std::array<wchar_t, CEC_ERROR_CAPACITY> error{};
    const bool                              parsed =
        cec_parse_options(static_cast<int>(args.size()), args.data(), &options, error.data(), error.size());
    cec_test_expect(parsed && options.protocol == cec_protocol::tcp && options.remote_port == 4578 &&
                        options.echo_count == 1000 && options.pipeline_depth == 8 && options.pattern_bytes == 64 &&
                        options.session_count == 32 && options.worker_count == 4 && options.quiet,
                    "client parses concurrent TCP workload");

    std::array<wchar_t*, 7> invalid{ cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"),
                                     cec_test_arg(L"udp"),    cec_test_arg(L"/k"),        cec_test_arg(L"2"),
                                     cec_test_arg(L"/q") };
    cec_options             rejected{};
    error.fill(L'\0');
    cec_test_expect(
        !cec_parse_options(static_cast<int>(invalid.size()), invalid.data(), &rejected, error.data(), error.size()) &&
            std::wcsstr(error.data(), L"TCP") != nullptr,
        "client rejects TCP pipeline depth for UDP");

    wchar_t                 literal[] = L"owned-literal";
    std::array<wchar_t*, 6> owned_args{ cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/P"),
                                        cec_test_arg(L"TCP"),    cec_test_arg(L"/d"),        literal };
    cec_options             owned{};
    error.fill(L'\0');
    const bool owned_parsed =
        cec_parse_options(static_cast<int>(owned_args.size()), owned_args.data(), &owned, error.data(), error.size());
    literal[0] = L'X';
    cec_test_expect(owned_parsed && std::wcscmp(owned.literal_pattern, L"owned-literal") == 0,
                    "client owns literal text and folds ASCII switch case");

    std::array<wchar_t*, 6> unknown_args{ cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"),
                                          cec_test_arg(L"tcp"),    cec_test_arg(L"/foo"),      cec_test_arg(L"1") };
    error.fill(L'\0');
    cec_test_expect(!cec_parse_options(static_cast<int>(unknown_args.size()), unknown_args.data(), &rejected,
                                       error.data(), error.size()) &&
                        std::wcsstr(error.data(), L"unknown switch") != nullptr,
                    "client reports unknown switch before parsing its value");

    std::array<wchar_t*, 5> empty_args{ cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"),
                                        cec_test_arg(L"tcp"), cec_test_arg(L"/r=") };
    error.fill(L'\0');
    cec_test_expect(!cec_parse_options(static_cast<int>(empty_args.size()), empty_args.data(), &rejected, error.data(),
                                       error.size()),
                    "client rejects empty inline values");

    std::array<wchar_t*, 7> followed_empty_args{ cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"),
                                                 cec_test_arg(L"/p"),     cec_test_arg(L"tcp"),
                                                 cec_test_arg(L"/r="),    cec_test_arg(L"7001"),
                                                 cec_test_arg(L"/q") };
    error.fill(L'\0');
    cec_test_expect(!cec_parse_options(static_cast<int>(followed_empty_args.size()), followed_empty_args.data(),
                                       &rejected, error.data(), error.size()),
                    "client empty inline value never consumes the following token");

    std::array<wchar_t*, 8> tcp_reconnect_args{ cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"),
                                                cec_test_arg(L"/p"),     cec_test_arg(L"tcp"),
                                                cec_test_arg(L"/l"),     cec_test_arg(L"7001"),
                                                cec_test_arg(L"/rc"),    cec_test_arg(L"1") };
    error.fill(L'\0');
    cec_test_expect(!cec_parse_options(static_cast<int>(tcp_reconnect_args.size()), tcp_reconnect_args.data(),
                                       &rejected, error.data(), error.size()),
                    "client rejects TCP reconnect with fixed local port");

    std::array<wchar_t*, 8> udp_reconnect_args{ cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"),
                                                cec_test_arg(L"/p"),     cec_test_arg(L"udp"),
                                                cec_test_arg(L"/l"),     cec_test_arg(L"7001"),
                                                cec_test_arg(L"/rc"),    cec_test_arg(L"1") };
    error.fill(L'\0');
    cec_test_expect(cec_parse_options(static_cast<int>(udp_reconnect_args.size()), udp_reconnect_args.data(), &rejected,
                                      error.data(), error.size()),
                    "client allows UDP reconnect with fixed local port");

    std::array<wchar_t*, 8> tcp_fixed_port_many_sessions{ cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"),
                                                          cec_test_arg(L"/p"),     cec_test_arg(L"tcp"),
                                                          cec_test_arg(L"/l"),     cec_test_arg(L"45000"),
                                                          cec_test_arg(L"/c"),     cec_test_arg(L"2") };
    error.fill(L'\0');
    cec_test_expect(!cec_parse_options(static_cast<int>(tcp_fixed_port_many_sessions.size()),
                                       tcp_fixed_port_many_sessions.data(), &rejected, error.data(), error.size()) &&
                        std::wcscmp(error.data(), L"a fixed /l port requires /c 1") == 0,
                    "client parser rejects fixed TCP port with multiple sessions");

    std::array<wchar_t*, 8> udp_fixed_port_many_sessions{ cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"),
                                                          cec_test_arg(L"/p"),     cec_test_arg(L"udp"),
                                                          cec_test_arg(L"/l"),     cec_test_arg(L"45000"),
                                                          cec_test_arg(L"/c"),     cec_test_arg(L"2") };
    error.fill(L'\0');
    cec_test_expect(!cec_parse_options(static_cast<int>(udp_fixed_port_many_sessions.size()),
                                       udp_fixed_port_many_sessions.data(), &rejected, error.data(), error.size()) &&
                        std::wcscmp(error.data(), L"a fixed /l port requires /c 1") == 0,
                    "client parser rejects fixed UDP port with multiple sessions");

    std::array<wchar_t*, 8> fixed_port_one_session{ cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"),
                                                    cec_test_arg(L"/p"),     cec_test_arg(L"tcp"),
                                                    cec_test_arg(L"/l"),     cec_test_arg(L"45000"),
                                                    cec_test_arg(L"/c"),     cec_test_arg(L"1") };
    error.fill(L'\0');
    cec_test_expect(cec_parse_options(static_cast<int>(fixed_port_one_session.size()), fixed_port_one_session.data(),
                                      &rejected, error.data(), error.size()),
                    "client parser accepts fixed port with one session");

    std::array<wchar_t*, 9> fixed_port_conflict_with_help{ cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"),
                                                           cec_test_arg(L"/p"),     cec_test_arg(L"tcp"),
                                                           cec_test_arg(L"/l"),     cec_test_arg(L"45000"),
                                                           cec_test_arg(L"/c"),     cec_test_arg(L"2"),
                                                           cec_test_arg(L"/h") };
    error.fill(L'\0');
    cec_test_expect(!cec_parse_options(static_cast<int>(fixed_port_conflict_with_help.size()),
                                       fixed_port_conflict_with_help.data(), &rejected, error.data(), error.size()) &&
                        std::wcscmp(error.data(), L"a fixed /l port requires /c 1") == 0,
                    "client help does not suppress a fixed-port session conflict");
}

static void cec_test_quota_and_cq_options() noexcept {
    std::array<wchar_t*, 14> args{ cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"),
                                   cec_test_arg(L"tcp"),    cec_test_arg(L"/n"),        cec_test_arg(L"1"),
                                   cec_test_arg(L"/c"),     cec_test_arg(L"64"),        cec_test_arg(L"/threads"),
                                   cec_test_arg(L"2"),      cec_test_arg(L"/cq"),       cec_test_arg(L"64"),
                                   cec_test_arg(L"/z"),     cec_test_arg(L"1") };
    cec_options              options{};
    std::array<wchar_t, CEC_ERROR_CAPACITY> error{};
    cec_test_expect(cec_parse_options(static_cast<int>(args.size()), args.data(), &options, error.data(), error.size()),
                    "client CQ capacity applies to each worker, not all sessions");
    args[7] = cec_test_arg(L"65");
    cec_test_expect(
        !cec_parse_options(static_cast<int>(args.size()), args.data(), &options, error.data(), error.size()),
        "client CQ capacity includes the larger uneven session shard");
    args[7] = cec_test_arg(L"64");
    args[9] = cec_test_arg(L"1");
    cec_test_expect(
        !cec_parse_options(static_cast<int>(args.size()), args.data(), &options, error.data(), error.size()),
        "client CQ capacity still rejects an undersized single worker");

    args[7] = cec_test_arg(L"2");
    args[5] = cec_test_arg(L"9223372036854775809");
    cec_test_expect(
        !cec_parse_options(static_cast<int>(args.size()), args.data(), &options, error.data(), error.size()),
        "client rejects a finite quota product that wraps to a smaller quota");
    args[5] = cec_test_arg(L"9223372036854775808");
    cec_test_expect(
        !cec_parse_options(static_cast<int>(args.size()), args.data(), &options, error.data(), error.size()),
        "client rejects a finite quota product that wraps to unlimited");
    args[5] = cec_test_arg(L"9223372036854775807");
    cec_test_expect(cec_parse_options(static_cast<int>(args.size()), args.data(), &options, error.data(), error.size()),
                    "client accepts the largest representable two-session quota");
    args[7] = cec_test_arg(L"1");
    args[5] = cec_test_arg(L"18446744073709551615");
    cec_test_expect(cec_parse_options(static_cast<int>(args.size()), args.data(), &options, error.data(), error.size()),
                    "client accepts the maximum one-session quota");
    args[7] = cec_test_arg(L"64");
    args[9] = cec_test_arg(L"2");
    args[5] = cec_test_arg(L"0");
    cec_test_expect(cec_parse_options(static_cast<int>(args.size()), args.data(), &options, error.data(), error.size()),
                    "client unlimited sessions still use worker-local CQ capacity");
}

static void cec_test_capacity() noexcept {
    std::size_t value = 0;
    cec_test_expect(cec_checked_product(8, 65536, &value) && value == 524288,
                    "client checked batch size accepts valid values");
    cec_test_expect(!cec_checked_product(std::numeric_limits<std::size_t>::max(), 2, &value),
                    "client checked batch size rejects overflow");
    cec_test_expect(cec_checked_storage_bytes(32, 4096, 1048576, &value) && value == 262144,
                    "client storage includes send and receive buffers");
    cec_test_expect(!cec_checked_storage_bytes(32, 4096, 131072, &value), "client storage rejects memory limit excess");
}

static void cec_test_cq_batch_reservation() noexcept {
    std::array<wchar_t*, 16> args{ cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"),
                                   cec_test_arg(L"tcp"),    cec_test_arg(L"/n"),        cec_test_arg(L"0"),
                                   cec_test_arg(L"/c"),     cec_test_arg(L"64"),        cec_test_arg(L"/threads"),
                                   cec_test_arg(L"2"),      cec_test_arg(L"/cq"),       cec_test_arg(L"64"),
                                   cec_test_arg(L"/z"),     cec_test_arg(L"1"),         cec_test_arg(L"/k"),
                                   cec_test_arg(L"128") };
    cec_options              options{};
    std::array<wchar_t, CEC_ERROR_CAPACITY> error{};
    cec_test_expect(
        cec_parse_options(static_cast<int>(args.size()), args.data(), &options, error.data(), error.size()) &&
            options.pipeline_depth == 128 && cec_resolve_worker_count(&options) == 2,
        "client CQ reserves one send and one receive per batch session irrespective of pipeline depth");
    args[7] = cec_test_arg(L"65");
    cec_test_expect(
        !cec_parse_options(static_cast<int>(args.size()), args.data(), &options, error.data(), error.size()) &&
            std::wcsstr(error.data(), L"completion queue") != nullptr,
        "client high-depth batches still reject a CQ below twice the largest shard");
    args[7]  = cec_test_arg(L"64");
    args[11] = cec_test_arg(L"66");
    cec_test_expect(cec_parse_options(static_cast<int>(args.size()), args.data(), &options, error.data(), error.size()),
                    "client CQ permits spare entries beyond the exact batch reservation");
}

static void cec_test_resource_validation() noexcept {
    cec_options                             options{};
    std::array<wchar_t, CEC_ERROR_CAPACITY> error{};
    const auto                              parse = [&](auto& args, int omitted_tail = 0) noexcept {
        error.fill(L'\0');
        return cec_parse_options(static_cast<int>(args.size()) - omitted_tail, args.data(), &options, error.data(),
                                                              error.size());
    };
    std::array<wchar_t*, 17> memory{ cec_test_arg(L"client"),  cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"),
                                     cec_test_arg(L"tcp"),     cec_test_arg(L"/z"),        cec_test_arg(L"8192"),
                                     cec_test_arg(L"/k"),      cec_test_arg(L"1"),         cec_test_arg(L"/c"),
                                     cec_test_arg(L"64"),      cec_test_arg(L"/threads"),  cec_test_arg(L"2"),
                                     cec_test_arg(L"/memory"), cec_test_arg(L"1048576"),   cec_test_arg(L"/cq"),
                                     cec_test_arg(L"64"),      cec_test_arg(L"/h") };
    cec_test_expect(parse(memory, 1) && parse(memory) && options.help,
                    "client memory accepts exact send-plus-receive storage with and without help");
    memory[5] = cec_test_arg(L"8193");
    cec_test_expect(!parse(memory, 1) && !parse(memory) && std::wcsstr(error.data(), L"/memory") != nullptr,
                    "client memory rejects both directions beyond the budget before help");

    std::array<wchar_t*, 15> batch{ cec_test_arg(L"client"),  cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"),
                                    cec_test_arg(L"tcp"),     cec_test_arg(L"/z"),        cec_test_arg(L"33554432"),
                                    cec_test_arg(L"/k"),      cec_test_arg(L"2"),         cec_test_arg(L"/c"),
                                    cec_test_arg(L"1"),       cec_test_arg(L"/threads"),  cec_test_arg(L"1"),
                                    cec_test_arg(L"/memory"), cec_test_arg(L"134217728"), cec_test_arg(L"/h") };
    cec_test_expect(parse(batch, 1) && parse(batch), "client TCP batch accepts the exact 64 MiB limit before help");
    batch[5] = cec_test_arg(L"33554433");
    cec_test_expect(!parse(batch, 1) && !parse(batch) && std::wcsstr(error.data(), L"64 MiB") != nullptr,
                    "client TCP batch rejects an oversized payload-depth product before help");

    std::array<wchar_t*, 17> arena{ cec_test_arg(L"client"),  cec_test_arg(L"127.0.0.1"),  cec_test_arg(L"/p"),
                                    cec_test_arg(L"tcp"),     cec_test_arg(L"/z"),         cec_test_arg(L"67108864"),
                                    cec_test_arg(L"/k"),      cec_test_arg(L"1"),          cec_test_arg(L"/c"),
                                    cec_test_arg(L"31"),      cec_test_arg(L"/threads"),   cec_test_arg(L"1"),
                                    cec_test_arg(L"/memory"), cec_test_arg(L"4294967296"), cec_test_arg(L"/cq"),
                                    cec_test_arg(L"64"),      cec_test_arg(L"/h") };
    cec_test_expect(parse(arena, 1) && parse(arena), "client registered arena accepts a worker below the DWORD limit");
    arena[9] = cec_test_arg(L"32");
    cec_test_expect(!parse(arena, 1) && !parse(arena) && std::wcsstr(error.data(), L"per worker") != nullptr,
                    "client registered arena rejects a 4 GiB worker before help even with enough total memory");
    arena[11] = cec_test_arg(L"2");
    cec_test_expect(parse(arena, 1) && parse(arena),
                    "client registered arena accepts the same storage split across smaller worker shards");
}

static void cec_test_text_resources() noexcept {
    cec_options                             options{};
    std::array<wchar_t, CEC_ERROR_CAPACITY> error{};
    const auto                              parse = [&](auto& args, int omitted_tail = 0) noexcept {
        error.fill(L'\0');
        return cec_parse_options(static_cast<int>(args.size()) - omitted_tail, args.data(), &options, error.data(),
                                                              error.size());
    };
    std::array<wchar_t*, 13> default_text{
        cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"),       cec_test_arg(L"tcp"),
        cec_test_arg(L"/c"),     cec_test_arg(L"22795"),     cec_test_arg(L"/threads"), cec_test_arg(L"64"),
        cec_test_arg(L"/cq"),    cec_test_arg(L"1024"),      cec_test_arg(L"/memory"),  cec_test_arg(L"1048576"),
        cec_test_arg(L"/h")
    };
    cec_test_expect(parse(default_text, 1) && parse(default_text),
                    "client default text fits its UTF-8 storage budget before help");
    default_text[5] = cec_test_arg(L"22796");
    cec_test_expect(!parse(default_text, 1) && !parse(default_text) && std::wcsstr(error.data(), L"/memory") != nullptr,
                    "client default text participates in parser memory validation before help");

    std::array<wchar_t*, 15> literal{ cec_test_arg(L"client"),  cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"),
                                      cec_test_arg(L"tcp"),     cec_test_arg(L"/d"),        cec_test_arg(L"\u00E9"),
                                      cec_test_arg(L"/c"),      cec_test_arg(L"262144"),    cec_test_arg(L"/threads"),
                                      cec_test_arg(L"64"),      cec_test_arg(L"/cq"),       cec_test_arg(L"16384"),
                                      cec_test_arg(L"/memory"), cec_test_arg(L"1048576"),   cec_test_arg(L"/h") };
    cec_test_expect(parse(literal, 1) && parse(literal), "client literal UTF-8 bytes fit an exact memory budget");
    literal[7] = cec_test_arg(L"262145");
    cec_test_expect(!parse(literal, 1) && !parse(literal) && std::wcsstr(error.data(), L"/memory") != nullptr,
                    "client literal memory counts UTF-8 bytes rather than UTF-16 code units before help");
    literal[5] = cec_test_arg(L"A");
    cec_test_expect(parse(literal, 1) && parse(literal),
                    "client shorter ASCII literal accepts the same otherwise valid resource settings");
    literal[5] = cec_test_arg(L"\xD83D\xDE00");
    literal[7] = cec_test_arg(L"131072");
    cec_test_expect(parse(literal, 1) && parse(literal),
                    "client paired surrogate literal accounts for four UTF-8 bytes");
    literal[7] = cec_test_arg(L"131073");
    cec_test_expect(!parse(literal, 1) && !parse(literal) && std::wcsstr(error.data(), L"/memory") != nullptr,
                    "client surrogate literal rejects the next session beyond its UTF-8 memory boundary");

    std::array<wchar_t, 21837> udp_text{};
    std::fill_n(udp_text.data(), 21835, L'\u4E2D');
    std::array<wchar_t*, 7> udp{ cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"),
                                 cec_test_arg(L"udp"),    cec_test_arg(L"/d"),        udp_text.data(),
                                 cec_test_arg(L"/h") };
    cec_test_expect(parse(udp, 1) && parse(udp), "client UDP text accepts a 65505-byte UTF-8 payload");
    udp_text[21835] = L'\u4E2D';
    cec_test_expect(!parse(udp, 1) && !parse(udp) && std::wcsstr(error.data(), L"65507") != nullptr,
                    "client UDP text rejects a 65508-byte UTF-8 payload before help");
}

static void cec_test_help_validation() noexcept {
    cec_options                             options{};
    std::array<wchar_t, CEC_ERROR_CAPACITY> error{};
    const auto                              parse = [&](auto& args) noexcept {
        error.fill(L'\0');
        return cec_parse_options(static_cast<int>(args.size()), args.data(), &options, error.data(), error.size());
    };
    std::array<wchar_t*, 2> help{ cec_test_arg(L"client"), cec_test_arg(L"/h") };
    cec_test_expect(parse(help) && options.help && options.protocol == cec_protocol::none,
                    "client help permits an omitted host and protocol");
    std::array<wchar_t*, 4> protocol_help{ cec_test_arg(L"client"), cec_test_arg(L"/h"), cec_test_arg(L"/p"),
                                           cec_test_arg(L"tcp") };
    cec_test_expect(parse(protocol_help) && options.help && options.protocol == cec_protocol::tcp,
                    "client help permits an omitted host with a valid protocol");
    std::array<wchar_t*, 3> host_help{ cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/h") };
    cec_test_expect(parse(host_help) && options.help, "client help permits an omitted protocol with a valid host");
    std::array<wchar_t*, 4> literal_help{ cec_test_arg(L"client"), cec_test_arg(L"/h"), cec_test_arg(L"/d"),
                                          cec_test_arg(L"\u00E9") };
    cec_test_expect(parse(literal_help) && options.help, "client help validates literal text without required fields");
    literal_help[3] = cec_test_arg(L"\xD800");
    cec_test_expect(!parse(literal_help) && std::wcsstr(error.data(), L"UTF-16") != nullptr,
                    "client help rejects unpaired surrogate text before the required-field shortcut");

    std::array<wchar_t*, 6> pattern_conflict{ cec_test_arg(L"client"), cec_test_arg(L"/h"), cec_test_arg(L"/d"),
                                              cec_test_arg(L"A"),      cec_test_arg(L"/z"), cec_test_arg(L"1") };
    cec_test_expect(!parse(pattern_conflict), "client help does not suppress conflicting payload modes");
    std::array<wchar_t*, 6> udp_pipeline{ cec_test_arg(L"client"), cec_test_arg(L"/h"), cec_test_arg(L"/p"),
                                          cec_test_arg(L"udp"),    cec_test_arg(L"/k"), cec_test_arg(L"1") };
    cec_test_expect(!parse(udp_pipeline) && std::wcsstr(error.data(), L"TCP") != nullptr,
                    "client help rejects an explicit UDP pipeline even at depth one");
    std::array<wchar_t*, 8> cq_conflict{ cec_test_arg(L"client"), cec_test_arg(L"/h"),       cec_test_arg(L"/c"),
                                         cec_test_arg(L"33"),     cec_test_arg(L"/threads"), cec_test_arg(L"1"),
                                         cec_test_arg(L"/cq"),    cec_test_arg(L"64") };
    cec_test_expect(!parse(cq_conflict) && std::wcsstr(error.data(), L"completion queue") != nullptr,
                    "client help rejects an undersized CQ without required fields");
    cq_conflict[3] = cec_test_arg(L"32");
    cec_test_expect(parse(cq_conflict) && options.help, "client help accepts exact two-operation CQ reservation");
    std::array<wchar_t*, 6> quota_conflict{ cec_test_arg(L"client"), cec_test_arg(L"/h"),
                                            cec_test_arg(L"/c"),     cec_test_arg(L"2"),
                                            cec_test_arg(L"/n"),     cec_test_arg(L"9223372036854775808") };
    cec_test_expect(!parse(quota_conflict) && std::wcsstr(error.data(), L"finite quota") != nullptr,
                    "client help rejects finite quota overflow before required fields");
    std::array<wchar_t*, 4> numeric{ cec_test_arg(L"client"), cec_test_arg(L"/h"), cec_test_arg(L"/t"),
                                     cec_test_arg(L"0") };
    cec_test_expect(!parse(numeric), "client help does not suppress an out-of-range numeric value");
    numeric[2] = cec_test_arg(L"/foo");
    cec_test_expect(!parse(numeric) && std::wcsstr(error.data(), L"unknown switch") != nullptr,
                    "client help does not suppress an unknown switch");
}

static void cec_test_patterns() noexcept {
    std::array<std::byte, 12> binary{};
    cec_fill_binary_pattern(binary.data(), binary.size());
    cec_test_expect(binary[0] == std::byte{ 0 } && binary[1] == std::byte{ 1 } && binary[11] == std::byte{ 11 },
                    "client binary pattern is deterministic");

    std::array<std::byte, 9> printable{};
    cec_fill_printable_pattern(printable.data(), printable.size());
    const std::array<std::byte, 9> expected{ std::byte{ '0' }, std::byte{ '0' }, std::byte{ '0' },
                                             std::byte{ '0' }, std::byte{ '0' }, std::byte{ '0' },
                                             std::byte{ '0' }, std::byte{ '0' }, std::byte{ ' ' } };
    cec_test_expect(printable == expected, "client printable pattern is deterministic");
}

static void cec_test_attempt_claim() noexcept {
    std::atomic<std::uint64_t> claimed{ 7 };
    cec_test_expect(cec_claim_attempts(&claimed, 10, 8) == 3 && claimed.load() == 10,
                    "client finite attempt claim never exceeds limit");
    cec_test_expect(cec_claim_attempts(&claimed, 10, 1) == 0 && claimed.load() == 10,
                    "client exhausted attempt claim returns zero");
    cec_test_expect(cec_unclaimed_echoes(10, 4, false) == 6,
                    "client finite workload reports echoes never claimed after terminal failure");
    cec_test_expect(cec_unclaimed_echoes(10, 4, true) == 0 && cec_unclaimed_echoes(0, 100, false) == 0,
                    "client controlled stop and unlimited workload do not invent loss");
    cec_test_expect(cec_percentile_target(std::numeric_limits<std::uint64_t>::max(), 999, 1000) != 0,
                    "client percentile rank calculation does not overflow");
}

static void cec_test_notification() noexcept {
    bool armed = true;
    cec_test_expect(cec_notification_mark_delivered(&armed) && !armed,
                    "client CQ delivery consumes one armed notification");
    cec_test_expect(!cec_notification_mark_delivered(&armed), "client CQ delivery cannot consume notification twice");
    cec_test_expect(cec_notification_mark_rearmed(&armed) && armed, "client CQ drain rearms notification once");
    cec_test_expect(!cec_notification_mark_rearmed(&armed), "client CQ cannot be rearmed twice");
}

static void cec_test_result_classification() noexcept {
    cec_test_expect(cec_classify_result(10, 0, 0, 2, false, false) == cec_exit_code::success,
                    "client recovered connection errors do not fail a successful workload");
    cec_test_expect(cec_classify_result(0, 0, 0, 2, false, false) == cec_exit_code::network,
                    "client reports network failure when no echo succeeds");
    cec_test_expect(cec_classify_result(9, 0, 1, 1, false, false) == cec_exit_code::echo_failure,
                    "client reports an incomplete echo workload");
    cec_test_expect(cec_classify_result(10, 0, 0, 0, true, false) == cec_exit_code::internal,
                    "client reports fatal engine failures");
    cec_test_expect(cec_classify_result(0, 0, 0, 0, false, true) == cec_exit_code::success,
                    "client controlled stop succeeds before the first echo");
    cec_test_expect(cec_classify_result(0, 1, 0, 0, false, true) == cec_exit_code::echo_failure,
                    "client controlled stop does not hide corruption");
}

int main() {
    cec_test_parser();
    cec_test_quota_and_cq_options();
    cec_test_capacity();
    cec_test_cq_batch_reservation();
    cec_test_resource_validation();
    cec_test_text_resources();
    cec_test_help_validation();
    cec_test_patterns();
    cec_test_attempt_claim();
    cec_test_notification();
    cec_test_result_classification();
    std::printf("client_contract_failures=%d\n", cec_test_failures);
    return cec_test_failures == 0 ? 0 : 1;
}
