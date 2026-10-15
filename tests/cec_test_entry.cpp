//! Entry point of the single test executable. The project builds the product executable from
//! src/*.cpp and this test executable from src/*.cpp plus tests/*.cpp, so every suite keeps its
//! own accounting and exit code while sharing one project and one binary.

#include <Windows.h>

#include <cstdio>
#include <cwchar>

int cec_core_tests_main();
int cec_runtime_tests_main();
int cec_engine_tests_main();
int cec_tests_main();
int cec_fault_driver_main(int argc, wchar_t** argv);

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: cpp-echo-client-tests <suite> [mode]\n"
                     "  suite: core | runtime | engine | contracts | fault <mode>\n");
        return 2;
    }
    const wchar_t* suite = argv[1];
    if (std::wcscmp(suite, L"core") == 0) {
        return cec_core_tests_main();
    }
    if (std::wcscmp(suite, L"runtime") == 0) {
        return cec_runtime_tests_main();
    }
    if (std::wcscmp(suite, L"engine") == 0) {
        return cec_engine_tests_main();
    }
    if (std::wcscmp(suite, L"contracts") == 0) {
        return cec_tests_main();
    }
    if (std::wcscmp(suite, L"fault") == 0) {
        // The fault driver keeps its historical argument layout: argv[1] is the mode.
        return cec_fault_driver_main(argc - 1, argv + 1);
    }
    std::fprintf(stderr, "unknown suite\n");
    return 2;
}
