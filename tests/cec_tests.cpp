#include "cec_types.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <limits>

static int cec_test_failures = 0;

static void cec_test_expect(bool condition, const char* name) noexcept
{
    if (condition)
    {
        std::printf("PASS %s\n", name);
        return;
    }
    std::fprintf(stderr, "FAIL %s\n", name);
    ++cec_test_failures;
}

static wchar_t* cec_test_arg(const wchar_t* value) noexcept
{
    return const_cast<wchar_t*>(value);
}

static void cec_test_parser() noexcept
{
    std::array<wchar_t*, 17> args{cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"),
                                  cec_test_arg(L"tcp"),    cec_test_arg(L"/r"),        cec_test_arg(L"4578"),
                                  cec_test_arg(L"/n"),     cec_test_arg(L"1000"),      cec_test_arg(L"/k"),
                                  cec_test_arg(L"8"),      cec_test_arg(L"/z"),        cec_test_arg(L"64"),
                                  cec_test_arg(L"/c"),     cec_test_arg(L"32"),        cec_test_arg(L"/threads"),
                                  cec_test_arg(L"4"),      cec_test_arg(L"/q")};
    cec_options options{};
    std::array<wchar_t, CEC_ERROR_CAPACITY> error{};
    const bool parsed =
        cec_parse_options(static_cast<int>(args.size()), args.data(), &options, error.data(), error.size());
    cec_test_expect(parsed && options.protocol == cec_protocol::tcp && options.remote_port == 4578 &&
                        options.echo_count == 1000 && options.pipeline_depth == 8 && options.pattern_bytes == 64 &&
                        options.session_count == 32 && options.worker_count == 4 && options.quiet,
                    "client parses concurrent TCP workload");

    std::array<wchar_t*, 7> invalid{cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"),
                                    cec_test_arg(L"udp"),    cec_test_arg(L"/k"),        cec_test_arg(L"2"),
                                    cec_test_arg(L"/q")};
    cec_options rejected{};
    error.fill(L'\0');
    cec_test_expect(
        !cec_parse_options(static_cast<int>(invalid.size()), invalid.data(), &rejected, error.data(), error.size()) &&
            std::wcsstr(error.data(), L"TCP") != nullptr,
        "client rejects TCP pipeline depth for UDP");

    wchar_t literal[] = L"owned-literal";
    std::array<wchar_t*, 6> owned_args{cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/P"),
                                       cec_test_arg(L"TCP"),    cec_test_arg(L"/d"),        literal};
    cec_options owned{};
    error.fill(L'\0');
    const bool owned_parsed =
        cec_parse_options(static_cast<int>(owned_args.size()), owned_args.data(), &owned, error.data(), error.size());
    literal[0] = L'X';
    cec_test_expect(owned_parsed && std::wcscmp(owned.literal_pattern, L"owned-literal") == 0,
                    "client owns literal text and folds ASCII switch case");

    std::array<wchar_t*, 6> unknown_args{cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"),
                                         cec_test_arg(L"tcp"),    cec_test_arg(L"/foo"),      cec_test_arg(L"1")};
    error.fill(L'\0');
    cec_test_expect(!cec_parse_options(static_cast<int>(unknown_args.size()), unknown_args.data(), &rejected,
                                       error.data(), error.size()) &&
                        std::wcsstr(error.data(), L"unknown switch") != nullptr,
                    "client reports unknown switch before parsing its value");

    std::array<wchar_t*, 5> empty_args{cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"),
                                       cec_test_arg(L"tcp"), cec_test_arg(L"/r=")};
    error.fill(L'\0');
    cec_test_expect(!cec_parse_options(static_cast<int>(empty_args.size()), empty_args.data(), &rejected, error.data(),
                                       error.size()),
                    "client rejects empty inline values");

    std::array<wchar_t*, 7> followed_empty_args{
        cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"), cec_test_arg(L"tcp"),
        cec_test_arg(L"/r="),    cec_test_arg(L"7001"),      cec_test_arg(L"/q")};
    error.fill(L'\0');
    cec_test_expect(!cec_parse_options(static_cast<int>(followed_empty_args.size()), followed_empty_args.data(),
                                       &rejected, error.data(), error.size()),
                    "client empty inline value never consumes the following token");

    std::array<wchar_t*, 8> tcp_reconnect_args{
        cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"),  cec_test_arg(L"tcp"),
        cec_test_arg(L"/l"),     cec_test_arg(L"7001"),      cec_test_arg(L"/rc"), cec_test_arg(L"1")};
    error.fill(L'\0');
    cec_test_expect(!cec_parse_options(static_cast<int>(tcp_reconnect_args.size()), tcp_reconnect_args.data(),
                                       &rejected, error.data(), error.size()),
                    "client rejects TCP reconnect with fixed local port");

    std::array<wchar_t*, 8> udp_reconnect_args{
        cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"),  cec_test_arg(L"udp"),
        cec_test_arg(L"/l"),     cec_test_arg(L"7001"),      cec_test_arg(L"/rc"), cec_test_arg(L"1")};
    error.fill(L'\0');
    cec_test_expect(cec_parse_options(static_cast<int>(udp_reconnect_args.size()), udp_reconnect_args.data(), &rejected,
                                      error.data(), error.size()),
                    "client allows UDP reconnect with fixed local port");

    std::array<wchar_t*, 8> tcp_fixed_port_many_sessions{
        cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"), cec_test_arg(L"tcp"),
        cec_test_arg(L"/l"),     cec_test_arg(L"45000"),     cec_test_arg(L"/c"), cec_test_arg(L"2")};
    error.fill(L'\0');
    cec_test_expect(!cec_parse_options(static_cast<int>(tcp_fixed_port_many_sessions.size()),
                                       tcp_fixed_port_many_sessions.data(), &rejected, error.data(), error.size()) &&
                        std::wcscmp(error.data(), L"a fixed /l port requires /c 1") == 0,
                    "client parser rejects fixed TCP port with multiple sessions");

    std::array<wchar_t*, 8> udp_fixed_port_many_sessions{
        cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"), cec_test_arg(L"udp"),
        cec_test_arg(L"/l"),     cec_test_arg(L"45000"),     cec_test_arg(L"/c"), cec_test_arg(L"2")};
    error.fill(L'\0');
    cec_test_expect(!cec_parse_options(static_cast<int>(udp_fixed_port_many_sessions.size()),
                                       udp_fixed_port_many_sessions.data(), &rejected, error.data(), error.size()) &&
                        std::wcscmp(error.data(), L"a fixed /l port requires /c 1") == 0,
                    "client parser rejects fixed UDP port with multiple sessions");

    std::array<wchar_t*, 8> fixed_port_one_session{
        cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"), cec_test_arg(L"tcp"),
        cec_test_arg(L"/l"),     cec_test_arg(L"45000"),     cec_test_arg(L"/c"), cec_test_arg(L"1")};
    error.fill(L'\0');
    cec_test_expect(cec_parse_options(static_cast<int>(fixed_port_one_session.size()), fixed_port_one_session.data(),
                                      &rejected, error.data(), error.size()),
                    "client parser accepts fixed port with one session");

    std::array<wchar_t*, 9> fixed_port_conflict_with_help{
        cec_test_arg(L"client"), cec_test_arg(L"127.0.0.1"), cec_test_arg(L"/p"),
        cec_test_arg(L"tcp"),    cec_test_arg(L"/l"),        cec_test_arg(L"45000"),
        cec_test_arg(L"/c"),     cec_test_arg(L"2"),         cec_test_arg(L"/h")};
    error.fill(L'\0');
    cec_test_expect(!cec_parse_options(static_cast<int>(fixed_port_conflict_with_help.size()),
                                       fixed_port_conflict_with_help.data(), &rejected, error.data(), error.size()) &&
                        std::wcscmp(error.data(), L"a fixed /l port requires /c 1") == 0,
                    "client help does not suppress a fixed-port session conflict");
}

static void cec_test_capacity() noexcept
{
    std::size_t value = 0;
    cec_test_expect(cec_checked_product(8, 65536, &value) && value == 524288,
                    "client checked batch size accepts valid values");
    cec_test_expect(!cec_checked_product(std::numeric_limits<std::size_t>::max(), 2, &value),
                    "client checked batch size rejects overflow");
    cec_test_expect(cec_checked_storage_bytes(32, 4096, 1048576, &value) && value == 262144,
                    "client storage includes send and receive buffers");
    cec_test_expect(!cec_checked_storage_bytes(32, 4096, 131072, &value), "client storage rejects memory limit excess");
}

static void cec_test_patterns() noexcept
{
    std::array<std::byte, 12> binary{};
    cec_fill_binary_pattern(binary.data(), binary.size());
    cec_test_expect(binary[0] == std::byte{0} && binary[1] == std::byte{1} && binary[11] == std::byte{11},
                    "client binary pattern is deterministic");

    std::array<std::byte, 9> printable{};
    cec_fill_printable_pattern(printable.data(), printable.size());
    const std::array<std::byte, 9> expected{std::byte{'0'}, std::byte{'0'}, std::byte{'0'},
                                            std::byte{'0'}, std::byte{'0'}, std::byte{'0'},
                                            std::byte{'0'}, std::byte{'0'}, std::byte{' '}};
    cec_test_expect(printable == expected, "client printable pattern is deterministic");
}

static void cec_test_attempt_claim() noexcept
{
    std::atomic<std::uint64_t> claimed{7};
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

static void cec_test_notification() noexcept
{
    bool armed = true;
    cec_test_expect(cec_notification_mark_delivered(&armed) && !armed,
                    "client CQ delivery consumes one armed notification");
    cec_test_expect(!cec_notification_mark_delivered(&armed), "client CQ delivery cannot consume notification twice");
    cec_test_expect(cec_notification_mark_rearmed(&armed) && armed, "client CQ drain rearms notification once");
    cec_test_expect(!cec_notification_mark_rearmed(&armed), "client CQ cannot be rearmed twice");
}

static void cec_test_result_classification() noexcept
{
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

int main()
{
    cec_test_parser();
    cec_test_capacity();
    cec_test_patterns();
    cec_test_attempt_claim();
    cec_test_notification();
    cec_test_result_classification();
    std::printf("client_contract_failures=%d\n", cec_test_failures);
    return cec_test_failures == 0 ? 0 : 1;
}
