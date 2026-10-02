cmake_minimum_required(VERSION 3.16)

project(SFUCore VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LIBDATACHANNEL_NUGET_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdatachannel_nuget" CACHE PATH "Path to extracted libdatachannel NuGet package")

if(WIN32 AND EXISTS "${LIBDATACHANNEL_NUGET_ROOT}/include/rtc/rtc.hpp")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(LIBDATACHANNEL_NUGET_ARCH "x64")
    else()
        set(LIBDATACHANNEL_NUGET_ARCH "win32")
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(LIBDATACHANNEL_NUGET_CONFIG "release")
    else()
        set(LIBDATACHANNEL_NUGET_CONFIG "debug")
    endif()

    set(LIBDATACHANNEL_NUGET_LIB_DIR "${LIBDATACHANNEL_NUGET_ROOT}/lib/windows/${LIBDATACHANNEL_NUGET_ARCH}/${LIBDATACHANNEL_NUGET_CONFIG}")
    set(LIBDATACHANNEL_NUGET_BIN_DIR "${LIBDATACHANNEL_NUGET_ROOT}/bin/windows/${LIBDATACHANNEL_NUGET_ARCH}/${LIBDATACHANNEL_NUGET_CONFIG}")

    add_library(libdatachannel_prebuilt INTERFACE)
    target_include_directories(libdatachannel_prebuilt INTERFACE "${LIBDATACHANNEL_NUGET_ROOT}/include")
    target_link_directories(libdatachannel_prebuilt INTERFACE "${LIBDATACHANNEL_NUGET_LIB_DIR}")
    set(LIBDATACHANNEL_PREBUILT_LIBS datachannel)
    foreach(LIBDATACHANNEL_OPTIONAL_LIB juice usrsctp libcrypto libssl)
        if(EXISTS "${LIBDATACHANNEL_NUGET_LIB_DIR}/${LIBDATACHANNEL_OPTIONAL_LIB}.lib")
            list(APPEND LIBDATACHANNEL_PREBUILT_LIBS "${LIBDATACHANNEL_OPTIONAL_LIB}")
        endif()
    endforeach()
    target_link_libraries(libdatachannel_prebuilt INTERFACE ${LIBDATACHANNEL_PREBUILT_LIBS})
    target_compile_definitions(libdatachannel_prebuilt INTERFACE SFU_HAS_LIBDATACHANNEL=1)
    if(EXISTS "${LIBDATACHANNEL_NUGET_ROOT}/datachannel.def")
        target_compile_definitions(libdatachannel_prebuilt INTERFACE SFU_USE_LIBDATACHANNEL_C_API=1)
    endif()
endif()

add_library(sfu_core
    src/metrics.cpp
    src/participant.cpp
    src/room.cpp
    src/rtp_packet.cpp
    src/signaling.cpp
    src/transport.cpp
    src/udp_transport.cpp
    src/webrtc_backend.cpp
)

target_include_directories(sfu_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(sfu_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(sfu_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(sfu_core PUBLIC ws2_32)
endif()

if(TARGET libdatachannel_prebuilt)
    target_link_libraries(sfu_core PUBLIC libdatachannel_prebuilt)

    add_custom_target(copy_libdatachannel_dlls ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${LIBDATACHANNEL_NUGET_BIN_DIR}"
            "$<TARGET_FILE_DIR:sfu_server>"
        COMMENT "Copying libdatachannel runtime DLLs")
endif()

add_executable(sfu_server
    src/main.cpp
)

target_link_libraries(sfu_server PRIVATE sfu_core)

add_executable(udp_rtp_demo
    src/udp_rtp_demo.cpp
)

target_link_libraries(udp_rtp_demo PRIVATE sfu_core)

add_executable(udp_rtp_client
    src/udp_rtp_client.cpp
)

target_link_libraries(udp_rtp_client PRIVATE sfu_core)

add_executable(udp_loopback_demo
    src/udp_loopback_demo.cpp
)

target_link_libraries(udp_loopback_demo PRIVATE sfu_core)

add_executable(tcp_signaling_server
    src/tcp_signaling_server.cpp
)

target_link_libraries(tcp_signaling_server PRIVATE sfu_core)

add_executable(tcp_signaling_client
    src/tcp_signaling_client.cpp
)

target_link_libraries(tcp_signaling_client PRIVATE sfu_core)

add_executable(ws_signaling_server
    src/ws_signaling_server.cpp
)

target_link_libraries(ws_signaling_server PRIVATE sfu_core)

if(TARGET libdatachannel_prebuilt)
    add_executable(libdatachannel_smoke
        tests/libdatachannel_smoke.cpp
    )

    target_link_libraries(libdatachannel_smoke PRIVATE sfu_core)
    add_dependencies(libdatachannel_smoke copy_libdatachannel_dlls)
    add_test(NAME libdatachannel_smoke COMMAND libdatachannel_smoke)
endif()

enable_testing()

add_executable(sfu_core_tests
    tests/core_tests.cpp
)

target_link_libraries(sfu_core_tests PRIVATE sfu_core)

add_test(NAME sfu_core_tests COMMAND sfu_core_tests)

add_executable(udp_transport_tests
    tests/udp_transport_tests.cpp
)

target_link_libraries(udp_transport_tests PRIVATE sfu_core)

add_test(NAME udp_transport_tests COMMAND udp_transport_tests)

add_executable(udp_sfu_integration_tests
    tests/udp_sfu_integration_tests.cpp
)

target_link_libraries(udp_sfu_integration_tests PRIVATE sfu_core)

add_test(NAME udp_sfu_integration_tests COMMAND udp_sfu_integration_tests)

add_executable(signaling_tests
    tests/signaling_tests.cpp
)

target_link_libraries(signaling_tests PRIVATE sfu_core)

add_test(NAME signaling_tests COMMAND signaling_tests)
