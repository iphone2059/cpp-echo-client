include_guard(GLOBAL)

include(CheckIPOSupported)
check_ipo_supported(RESULT CEC_IPO_SUPPORTED OUTPUT CEC_IPO_ERROR LANGUAGES CXX)

string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

function(cec_apply_constraints target)
    set_target_properties("${target}" PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_definitions("${target}" PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _HAS_EXCEPTIONS=0 _WIN32_WINNT=0x0A00)
    target_compile_options("${target}" PRIVATE
        /std:c++latest /utf-8 /W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor /EHs-c- /GR-
        "/FI${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../include/cec_compiler_contract.h")
    if(CEC_IPO_SUPPORTED)
        set_property(TARGET "${target}" PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
endfunction()
