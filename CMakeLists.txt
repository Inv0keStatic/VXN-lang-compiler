cmake_minimum_required(VERSION 3.16)
project(vxn VERSION 1.0.0 LANGUAGES C)
include(GNUInstallDirs)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions(_POSIX_C_SOURCE=200809L)
endif()
set(VXN_EXTENDED_IMPLEMENTATION "SOFTFLOAT" CACHE STRING "Strict extended floating-point implementation")
if(NOT VXN_EXTENDED_IMPLEMENTATION STREQUAL "SOFTFLOAT")
    message(FATAL_ERROR "The supported strict extended implementation is SOFTFLOAT")
endif()
add_subdirectory(third_party)
add_library(vxn_extended_object OBJECT compiler/runtime/extended.c)
target_include_directories(vxn_extended_object PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/third_party"
    third_party/SoftFloat-3e/source/include)
target_compile_definitions(vxn_extended_object PRIVATE SOFTFLOAT_FAST_INT64)
add_library(vxn_extended STATIC $<TARGET_OBJECTS:vxn_extended_object> $<TARGET_OBJECTS:vxn_softfloat>)
if(WIN32)
    # Use the portable C library API rather than Microsoft's optional *_s API.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

add_executable(vxn_embed compiler/tools/embed.c)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vxn_runtime_text.h"
    COMMAND vxn_embed "${CMAKE_CURRENT_SOURCE_DIR}/compiler/runtime/scalar.h" "${CMAKE_CURRENT_BINARY_DIR}/vxn_runtime_text.h"
    DEPENDS vxn_embed compiler/runtime/scalar.h VERBATIM)
foreach(part io_api io heap_api heap parse utf8 text line extended)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vxn_${part}_text.h"
        COMMAND vxn_embed "${CMAKE_CURRENT_SOURCE_DIR}/compiler/runtime/${part}.h" "${CMAKE_CURRENT_BINARY_DIR}/vxn_${part}_text.h" "vx_${part}_text"
        DEPENDS vxn_embed "compiler/runtime/${part}.h" VERBATIM)
endforeach()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vxn_io_declarations.h"
    COMMAND vxn_embed "${CMAKE_CURRENT_SOURCE_DIR}/stdlib/io.vxn" "${CMAKE_CURRENT_BINARY_DIR}/vxn_io_declarations.h" vx_io_declarations
    DEPENDS vxn_embed stdlib/io.vxn VERBATIM)
set(VXN_LIBRARY_HEADERS "")
foreach(part graphics window time memory parse text lazy)
    set(header "${CMAKE_CURRENT_BINARY_DIR}/vxn_${part}_declarations.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND vxn_embed "${CMAKE_CURRENT_SOURCE_DIR}/stdlib/${part}.vxn" "${header}" "vx_${part}_declarations"
        DEPENDS vxn_embed "stdlib/${part}.vxn" VERBATIM)
    list(APPEND VXN_LIBRARY_HEADERS "${header}")
endforeach()
foreach(part library_api graphics_core platform_win32 platform_linux platform_stub library)
    set(header "${CMAKE_CURRENT_BINARY_DIR}/vxn_${part}_text.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND vxn_embed "${CMAKE_CURRENT_SOURCE_DIR}/stdlib/runtime/${part}.h" "${header}" "vx_${part}_text"
        DEPENDS vxn_embed "stdlib/runtime/${part}.h" VERBATIM)
    list(APPEND VXN_LIBRARY_HEADERS "${header}")
endforeach()
add_library(vxn_compiler STATIC
    $<TARGET_OBJECTS:vxn_extended_object> $<TARGET_OBJECTS:vxn_softfloat>
    compiler/src/tail.c
    compiler/src/ranges.c
    compiler/src/patterns.c
    compiler/src/ownership.c
    compiler/src/exclusive.c
    ${VXN_LIBRARY_HEADERS}
    "${CMAKE_CURRENT_BINARY_DIR}/vxn_runtime_text.h"
    "${CMAKE_CURRENT_BINARY_DIR}/vxn_extended_text.h"
    "${CMAKE_CURRENT_BINARY_DIR}/vxn_io_api_text.h"
    "${CMAKE_CURRENT_BINARY_DIR}/vxn_io_text.h"
    "${CMAKE_CURRENT_BINARY_DIR}/vxn_heap_api_text.h"
    "${CMAKE_CURRENT_BINARY_DIR}/vxn_heap_text.h"
    "${CMAKE_CURRENT_BINARY_DIR}/vxn_parse_text.h"
    "${CMAKE_CURRENT_BINARY_DIR}/vxn_utf8_text.h"
    "${CMAKE_CURRENT_BINARY_DIR}/vxn_text_text.h"
    "${CMAKE_CURRENT_BINARY_DIR}/vxn_line_text.h"
    "${CMAKE_CURRENT_BINARY_DIR}/vxn_io_declarations.h"
    compiler/src/types.c
    compiler/src/lazy.c
    compiler/src/callables.c
    compiler/src/library.c
    compiler/src/fold.c
    compiler/src/frontend.c
    compiler/src/modules.c
    compiler/src/generics.c
    compiler/src/check.c
    compiler/src/borrow.c
    compiler/src/stage.c
    compiler/src/eval.c
    compiler/src/codegen.c
    compiler/src/llvm.c
    compiler/src/process.c)
target_include_directories(vxn_compiler PUBLIC compiler/include PRIVATE compiler/src "${CMAKE_CURRENT_BINARY_DIR}")
add_executable(vxnc compiler/src/main.c)
target_link_libraries(vxnc PRIVATE vxn_compiler)
if(MINGW)
    target_link_options(vxnc PRIVATE -static)
endif()
add_library(vxn_runtime STATIC compiler/runtime/native.c $<TARGET_OBJECTS:vxn_extended_object> $<TARGET_OBJECTS:vxn_softfloat>)
if(WIN32)
    target_link_libraries(vxn_compiler PUBLIC user32 gdi32)
    target_link_libraries(vxn_runtime PUBLIC user32 gdi32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(vxn_compiler PUBLIC m)
    target_link_libraries(vxn_runtime PUBLIC m)
endif()
add_dependencies(vxnc vxn_runtime vxn_extended)

# LLVM reads the emitted .ll directly. The C driver only links objects with
# the prebuilt runtime, using the same target/CRT as the CMake build.
find_program(VXN_LLVM_COMPILER NAMES clang HINTS "C:/Program Files/LLVM/bin")
if(NOT VXN_LLVM_COMPILER)
    message(STATUS "Clang not found: emit-llvm works; native builds require --llvm")
    set(VXN_LLVM_COMPILER "clang")
endif()
if(NOT MSVC)
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
        OUTPUT_VARIABLE VXN_HOST_TARGET OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE VXN_TARGET_STATUS)
    if(NOT VXN_TARGET_STATUS EQUAL 0)
        message(FATAL_ERROR "Cannot determine the native runtime target")
    endif()
else()
    set(VXN_HOST_TARGET "x86_64-pc-windows-msvc")
endif()
set(VXN_LLVM_TARGET "${VXN_HOST_TARGET}" CACHE STRING "LLVM target matching the native C runtime")
set(VXN_LINK_OPTIONS "")
set(VXN_PLATFORM_LINKS "")
if(WIN32)
    set(VXN_PLATFORM_LINKS "\"-luser32\",\"-lgdi32\",")
    if(MINGW)
        string(APPEND VXN_PLATFORM_LINKS "\"-static\",")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(VXN_PLATFORM_LINKS "\"-lm\",")
endif()
string(TOUPPER "${CMAKE_BUILD_TYPE}" VXN_BUILD_TYPE)
separate_arguments(VXN_LINK_FLAGS NATIVE_COMMAND
    "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${VXN_BUILD_TYPE}} ${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_EXE_LINKER_FLAGS_${VXN_BUILD_TYPE}}")
foreach(flag IN LISTS VXN_LINK_FLAGS)
    string(REPLACE "\\" "\\\\" flag "${flag}")
    string(REPLACE "\"" "\\\"" flag "${flag}")
    string(APPEND VXN_LINK_OPTIONS "\"${flag}\",")
endforeach()
if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    # CMake selects the MSVC CRT through target properties, not CMAKE_C_FLAGS.
    # The final native link must use the same CRT as the prebuilt runtime.
    set(VXN_CRT "$<TARGET_GENEX_EVAL:vxn_runtime,$<TARGET_PROPERTY:vxn_runtime,MSVC_RUNTIME_LIBRARY>>")
    if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set(VXN_CRT "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
    # An object-only Clang link otherwise injects libcmt even when the archive
    # correctly requests msvcrtd/msvcrt/libcmtd through its COFF directives.
    string(APPEND VXN_LINK_OPTIONS
        "$<$<AND:$<BOOL:${VXN_CRT}>,$<NOT:$<STREQUAL:${VXN_CRT},MultiThreaded>>>:\"-Xlinker\",\"/nodefaultlib:libcmt\",>")
    # Clang's object-only driver chooses the ASan thunk from _DLL, separately
    # from CRT directives in the archive. Match CMake's DLL runtime choice.
    string(APPEND VXN_LINK_OPTIONS
        "$<$<OR:$<STREQUAL:${VXN_CRT},MultiThreadedDLL>,$<STREQUAL:${VXN_CRT},MultiThreadedDebugDLL>>:\"-D_DLL\",>")
endif()
get_filename_component(VXN_CC_NAME "${CMAKE_C_COMPILER}" NAME)
get_filename_component(VXN_LLVM_NAME "${VXN_LLVM_COMPILER}" NAME)
file(RELATIVE_PATH VXN_LIBRARY_FROM_BIN "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}/vxn")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/vxn_native_config.h" CONTENT
"#define VXN_VERSION \"${PROJECT_VERSION}\"
#define VXN_DEFAULT_LLVM \"${VXN_LLVM_COMPILER}\"
#define VXN_DEFAULT_CC \"${CMAKE_C_COMPILER}\"
#define VXN_LLVM_NAME \"${VXN_LLVM_NAME}\"
#define VXN_CC_NAME \"${VXN_CC_NAME}\"
#define VXN_LLVM_TARGET \"${VXN_LLVM_TARGET}\"
#define VXN_BUILD_COMPILER_PATH \"$<TARGET_FILE:vxnc>\"
#define VXN_RUNTIME_PATH \"$<TARGET_FILE:vxn_runtime>\"
#define VXN_EXTENDED_PATH \"$<TARGET_FILE:vxn_extended>\"
#define VXN_INSTALLED_RUNTIME \"${VXN_LIBRARY_FROM_BIN}/$<TARGET_FILE_NAME:vxn_runtime>\"
#define VXN_INSTALLED_EXTENDED \"${VXN_LIBRARY_FROM_BIN}/$<TARGET_FILE_NAME:vxn_extended>\"
static const char *const vxn_link_options[] = {${VXN_LINK_OPTIONS}NULL};
static const char *const vxn_platform_links[] = {${VXN_PLATFORM_LINKS}NULL};
")
target_include_directories(vxnc PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
add_executable(vxn_bitmap examples/write_bitmap.c)

if(MSVC)
    target_compile_options(vxn_compiler PRIVATE /W4)
    target_compile_options(vxnc PRIVATE /W4)
    target_compile_options(vxn_bitmap PRIVATE /W4)
    target_compile_options(vxn_runtime PRIVATE /W4)
    target_compile_options(vxn_extended_object PRIVATE /W4)
else()
    target_compile_options(vxn_compiler PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(vxnc PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(vxn_bitmap PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(vxn_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(vxn_extended_object PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

install(TARGETS vxnc RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(TARGETS vxn_runtime vxn_extended ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/vxn")
install(DIRECTORY stdlib examples DESTINATION "${CMAKE_INSTALL_DATADIR}/vxn")
install(FILES README.md LANGUAGE.txt THIRD_PARTY_NOTICES.txt
    DESTINATION "${CMAKE_INSTALL_DATADIR}/vxn")
install(FILES third_party/SoftFloat-3e/COPYING.txt
    DESTINATION "${CMAKE_INSTALL_DATADIR}/vxn/third_party/SoftFloat-3e")
install(DIRECTORY third_party/toolchain-licenses DESTINATION "${CMAKE_INSTALL_DATADIR}/vxn/third_party")
set(CPACK_PACKAGE_NAME "vxn")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_GENERATOR "ZIP")
set(CPACK_SOURCE_GENERATOR "TGZ;ZIP")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "vxn-${PROJECT_VERSION}-source")
set(CPACK_SOURCE_IGNORE_FILES "/build/;/dist/;/.git/;/.github/;~$;[.]pyc$;/__pycache__/;[.]DS_Store$")
include(CPack)

include(CTest)
if(BUILD_TESTING)
    add_executable(vxn_extended_tests compiler/tests/extended_runtime.c)
    target_link_libraries(vxn_extended_tests PRIVATE vxn_extended)
    add_test(NAME extended_runtime COMMAND vxn_extended_tests)
    add_test(NAME collections_interpreter COMMAND vxnc run
        "${CMAKE_CURRENT_SOURCE_DIR}/compiler/tests/collections.vxn" --manifest
        "${CMAKE_CURRENT_SOURCE_DIR}/compiler/tests/collections.vxnp")
    set_tests_properties(collections_interpreter PROPERTIES PASS_REGULAR_EXPRESSION "true[\r\n]+42")
    add_test(NAME packages_interpreter COMMAND vxnc run
        "${CMAKE_CURRENT_SOURCE_DIR}/compiler/tests/package_units/main.vxn" --manifest
        "${CMAKE_CURRENT_SOURCE_DIR}/compiler/tests/package_units/root.vxnp")
    add_test(NAME orbit_navigation COMMAND vxnc run
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/functional_orbits/navigation_test.vxn" --manifest
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/functional_orbits/navigation_test.vxnp")
    if(WIN32)
        add_test(NAME orbit_cache_build COMMAND vxnc build
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/functional_orbits/cache_test.vxn" --manifest
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/functional_orbits/cache_test.vxnp"
            -o "${CMAKE_CURRENT_BINARY_DIR}/orbit-cache-test.exe")
        set_tests_properties(orbit_cache_build PROPERTIES FIXTURES_SETUP orbit_cache_binary)
        add_test(NAME orbit_cache COMMAND "${CMAKE_CURRENT_BINARY_DIR}/orbit-cache-test.exe")
        set_tests_properties(orbit_cache PROPERTIES FIXTURES_REQUIRED orbit_cache_binary TIMEOUT 60)
    endif()
    foreach(collection maps maps_owned maps_borrowed hashmaps hashmaps_owned hashmaps_collisions sorted_collections sorted_oracle sorted_owned)
        add_test(NAME ${collection}_interpreter COMMAND vxnc run
            "${CMAKE_CURRENT_SOURCE_DIR}/compiler/tests/${collection}.vxn" --manifest
            "${CMAKE_CURRENT_SOURCE_DIR}/compiler/tests/${collection}.vxnp")
    endforeach()
    set_tests_properties(maps_interpreter PROPERTIES PASS_REGULAR_EXPRESSION "true")
    set_tests_properties(maps_owned_interpreter PROPERTIES PASS_REGULAR_EXPRESSION "42")
    add_library(vxn_failing_runtime STATIC compiler/tests/failing_runtime.c $<TARGET_OBJECTS:vxn_extended_object> $<TARGET_OBJECTS:vxn_softfloat>)
    add_library(vxn_limited_runtime STATIC compiler/tests/failing_runtime.c $<TARGET_OBJECTS:vxn_extended_object> $<TARGET_OBJECTS:vxn_softfloat>)
    target_compile_definitions(vxn_limited_runtime PRIVATE VX_TEST_ALLOC_LIMIT=2)
    add_executable(vxn_heap_tests compiler/tests/heap_faults.c)
    if(NOT MSVC)
        target_compile_options(vxn_heap_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME heap_runtime COMMAND vxn_heap_tests)
    add_executable(vxn_library_tests compiler/tests/library_tests.c)
    if(WIN32)
        target_link_libraries(vxn_library_tests PRIVATE user32 gdi32)
    endif()
    if(NOT MSVC)
        target_compile_options(vxn_library_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME standard_library COMMAND vxn_library_tests)
    add_executable(vxn_io_tests compiler/tests/io_faults.c)
    target_link_libraries(vxn_io_tests PRIVATE vxn_extended)
    if(NOT MSVC)
        target_compile_options(vxn_io_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME io_runtime COMMAND vxn_io_tests)
    add_executable(vxn_tests compiler/tests/test.c)
    add_dependencies(vxn_tests vxn_failing_runtime vxn_limited_runtime)
    target_compile_definitions(vxn_tests PRIVATE VXN_EXTENDED_PATH="$<TARGET_FILE:vxn_extended>" VXN_FAILING_RUNTIME="$<TARGET_FILE:vxn_failing_runtime>" VXN_LIMITED_RUNTIME="$<TARGET_FILE:vxn_limited_runtime>")
    target_include_directories(vxn_tests PRIVATE compiler/src)
    target_link_libraries(vxn_tests PRIVATE vxn_compiler)
    if(NOT MSVC)
        target_compile_options(vxn_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME compiler COMMAND vxn_tests)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
        add_test(NAME native_differential COMMAND vxn_tests --native "${CMAKE_C_COMPILER}" --driver $<TARGET_FILE:vxnc> $<TARGET_FILE:vxn_tests>)
        add_test(NAME llvm_differential COMMAND vxn_tests --llvm "${VXN_LLVM_COMPILER}" --driver $<TARGET_FILE:vxnc> $<TARGET_FILE:vxn_tests>)
        add_test(NAME llvm_units COMMAND vxn_tests --unit-llvm $<TARGET_FILE:vxnc> "${VXN_LLVM_COMPILER}")
        add_test(NAME lazy_llvm COMMAND vxn_tests --lazy-llvm $<TARGET_FILE:vxnc> "${VXN_LLVM_COMPILER}")
        add_test(NAME lazy_native COMMAND vxn_tests --lazy-native $<TARGET_FILE:vxnc> "${CMAKE_C_COMPILER}")
        add_test(NAME lazy_units COMMAND vxn_tests --lazy-unit-llvm $<TARGET_FILE:vxnc> "${VXN_LLVM_COMPILER}")
        add_test(NAME equations_llvm COMMAND vxn_tests --equations-llvm $<TARGET_FILE:vxnc> "${VXN_LLVM_COMPILER}")
        add_test(NAME equations_native COMMAND vxn_tests --equations-native $<TARGET_FILE:vxnc> "${CMAKE_C_COMPILER}")
        add_test(NAME equations_units COMMAND vxn_tests --equations-unit-llvm $<TARGET_FILE:vxnc> "${VXN_LLVM_COMPILER}")
        foreach(feature partial kinds recursive)
            add_test(NAME ${feature}_llvm COMMAND vxn_tests --${feature}-llvm $<TARGET_FILE:vxnc> "${VXN_LLVM_COMPILER}")
            add_test(NAME ${feature}_native COMMAND vxn_tests --${feature}-native $<TARGET_FILE:vxnc> "${CMAKE_C_COMPILER}")
            add_test(NAME ${feature}_units COMMAND vxn_tests --${feature}-unit-llvm $<TARGET_FILE:vxnc> "${VXN_LLVM_COMPILER}")
        endforeach()
    endif()
endif()
