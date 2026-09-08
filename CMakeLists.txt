cmake_minimum_required(VERSION 3.10)
project(rc_receiver LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(_RC_RECEIVER_INSTALL_STANDALONE_CONFIG OFF)
if(NOT DEFINED SROBOTIS_EXPORT_SET)
    set(SROBOTIS_EXPORT_SET "ComponentsTargets")
    set(_RC_RECEIVER_INSTALL_STANDALONE_CONFIG ON)
endif()

set(RC_RECEIVER_CORE_SOURCES
    src/rc_receiver_core.c
)

# Target files may select a driver through enabled_drivers. Keep the UART
# implementation as the default so a standalone build remains useful.
if(DEFINED SROBOTIS_PERIPHERALS_RC_RECEIVER_ENABLED_DRIVERS)
    set(_rc_receiver_enabled_drivers
        ${SROBOTIS_PERIPHERALS_RC_RECEIVER_ENABLED_DRIVERS})
else()
    set(_rc_receiver_enabled_drivers drv_uart_sbus)
endif()
set(RC_RECEIVER_DRIVER_SOURCES)
foreach(_driver IN LISTS _rc_receiver_enabled_drivers)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/drivers/${_driver}.c")
        list(APPEND RC_RECEIVER_DRIVER_SOURCES
            "src/drivers/${_driver}.c")
    else()
        message(FATAL_ERROR
            "rc_receiver: enabled driver '${_driver}' not found at "
            "src/drivers/${_driver}.c")
    endif()
endforeach()

add_library(rc_receiver SHARED
    ${RC_RECEIVER_CORE_SOURCES}
    ${RC_RECEIVER_DRIVER_SOURCES}
)

find_package(Threads REQUIRED)
target_link_libraries(rc_receiver PRIVATE Threads::Threads)

target_include_directories(rc_receiver
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(rc_receiver PUBLIC c_std_11)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rc_receiver PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

# Support both build-tree binaries and the SDK's installed bin/ + lib/ layout.
set(_RC_RECEIVER_RUNTIME_RPATH "$ORIGIN;$ORIGIN/../lib")
function(_rc_receiver_set_runtime_rpath target)
    set_target_properties(${target} PROPERTIES
        BUILD_RPATH "${_RC_RECEIVER_RUNTIME_RPATH}"
        INSTALL_RPATH "${_RC_RECEIVER_RUNTIME_RPATH}"
    )
endfunction()

if(_RC_RECEIVER_INSTALL_STANDALONE_CONFIG)
    # A standalone build consumes this package with find_package(rc_receiver).
    # The SDK aggregate build uses ComponentsTargets instead (see the other
    # components/peripherals packages).
    install(TARGETS rc_receiver
        EXPORT rc_receiverTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
    )
    install(EXPORT rc_receiverTargets
        FILE rc_receiverConfig.cmake
        NAMESPACE rc_receiver::
        DESTINATION lib/cmake/rc_receiver
    )
else()
    install(TARGETS rc_receiver
        EXPORT ${SROBOTIS_EXPORT_SET}
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
    )
endif()

install(FILES include/rc_receiver.h DESTINATION include)
install(FILES package.xml README.md LICENSE DESTINATION share/rc_receiver)

option(BUILD_TESTS "Build rc_receiver test and diagnostic programs" OFF)
if(BUILD_TESTS)
    enable_testing()

    list(FIND _rc_receiver_enabled_drivers "drv_uart_sbus" _rc_receiver_sbus_idx)
    if(_rc_receiver_sbus_idx GREATER -1)
        add_executable(test_rc_receiver example/test_rc_receiver.c)
        target_link_libraries(test_rc_receiver PRIVATE rc_receiver)
        target_compile_options(test_rc_receiver PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
        _rc_receiver_set_runtime_rpath(test_rc_receiver)
        install(TARGETS test_rc_receiver RUNTIME DESTINATION bin)

        add_executable(test_sbus_parser tests/test_sbus_parser.c)
        target_link_libraries(test_sbus_parser PRIVATE
            rc_receiver
            Threads::Threads
        )
        target_compile_options(test_sbus_parser PRIVATE
            -UNDEBUG
            -Wall
            -Wextra
            -Wpedantic
        )
        _rc_receiver_set_runtime_rpath(test_sbus_parser)

        add_test(NAME rc_receiver_sbus_parser COMMAND test_sbus_parser)
        set_tests_properties(rc_receiver_sbus_parser PROPERTIES TIMEOUT 30)

        add_test(
            NAME rc_receiver_api_contract
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rc_receiver_api_contract.sh
                    $<TARGET_FILE:rc_receiver>
                    ${CMAKE_CURRENT_SOURCE_DIR}/include
        )
        set_tests_properties(rc_receiver_api_contract PROPERTIES TIMEOUT 30)

        add_executable(test_rc_receiver_core tests/test_rc_receiver_core.c)
        target_link_libraries(test_rc_receiver_core PRIVATE rc_receiver)
        target_include_directories(test_rc_receiver_core PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        target_compile_options(test_rc_receiver_core PRIVATE
            -UNDEBUG
            -Wall
            -Wextra
            -Wpedantic
        )
        _rc_receiver_set_runtime_rpath(test_rc_receiver_core)

        add_test(NAME rc_receiver_callback_functional
            COMMAND test_rc_receiver_core functional)
        add_test(NAME rc_receiver_error_paths
            COMMAND test_rc_receiver_core error-paths)
        add_test(NAME rc_receiver_callback_stability
            COMMAND test_rc_receiver_core stability)
        set_tests_properties(
            rc_receiver_callback_functional
            rc_receiver_error_paths
            rc_receiver_callback_stability
            PROPERTIES TIMEOUT 30
        )
    endif()
endif()
