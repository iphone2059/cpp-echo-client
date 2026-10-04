cmake_minimum_required(VERSION 3.28)
project(cpp_echo_client LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "cpp-echo-client requires Windows and MSVC")
endif()
if(NOT CMAKE_GENERATOR MATCHES "Ninja")
    message(FATAL_ERROR "cpp-echo-client requires the Ninja generator")
endif()

include(cmake/cec_constraints.cmake)

add_executable(cpp-echo-client-tests tests/cec_tests.cpp src/cec_contract.cpp)
target_include_directories(cpp-echo-client-tests PRIVATE include)
cec_apply_constraints(cpp-echo-client-tests)

add_executable(cpp-echo-client-engine-tests tests/cec_engine_tests.cpp src/cec_engine_internal.cpp)
target_include_directories(cpp-echo-client-engine-tests PRIVATE include)
target_link_libraries(cpp-echo-client-engine-tests PRIVATE ws2_32)
cec_apply_constraints(cpp-echo-client-engine-tests)

add_executable(cpp-echo-client-fault-driver tests/cec_fault_driver.cpp src/cec_engine_internal.cpp)
target_include_directories(cpp-echo-client-fault-driver PRIVATE include)
target_link_libraries(cpp-echo-client-fault-driver PRIVATE ws2_32)
cec_apply_constraints(cpp-echo-client-fault-driver)

add_executable(cpp-echo-client src/main.cpp src/cec_contract.cpp src/cec_engine.cpp src/cec_engine_internal.cpp)
target_include_directories(cpp-echo-client PRIVATE include)
target_link_libraries(cpp-echo-client PRIVATE ws2_32)
cec_apply_constraints(cpp-echo-client)

enable_testing()
add_test(NAME cpp-echo-client-contracts COMMAND cpp-echo-client-tests)
add_test(NAME cpp-echo-client-engine COMMAND cpp-echo-client-engine-tests)
add_test(NAME cpp-echo-client-fault-boundaries
    COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/cec_fault_process_tests.ps1
            -DriverPath $<TARGET_FILE:cpp-echo-client-fault-driver>)
add_test(NAME cpp-echo-client-source-policy
    COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/cec_source_policy.ps1
            -ProjectRoot ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME cpp-echo-client-process-loopback
    COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/cec_process_tests.ps1
            -ClientPath $<TARGET_FILE:cpp-echo-client>)
set_tests_properties(cpp-echo-client-process-loopback PROPERTIES TIMEOUT 40)
add_test(NAME cpp-echo-client-process-quota
    COMMAND pwsh -NoProfile -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/cec_quota_process_tests.ps1
            -ClientPath $<TARGET_FILE:cpp-echo-client>)
set_tests_properties(cpp-echo-client-process-quota PROPERTIES TIMEOUT 40)
