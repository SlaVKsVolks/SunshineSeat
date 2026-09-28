cmake_minimum_required(VERSION 3.13)
# https://github.com/google/oss-policies-info/blob/main/foundational-cxx-support-matrix.md#foundational-c-support

project(test_sunshine)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED CMAKE_CUDA_STANDARD)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
endif()

include_directories("${CMAKE_SOURCE_DIR}")

enable_testing()

# Add GoogleTest directory to the project
set(GTEST_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third-party/googletest")
set(INSTALL_GTEST OFF)
set(INSTALL_GMOCK OFF)
add_subdirectory("${GTEST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/googletest")
include_directories("${GTEST_SOURCE_DIR}/googletest/include" "${GTEST_SOURCE_DIR}")

# coverage
# https://gcovr.com/en/stable/guide/compiling.html#compiler-options
set(CMAKE_CXX_FLAGS "-fprofile-arcs -ftest-coverage -ggdb -O0")
set(CMAKE_C_FLAGS "-fprofile-arcs -ftest-coverage -ggdb -O0")

# Find the correct libgcov library path matching the gcc compiler version
# This ensures the test executable links against the same libgcov version used during compilation
if(UNIX AND NOT APPLE)
    # Get the gcc compiler version
    execute_process(
        COMMAND ${CMAKE_C_COMPILER} -dumpversion
        OUTPUT_VARIABLE GCC_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    # Extract major version
    string(REGEX MATCH "^[0-9]+" GCC_MAJOR_VERSION "${GCC_VERSION}")

    # Search for the gcc library directory matching this version
    file(GLOB GCC_LIB_DIRS "/usr/lib/gcc/*/*${GCC_MAJOR_VERSION}.*")

    if(GCC_LIB_DIRS)
        list(GET GCC_LIB_DIRS 0 GCC_LIB_DIR)
        message(STATUS "Found GCC library directory: ${GCC_LIB_DIR}")

        # Look for libgcov.a in the gcc library directory
        find_library(LIBGCOV_LIBRARY
            NAMES gcov
            PATHS ${GCC_LIB_DIR}
            NO_DEFAULT_PATH
        )

        if(LIBGCOV_LIBRARY)
            message(STATUS "Found libgcov: ${LIBGCOV_LIBRARY}")
            # Store this to link against later
            set(GCOV_LINK_LIBRARY ${LIBGCOV_LIBRARY})
        else()
            message(WARNING "Could not find libgcov in ${GCC_LIB_DIR}")
        endif()
    else()
        message(WARNING "Could not find GCC library directory for version ${GCC_VERSION}")
    endif()
endif()

# if windows
if (WIN32)
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # cmake-lint: disable=C0103
endif ()

# modify SUNSHINE_DEFINITIONS
if (WIN32)
    list(APPEND
            SUNSHINE_DEFINITIONS SUNSHINE_SHADERS_DIR="${CMAKE_SOURCE_DIR}/src_assets/windows/assets/shaders/directx")
elseif (NOT APPLE)
    list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_SHADERS_DIR="${CMAKE_SOURCE_DIR}/src_assets/linux/assets/shaders/opengl")
endif ()

set(TEST_DEFINITIONS)  # list will be appended as needed

# this indicates we're building tests in case sunshine needs to adjust some code or add private tests
list(APPEND TEST_DEFINITIONS SUNSHINE_TESTS)
list(APPEND TEST_DEFINITIONS SUNSHINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
list(APPEND TEST_DEFINITIONS SUNSHINE_TEST_BIN_DIR="${CMAKE_CURRENT_BINARY_DIR}")

# Override SUNSHINE_ASSETS_DIR to use a writable temp directory for tests
# Remove the existing definition from SUNSHINE_DEFINITIONS to avoid redefinition error
list(FILTER SUNSHINE_DEFINITIONS EXCLUDE REGEX "^SUNSHINE_ASSETS_DIR=")
list(APPEND TEST_DEFINITIONS SUNSHINE_ASSETS_DIR="${CMAKE_CURRENT_BINARY_DIR}/test_assets")

if(NOT WIN32)
    find_package(Udev 255)  # we need 255+ for udevadm verify
    message(STATUS "UDEV_FOUND: ${UDEV_FOUND}")
    if(UDEV_FOUND)
        list(APPEND TEST_DEFINITIONS UDEVADM_EXECUTABLE="${UDEVADM_EXECUTABLE}")
    endif()
endif()

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/tests/*.h
        ${CMAKE_SOURCE_DIR}/tests/*.cpp)
# Add macOS-specific test files only when building tests for macOS
if (APPLE)
    file(GLOB_RECURSE MACOS_TEST_SOURCES CONFIGURE_DEPENDS
            ${CMAKE_SOURCE_DIR}/tests/*.mm)
    list(APPEND TEST_SOURCES ${MACOS_TEST_SOURCES})
endif ()

set(SUNSHINE_SOURCES
        ${SUNSHINE_TARGET_FILES})

# remove main.cpp from the list of sources
list(REMOVE_ITEM SUNSHINE_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

add_executable(${PROJECT_NAME}
        ${TEST_SOURCES}
        ${SUNSHINE_SOURCES})

# Keep the dual-track launch contract executable independent from the full
# Sunshine test harness so the transport contract remains runnable when the
# broad integration test link is unavailable on a developer machine.
add_executable(test_dual_display_launch_contract
        internal/dual_display_launch_contract_test.cc
        ${CMAKE_SOURCE_DIR}/src/dual_display_launch_contract.cpp)
target_link_libraries(test_dual_display_launch_contract PRIVATE
        nlohmann_json::nlohmann_json)
add_test(NAME dual_display_launch_contract
        COMMAND test_dual_display_launch_contract)

# Keep the encrypted clipboard envelope budget contract independent from the
# full Sunshine test harness and any live control session.
add_executable(test_clipboard_image_envelope_contract
        internal/clipboard_image_envelope_contract_test.cc)
add_test(NAME clipboard_image_envelope_contract
        COMMAND test_clipboard_image_envelope_contract)

# Keep the recovery scheduler contract independent from the broad Sunshine
# test harness. It uses an injected clock and must never require a real stream.
add_executable(test_dual_idr_scheduler_contract
        internal/dual_idr_scheduler_contract_test.cc
        ${CMAKE_SOURCE_DIR}/src/dual_idr_scheduler.cpp)
add_test(NAME dual_idr_scheduler_contract
        COMMAND test_dual_idr_scheduler_contract)

# The WGC producer must publish the replacement frame before releasing the
# previous WinRT frame, because Close() can block while holding the producer
# lock and defeat the consumer timeout.
add_executable(test_wgc_frame_handoff_contract
        internal/wgc_frame_handoff_contract_test.cc)
add_test(NAME wgc_frame_handoff_contract
        COMMAND test_wgc_frame_handoff_contract)

# Keep the shared broadcaster free of cross-track recovery state. Ordinary
# rate pacing remains in stream.cpp; only video.cpp may schedule a dual IDR.
add_executable(test_dual_idr_broadcaster_contract
        internal/dual_idr_broadcaster_contract_test.cc)
target_compile_definitions(test_dual_idr_broadcaster_contract PRIVATE
        SUNSHINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME dual_idr_broadcaster_contract
        COMMAND test_dual_idr_broadcaster_contract)

# A boolean mail event is the recovery-IDR one-slot latch. Its timed pop must
# support a zero-wait consume without requiring an optional status type.
add_executable(test_thread_safe_event_contract
        internal/thread_safe_event_contract_test.cc)
add_test(NAME thread_safe_event_contract
        COMMAND test_thread_safe_event_contract)

# A dual AVCodec generation can block inside a driver call. Keep the retirement
# policy deterministic, clock-injected, and separate from a real encoder.
add_executable(test_dual_encoder_retirement_contract
        internal/dual_encoder_retirement_contract_test.cc
        ${CMAKE_SOURCE_DIR}/src/dual_encoder_retirement.cpp)
add_test(NAME dual_encoder_retirement_contract
        COMMAND test_dual_encoder_retirement_contract)

# The two video tracks must never initialize AMF/D3D encoder sessions concurrently.
add_executable(test_dual_encoder_initialization_gate_contract
        internal/dual_encoder_initialization_gate_contract_test.cc)
add_test(NAME dual_encoder_initialization_gate_contract
        COMMAND test_dual_encoder_initialization_gate_contract)

# Keep the network-test wire and policy contract independent from HTTP, curl,
# pairing, and active-stream state so malformed data never needs a live host.
add_executable(test_network_test_contract
        internal/network_test_contract_test.cc
        ${CMAKE_SOURCE_DIR}/src/network_test.cpp)
target_link_libraries(test_network_test_contract PRIVATE
        nlohmann_json::nlohmann_json)
add_test(NAME network_test_contract
        COMMAND test_network_test_contract)

# Keep input-timing math and correlation deterministic. The contract must not
# contact a client, inject Windows input, or start a stream.
add_executable(test_input_timing_contract
        internal/input_timing_contract_test.cc
        ${CMAKE_SOURCE_DIR}/src/input_timing.cpp)
add_test(NAME input_timing_contract
        COMMAND test_input_timing_contract)

# Keep the shared input-timing wire marker ABI explicit. This target contains
# no transport code, so it cannot enqueue input, contact a client, or start a
# stream.
add_executable(test_input_timing_wire_contract
        internal/input_timing_wire_contract_test.cc)
add_test(NAME input_timing_wire_contract
        COMMAND test_input_timing_wire_contract)

# Keep job lifecycle, ownership, limits, and persistence deterministic. This
# target uses injected clock and stream state; it must never contact a public
# provider or start a local server.
add_executable(test_network_test_jobs_contract
        internal/network_test_jobs_contract_test.cc
        ${CMAKE_SOURCE_DIR}/src/network_test.cpp
        ${CMAKE_SOURCE_DIR}/src/network_test_jobs.cpp
        ${CMAKE_SOURCE_DIR}/src/network_test_provider.cpp)
target_link_libraries(test_network_test_jobs_contract PRIVATE
        nlohmann_json::nlohmann_json)
add_test(NAME network_test_jobs_contract
        COMMAND test_network_test_jobs_contract)

# Exercise provider policy through a fake transport only. This prevents source
# tests from contacting Cloudflare or consuming real network bandwidth.
add_executable(test_network_test_provider_contract
        internal/network_test_provider_contract_test.cc
        ${CMAKE_SOURCE_DIR}/src/network_test.cpp
        ${CMAKE_SOURCE_DIR}/src/network_test_provider.cpp)
target_link_libraries(test_network_test_provider_contract PRIVATE
        nlohmann_json::nlohmann_json)
add_test(NAME network_test_provider_contract
        COMMAND test_network_test_provider_contract)

# Keep the certificate-only paired surface and separate administrator
# projection from regressing into the ordinary unauthenticated HTTP server.
add_executable(test_network_test_route_surface_contract
        internal/network_test_route_surface_contract_test.cc)
target_compile_definitions(test_network_test_route_surface_contract PRIVATE
        SUNSHINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME network_test_route_surface_contract
        COMMAND test_network_test_route_surface_contract)

add_executable(test_troubleshooting_network_test_ui_contract
        internal/troubleshooting_network_test_ui_contract_test.cc)
target_compile_definitions(test_troubleshooting_network_test_ui_contract PRIVATE
        SUNSHINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME troubleshooting_network_test_ui_contract
        COMMAND test_troubleshooting_network_test_ui_contract)

add_executable(test_dual_display_compositor_contract
        internal/dual_display_compositor_contract_test.cc
        ${CMAKE_SOURCE_DIR}/src/platform/windows/dual_display_compositor.cpp)
add_test(NAME dual_display_compositor_contract
        COMMAND test_dual_display_compositor_contract)

if (WIN32)
    add_executable(test_target_session_launch_ownership_contract
            internal/target_session_launch_ownership_contract_test.cc)
    target_link_libraries(test_target_session_launch_ownership_contract PRIVATE
            advapi32)
    add_test(NAME target_session_launch_ownership_contract
            COMMAND test_target_session_launch_ownership_contract)

    # Keep the pre-topology SudoVDA identity rule independent from the broad
    # test aggregate, which also links web and Boost/Asio integration tests.
    add_executable(test_sudovda_pair_identity_contract
            internal/sudovda_pair_identity_contract_test.cc)
    target_link_libraries(test_sudovda_pair_identity_contract PRIVATE
            libdisplaydevice::common
            libdisplaydevice::platform)
    add_test(NAME sudovda_pair_identity_contract
            COMMAND test_sudovda_pair_identity_contract)
endif ()

# Copy files needed for config consistency tests to build directory
# This ensures both CLI and CLion can access the same files relative to the test executable
# Using configure_file ensures files are copied when they change between builds
set(INTEGRATION_TEST_FILES
    "src/config.cpp"
    "src_assets/common/assets/web/config.html"
    "docs/configuration.md"
    "src_assets/common/assets/web/public/assets/locale/en.json"
    "src_assets/common/assets/web/configs/tabs/General.vue"
    "src_assets/linux/misc/60-sunshine.rules"
)

foreach(file ${INTEGRATION_TEST_FILES})
    configure_file(
        "${CMAKE_SOURCE_DIR}/${file}"
        "${CMAKE_CURRENT_BINARY_DIR}/${file}"
        COPYONLY
    )
endforeach()

# Copy all locale files for locale consistency tests
# Use a custom command to properly handle both adding and removing files
set(LOCALE_SRC_DIR "${CMAKE_SOURCE_DIR}/src_assets/common/assets/web/public/assets/locale")
set(LOCALE_DST_DIR "${CMAKE_CURRENT_BINARY_DIR}/src_assets/common/assets/web/public/assets/locale")
add_custom_target(sync_locale_files ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${LOCALE_DST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LOCALE_DST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${LOCALE_SRC_DIR}" "${LOCALE_DST_DIR}"
    COMMENT "Synchronizing locale files for tests"
    VERBATIM
)

foreach(dep ${SUNSHINE_TARGET_DEPENDENCIES})
    add_dependencies(${PROJECT_NAME} ${dep})  # compile these before sunshine
endforeach()

# Ensure locale files are synchronized before building the test executable
add_dependencies(${PROJECT_NAME} sync_locale_files)

# Build the list of libraries to link
set(TEST_LINK_LIBRARIES
    ${SUNSHINE_EXTERNAL_LIBRARIES}
    gtest
    ${PLATFORM_LIBRARIES}
)

if (WIN32 AND EXISTS "${FFMPEG_PREPARED_BINARIES}/include/AMF")
    # The AMF receive watchdog is implemented in FFmpeg's AMF wrapper. Keep
    # its clock-boundary contract linked against the actual FFmpeg archive so
    # the test cannot pass against a duplicated Sunshine-only model.
    add_executable(test_amf_receive_poll_contract
            internal/amf_receive_poll_contract_test.cc)
    target_link_libraries(test_amf_receive_poll_contract PRIVATE
            ${FFMPEG_LIBRARIES}
            bcrypt)
    if (TARGET ffmpeg)
        add_dependencies(test_amf_receive_poll_contract ffmpeg)
    endif ()
    add_test(NAME amf_receive_poll_contract
            COMMAND test_amf_receive_poll_contract)
endif ()

if(WIN32)
    list(APPEND TEST_LINK_LIBRARIES advapi32)
endif()

# Add the specific libgcov library if found
if(GCOV_LINK_LIBRARY)
    list(APPEND TEST_LINK_LIBRARIES ${GCOV_LINK_LIBRARY})
endif()

target_link_libraries(${PROJECT_NAME} ${TEST_LINK_LIBRARIES})
target_compile_definitions(${PROJECT_NAME} PUBLIC ${SUNSHINE_DEFINITIONS} ${TEST_DEFINITIONS})
target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${SUNSHINE_COMPILE_OPTIONS}>;$<$<COMPILE_LANGUAGE:CUDA>:${SUNSHINE_COMPILE_OPTIONS_CUDA};-std=c++17>)  # cmake-lint: disable=C0301
target_link_options(${PROJECT_NAME} PRIVATE)

if (WIN32)
    # prefer static libraries since we're linking statically
    # this fixes libcurl linking errors when using non MSYS2 version of CMake
    set_target_properties(${PROJECT_NAME} PROPERTIES LINK_SEARCH_START_STATIC 1)

    # Copy minhook-detours DLL to test binary directory for ARM64
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64" AND DEFINED _MINHOOK_DLL)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_MINHOOK_DLL}"
                "${CMAKE_CURRENT_BINARY_DIR}"
            COMMENT "Copying minhook-detours DLL to test binary directory"
            VERBATIM
        )
    endif()
endif ()
