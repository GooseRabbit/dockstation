cmake_minimum_required(VERSION 3.16)

project(DockStationLinux LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

option(DOCKSTATION_ENABLE_SANITIZERS
       "Enable AddressSanitizer and UndefinedBehaviorSanitizer for verification" OFF)

function(dockstation_apply_compile_policy target_name)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
            -fno-exceptions
            -fno-rtti
        )
    endif()
endfunction()

# The Linux runtime target contains implemented platform adapters.
# The Gateway executable will be added when its application entry point exists.
add_library(dockstation_linux_runtime STATIC
    src/platform/linux/runtime_environment.cpp
    src/platform/linux/system_diagnostics.cpp
    src/platform/linux/network_diagnostics.cpp
    src/platform/linux/runtime_config.cpp
    src/platform/logging/logger.cpp
)
target_include_directories(dockstation_linux_runtime
    PUBLIC "${PROJECT_SOURCE_DIR}/src"
)
dockstation_apply_compile_policy(dockstation_linux_runtime)

if(BUILD_TESTING)
    add_executable(test_runtime_environment
        verification/repka_gateway/test_runtime_environment.cpp
    )
    target_link_libraries(test_runtime_environment
        PRIVATE dockstation_linux_runtime
    )
    dockstation_apply_compile_policy(test_runtime_environment)
    add_test(NAME runtime_environment COMMAND test_runtime_environment)

    add_executable(test_system_diagnostics
        verification/repka_gateway/test_system_diagnostics.cpp
    )
    target_link_libraries(test_system_diagnostics
        PRIVATE dockstation_linux_runtime
    )
    dockstation_apply_compile_policy(test_system_diagnostics)
    add_test(NAME system_diagnostics COMMAND test_system_diagnostics)

    add_executable(test_network_diagnostics
        verification/repka_gateway/test_network_diagnostics.cpp
    )
    target_link_libraries(test_network_diagnostics
        PRIVATE dockstation_linux_runtime
    )
    dockstation_apply_compile_policy(test_network_diagnostics)
    add_test(NAME network_diagnostics COMMAND test_network_diagnostics)

    add_executable(test_logger
        verification/repka_gateway/test_logger.cpp
    )
    target_link_libraries(test_logger
        PRIVATE dockstation_linux_runtime
    )
    dockstation_apply_compile_policy(test_logger)
    add_test(NAME logger COMMAND test_logger)

    add_executable(test_runtime_config
        verification/repka_gateway/test_runtime_config.cpp
    )
    target_link_libraries(test_runtime_config
        PRIVATE dockstation_linux_runtime
    )
    dockstation_apply_compile_policy(test_runtime_config)
    add_test(NAME runtime_config COMMAND test_runtime_config)

    if(DOCKSTATION_ENABLE_SANITIZERS)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            foreach(target_name
                    dockstation_linux_runtime
                    test_runtime_environment
                    test_system_diagnostics
                    test_network_diagnostics
                    test_logger
                    test_runtime_config)
                target_compile_options(${target_name} PRIVATE
                    -fsanitize=address,undefined
                    -fno-omit-frame-pointer
                )
            endforeach()
            target_link_options(test_runtime_environment PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_system_diagnostics PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_network_diagnostics PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_logger PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_runtime_config PRIVATE
                -fsanitize=address,undefined
            )
        else()
            message(WARNING
                "DOCKSTATION_ENABLE_SANITIZERS is unsupported by this compiler; sanitizers are disabled.")
        endif()
    endif()
endif()
