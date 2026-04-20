if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.28)
    project(eq-cli LANGUAGES CXX C)

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    include(CTest)
    find_package(PkgConfig REQUIRED)

    pkg_check_modules(LIBPIPEWIRE libpipewire-0.3>=1.0.5 IMPORTED_TARGET REQUIRED)
    pkg_check_modules(LIBLILV lilv-0>=0.24 IMPORTED_TARGET REQUIRED)
    pkg_check_modules(LIBNLOHMANNJSON nlohmann_json IMPORTED_TARGET REQUIRED)

    set(EE_CORE_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src)
else()
    set(EE_CORE_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/src)
endif()

add_executable(eq-cli)

target_sources(eq-cli PRIVATE
    src/main.cpp
    src/cli_args.cpp
    src/daemon_controller.cpp
    src/daemon_ipc.cpp
    src/daemon_protocol.cpp
    src/daemon_backend.cpp
    src/preset_source.cpp
    src/ee_eq_preset_parser.cpp
    src/lsp_eq_port_mapper.cpp
    src/lsp_limiter_port_mapper.cpp
    src/kernel_resolver.cpp
    src/convolver_host.cpp
    src/lv2_host_core.cpp
    src/pipewire_router.cpp
)

target_include_directories(eq-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${EE_CORE_INCLUDE_DIR}
)

target_link_libraries(eq-cli PRIVATE
    PkgConfig::LIBPIPEWIRE
    PkgConfig::LIBLILV
    PkgConfig::LIBNLOHMANNJSON
)

pkg_check_modules(LIBSNDFILE sndfile IMPORTED_TARGET REQUIRED)
target_link_libraries(eq-cli PRIVATE
    PkgConfig::LIBSNDFILE
)

find_library(ZITACONVOLVER_LIB NAMES zita-convolver REQUIRED)
target_link_libraries(eq-cli PRIVATE ${ZITACONVOLVER_LIB})

if(BUILD_TESTING)
    add_executable(eq-cli-tests)
    target_sources(eq-cli-tests PRIVATE
        tests/test_main.cpp
        src/cli_args.cpp
        src/preset_source.cpp
        src/ee_eq_preset_parser.cpp
        src/kernel_resolver.cpp
        src/convolver_host.cpp
    )
    target_include_directories(eq-cli-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${EE_CORE_INCLUDE_DIR}
    )
    target_link_libraries(eq-cli-tests PRIVATE
        PkgConfig::LIBNLOHMANNJSON
        PkgConfig::LIBSNDFILE
        ${ZITACONVOLVER_LIB}
    )
    target_compile_definitions(eq-cli-tests PRIVATE
        EE_EQ_CLI_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    add_test(NAME eq-cli-tests COMMAND eq-cli-tests)

    add_executable(eq-cli-daemon-tests)
    target_sources(eq-cli-daemon-tests PRIVATE
        tests/test_daemon.cpp
        src/daemon_controller.cpp
        src/daemon_ipc.cpp
        src/daemon_protocol.cpp
        src/preset_source.cpp
        src/ee_eq_preset_parser.cpp
    )
    target_include_directories(eq-cli-daemon-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${EE_CORE_INCLUDE_DIR}
    )
    target_link_libraries(eq-cli-daemon-tests PRIVATE
        PkgConfig::LIBNLOHMANNJSON
    )
    target_compile_definitions(eq-cli-daemon-tests PRIVATE
        EE_EQ_CLI_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    add_test(NAME eq-cli-daemon-tests COMMAND eq-cli-daemon-tests)
endif()
