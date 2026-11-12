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

# The first Linux target contains only the implemented platform adapter.
# The Gateway executable will be added when its application entry point exists.
add_library(dockstation_linux_runtime STATIC
    src/platform/linux/runtime_environment.cpp
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

    if(DOCKSTATION_ENABLE_SANITIZERS)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(dockstation_linux_runtime PRIVATE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
            )
            target_compile_options(test_runtime_environment PRIVATE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
            )
            target_link_options(test_runtime_environment PRIVATE
                -fsanitize=address,undefined
            )
        else()
            message(WARNING
                "DOCKSTATION_ENABLE_SANITIZERS is unsupported by this compiler; sanitizers are disabled.")
        endif()
    endif()

    add_test(NAME runtime_environment COMMAND test_runtime_environment)
endif()
