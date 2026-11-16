cmake_minimum_required(VERSION 3.16)

project(DockStationLinux LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(GNUInstallDirs)

find_package(Threads REQUIRED)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)

# SQLite remains an isolated candidate until its persistence adapters are integrated.
# Gateway-only Repka builds can omit the SQLite development package entirely.
option(DOCKSTATION_BUILD_SQLITE_PERSISTENCE
       "Build the experimental SQLite persistence foundation" ON)
if(DOCKSTATION_BUILD_SQLITE_PERSISTENCE)
    find_package(SQLite3 3.31 REQUIRED)
endif()

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

# Shared Linux runtime support and application bootstrap components.
add_library(dockstation_linux_runtime STATIC
    src/application/linux/application_bootstrap.cpp
    src/platform/linux/runtime_environment.cpp
    src/platform/linux/platform_capabilities.cpp
    src/platform/linux/system_diagnostics.cpp
    src/platform/linux/network_diagnostics.cpp
    src/platform/linux/runtime_config.cpp
    src/platform/linux/command_line.cpp
    src/platform/linux/runtime_config_file.cpp
    src/platform/logging/logger.cpp
    src/hardware/interfaces/controller_codec.cpp
    src/hardware/interfaces/controller_frame_stream_decoder.cpp
    src/hardware/interfaces/controller_link_session.cpp
    src/hardware/interfaces/controller_telemetry_snapshot.cpp
    src/platform/linux/controller_link_service.cpp
    src/communications/api_serialization.cpp
    src/communications/local_api_server.cpp
)
target_include_directories(dockstation_linux_runtime
    PUBLIC "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(dockstation_linux_runtime
    PUBLIC Threads::Threads
)
dockstation_apply_compile_policy(dockstation_linux_runtime)

# Keep the TLS adapter independent from Gateway orchestration. The transport
# library composes it with POSIX UART; ControllerLinkService owns runtime wiring.
add_library(dockstation_controller_tls STATIC
    src/platform/linux/controller_psk_tls_client.cpp
    src/platform/linux/controller_psk_credential_store.cpp
)
target_include_directories(dockstation_controller_tls
    PUBLIC "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(dockstation_controller_tls
    PUBLIC OpenSSL::SSL OpenSSL::Crypto
)
dockstation_apply_compile_policy(dockstation_controller_tls)

# Compose the POSIX UART byte stream and OpenSSL TLS client without
# coupling either adapter to the current read-only Gateway runtime.
add_library(dockstation_controller_transport STATIC
    src/platform/linux/controller_uart_port.cpp
    src/platform/linux/controller_transport_manager.cpp
)
target_include_directories(dockstation_controller_transport
    PUBLIC "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(dockstation_controller_transport
    PUBLIC dockstation_controller_tls
)
dockstation_apply_compile_policy(dockstation_controller_transport)

if(DOCKSTATION_BUILD_SQLITE_PERSISTENCE)
    # Initial SQLite foundation is isolated from station business logic.
    add_library(dockstation_persistence_sqlite STATIC
        src/platform/persistence/sqlite_database.cpp
    )
    target_include_directories(dockstation_persistence_sqlite
        PUBLIC "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(dockstation_persistence_sqlite
        PUBLIC SQLite::SQLite3
    )
    dockstation_apply_compile_policy(dockstation_persistence_sqlite)
endif()


# The Gateway runtime owns the controller service and reuses the transport
# library without introducing a dependency back from transport into Gateway.
target_link_libraries(dockstation_linux_runtime
    PUBLIC dockstation_controller_transport
)

# Keep station event processing independent from the Linux hardware runtime.
add_library(dockstation_station_events STATIC
    src/core/event_processing.cpp
    src/core/event_validation.cpp
    src/core/event_handling.cpp
    src/core/event_processor_router.cpp
    src/core/source_session.cpp
    src/core/source_session_coordinator.cpp
    src/core/source_session_verifier.cpp
    src/core/station_event_pipeline.cpp
)
target_include_directories(dockstation_station_events
    PUBLIC "${PROJECT_SOURCE_DIR}/src"
)
dockstation_apply_compile_policy(dockstation_station_events)

# The authenticated ingress adapter bridges accepted ControllerLink messages
# to the session-gated SSM pipeline without coupling core to Linux/OpenSSL.
add_library(dockstation_event_transport STATIC
    src/communications/authenticated_event_transport_adapter.cpp
    src/communications/controller_subsystem_status_event_projector.cpp
)
target_include_directories(dockstation_event_transport
    PUBLIC "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(dockstation_event_transport
    PUBLIC dockstation_linux_runtime dockstation_station_events
)
dockstation_apply_compile_policy(dockstation_event_transport)

# Local-only PSK provisioning utility. It is not part of the Gateway runtime.
add_executable(dockstation-credential-provision
    tools/controller_psk_credential_provision.cpp
)
target_link_libraries(dockstation-credential-provision
    PRIVATE dockstation_controller_tls
)
dockstation_apply_compile_policy(dockstation-credential-provision)

add_executable(dockstation_linux_bootstrap
    repka_gateway/main.cpp
)
target_link_libraries(dockstation_linux_bootstrap
    PRIVATE dockstation_linux_runtime
)
dockstation_apply_compile_policy(dockstation_linux_bootstrap)

if(BUILD_TESTING)
    # Keep the credential-loader injection seam out of production/release builds.
    target_compile_definitions(dockstation_linux_runtime
        PUBLIC DOCKSTATION_ENABLE_TEST_SEAMS)

    add_executable(test_application_bootstrap
        verification/repka_gateway/test_application_bootstrap.cpp
    )
    target_link_libraries(test_application_bootstrap
        PRIVATE dockstation_linux_runtime
    )
    dockstation_apply_compile_policy(test_application_bootstrap)
    add_test(NAME application_bootstrap COMMAND test_application_bootstrap)

    add_executable(test_runtime_environment
        verification/repka_gateway/test_runtime_environment.cpp
    )
    target_link_libraries(test_runtime_environment
        PRIVATE dockstation_linux_runtime
    )
    dockstation_apply_compile_policy(test_runtime_environment)
    add_test(NAME runtime_environment COMMAND test_runtime_environment)

    add_executable(test_platform_capabilities
        verification/repka_gateway/test_platform_capabilities.cpp
    )
    target_link_libraries(test_platform_capabilities
        PRIVATE dockstation_linux_runtime
    )
    dockstation_apply_compile_policy(test_platform_capabilities)
    add_test(NAME platform_capabilities COMMAND test_platform_capabilities)

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

    add_executable(test_command_line
        verification/repka_gateway/test_command_line.cpp
    )
    target_link_libraries(test_command_line
        PRIVATE dockstation_linux_runtime
    )
    dockstation_apply_compile_policy(test_command_line)
    add_test(NAME command_line COMMAND test_command_line)

    add_executable(test_runtime_config_file
        verification/repka_gateway/test_runtime_config_file.cpp
    )
    target_link_libraries(test_runtime_config_file
        PRIVATE dockstation_linux_runtime
    )
    dockstation_apply_compile_policy(test_runtime_config_file)
    add_test(NAME runtime_config_file COMMAND test_runtime_config_file)

    add_executable(test_controller_protocol
        verification/repka_gateway/test_controller_protocol.cpp
    )
    target_link_libraries(test_controller_protocol
        PRIVATE dockstation_linux_runtime
    )
    dockstation_apply_compile_policy(test_controller_protocol)
    add_test(NAME controller_protocol COMMAND test_controller_protocol)

    add_executable(test_controller_codec
        verification/repka_gateway/test_controller_codec.cpp
    )
    target_link_libraries(test_controller_codec
        PRIVATE dockstation_linux_runtime
    )
    dockstation_apply_compile_policy(test_controller_codec)
    add_test(NAME controller_codec COMMAND test_controller_codec)

    add_executable(test_controller_frame_stream_decoder
        verification/repka_gateway/test_controller_frame_stream_decoder.cpp
    )
    target_link_libraries(test_controller_frame_stream_decoder
        PRIVATE dockstation_linux_runtime
    )
    dockstation_apply_compile_policy(test_controller_frame_stream_decoder)
    add_test(NAME controller_frame_stream_decoder
        COMMAND test_controller_frame_stream_decoder)

    add_executable(test_controller_link_session
        verification/repka_gateway/test_controller_link_session.cpp
    )
    target_link_libraries(test_controller_link_session
        PRIVATE dockstation_linux_runtime
    )
    dockstation_apply_compile_policy(test_controller_link_session)
    add_test(NAME controller_link_session
        COMMAND test_controller_link_session)

    add_executable(test_controller_psk_tls_client
        verification/repka_gateway/test_controller_psk_tls_client.cpp
    )
    target_link_libraries(test_controller_psk_tls_client
        PRIVATE dockstation_controller_tls
    )
    dockstation_apply_compile_policy(test_controller_psk_tls_client)
    add_test(NAME controller_psk_tls_client
        COMMAND test_controller_psk_tls_client)

    add_executable(test_controller_uart_port
        verification/repka_gateway/test_controller_uart_port.cpp
    )
    target_link_libraries(test_controller_uart_port
        PRIVATE dockstation_controller_transport
    )
    dockstation_apply_compile_policy(test_controller_uart_port)
    add_test(NAME controller_uart_port COMMAND test_controller_uart_port)

    add_executable(test_controller_transport_manager
        verification/repka_gateway/test_controller_transport_manager.cpp
    )
    target_link_libraries(test_controller_transport_manager
        PRIVATE dockstation_controller_transport
    )
    dockstation_apply_compile_policy(test_controller_transport_manager)
    add_test(NAME controller_transport_manager COMMAND test_controller_transport_manager)

    add_executable(test_controller_telemetry_snapshot
        verification/repka_gateway/test_controller_telemetry_snapshot.cpp
    )
    target_link_libraries(test_controller_telemetry_snapshot
        PRIVATE dockstation_linux_runtime
    )
    dockstation_apply_compile_policy(test_controller_telemetry_snapshot)
    add_test(NAME controller_telemetry_snapshot
        COMMAND test_controller_telemetry_snapshot)

    add_executable(test_controller_subsystem_status_event_projector
        verification/repka_gateway/test_controller_subsystem_status_event_projector.cpp
    )
    target_link_libraries(test_controller_subsystem_status_event_projector
        PRIVATE dockstation_event_transport
    )
    dockstation_apply_compile_policy(test_controller_subsystem_status_event_projector)
    add_test(NAME controller_subsystem_status_event_projector
        COMMAND test_controller_subsystem_status_event_projector)

    add_executable(test_controller_link_service
        verification/repka_gateway/test_controller_link_service.cpp
    )
    target_link_libraries(test_controller_link_service
        PRIVATE dockstation_event_transport
    )
    dockstation_apply_compile_policy(test_controller_link_service)
    add_test(NAME controller_link_service COMMAND test_controller_link_service)

    if(DOCKSTATION_BUILD_SQLITE_PERSISTENCE)
        add_executable(test_sqlite_persistence_probe
            verification/repka_gateway/test_sqlite_persistence_probe.cpp
        )
        target_link_libraries(test_sqlite_persistence_probe
            PRIVATE dockstation_persistence_sqlite
        )
        dockstation_apply_compile_policy(test_sqlite_persistence_probe)
        add_test(NAME sqlite_persistence_probe COMMAND test_sqlite_persistence_probe)
    endif()

    add_executable(test_controller_psk_credential_store
        verification/repka_gateway/test_controller_psk_credential_store.cpp
    )
    target_link_libraries(test_controller_psk_credential_store
        PRIVATE dockstation_controller_tls
    )
    dockstation_apply_compile_policy(test_controller_psk_credential_store)
    add_test(NAME controller_psk_credential_store_rejects_relative_path
        COMMAND test_controller_psk_credential_store expect-failure relative/path)

    add_test(NAME controller_psk_credential_provision_help
        COMMAND dockstation-credential-provision --help)

    add_executable(test_local_api
        verification/repka_gateway/test_local_api.cpp
    )
    target_link_libraries(test_local_api
        PRIVATE dockstation_linux_runtime
    )
    dockstation_apply_compile_policy(test_local_api)
    add_test(NAME local_api COMMAND test_local_api)

    add_executable(test_source_cursor_reader
        src/core/event_processing.cpp
        verification/regression/test_source_cursor_reader.cpp
    )
    target_include_directories(test_source_cursor_reader
        PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    dockstation_apply_compile_policy(test_source_cursor_reader)
    add_test(NAME source_cursor_reader COMMAND test_source_cursor_reader)

    add_executable(test_source_session_contract
        src/core/event_processing.cpp
        src/core/source_session.cpp
        src/core/source_session_coordinator.cpp
        verification/regression/test_source_session_contract.cpp
    )
    target_include_directories(test_source_session_contract
        PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    dockstation_apply_compile_policy(test_source_session_contract)
    add_test(NAME source_session_contract COMMAND test_source_session_contract)

    add_executable(test_source_session_verifier
        src/core/event_processing.cpp
        src/core/source_session.cpp
        src/core/source_session_verifier.cpp
        verification/regression/test_source_session_verifier.cpp
    )
    target_include_directories(test_source_session_verifier
        PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    dockstation_apply_compile_policy(test_source_session_verifier)
    add_test(NAME source_session_verifier COMMAND test_source_session_verifier)

    add_executable(test_subsystem_status_event_processor
        src/core/event_processing.cpp
        src/core/event_validation.cpp
        src/core/subsystem_status_event_processor.cpp
        verification/repka_gateway/test_subsystem_status_event_processor.cpp
    )
    target_include_directories(test_subsystem_status_event_processor
        PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    dockstation_apply_compile_policy(test_subsystem_status_event_processor)
    add_test(NAME subsystem_status_event_processor
        COMMAND test_subsystem_status_event_processor)

    add_executable(test_station_event_pipeline
        src/core/event_processing.cpp
        src/core/event_validation.cpp
        src/core/event_handling.cpp
        src/core/event_processor_router.cpp
        src/core/station_event_pipeline.cpp
        src/core/source_session.cpp
        src/core/source_session_coordinator.cpp
        verification/regression/test_station_event_pipeline.cpp
    )
    target_include_directories(test_station_event_pipeline
        PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    dockstation_apply_compile_policy(test_station_event_pipeline)
    add_test(NAME station_event_pipeline COMMAND test_station_event_pipeline)

    if(DOCKSTATION_ENABLE_SANITIZERS)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            foreach(target_name
                    dockstation_linux_runtime
                    dockstation_controller_tls
                    dockstation_controller_transport
                    dockstation_station_events
                    dockstation_event_transport
                    dockstation-credential-provision
                    dockstation_linux_bootstrap
                    test_application_bootstrap
                    test_runtime_environment
                    test_platform_capabilities
                    test_system_diagnostics
                    test_network_diagnostics
                    test_logger
                    test_runtime_config
                    test_command_line
                    test_runtime_config_file
                    test_controller_protocol
                    test_controller_codec
                    test_controller_frame_stream_decoder
                    test_controller_link_session
                    test_controller_psk_tls_client
                    test_controller_uart_port
                    test_controller_transport_manager
                    test_controller_subsystem_status_event_projector
                    test_controller_link_service
                    test_controller_telemetry_snapshot
                    test_controller_psk_credential_store
                    test_local_api
                    test_source_cursor_reader
                    test_source_session_contract
                    test_source_session_verifier
                    test_subsystem_status_event_processor
                    test_station_event_pipeline)
                target_compile_options(${target_name} PRIVATE
                    -fsanitize=address,undefined
                    -fno-omit-frame-pointer
                )
            endforeach()
            target_link_options(dockstation-credential-provision PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(dockstation_linux_bootstrap PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_application_bootstrap PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_runtime_environment PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_platform_capabilities PRIVATE
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
            target_link_options(test_command_line PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_runtime_config_file PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_controller_protocol PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_controller_codec PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_controller_frame_stream_decoder PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_controller_link_session PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_controller_psk_tls_client PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_controller_uart_port PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_controller_transport_manager PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_controller_subsystem_status_event_projector PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_controller_link_service PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_controller_telemetry_snapshot PRIVATE
                -fsanitize=address,undefined
            )
            if(DOCKSTATION_BUILD_SQLITE_PERSISTENCE)
                target_compile_options(test_sqlite_persistence_probe PRIVATE
                    -fsanitize=address,undefined
                    -fno-omit-frame-pointer
                )
                target_link_options(test_sqlite_persistence_probe PRIVATE
                    -fsanitize=address,undefined
                )
            endif()
            target_link_options(test_controller_psk_credential_store PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_local_api PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_source_cursor_reader PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_source_session_contract PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_source_session_verifier PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_subsystem_status_event_processor PRIVATE
                -fsanitize=address,undefined
            )
            target_link_options(test_station_event_pipeline PRIVATE
                -fsanitize=address,undefined
            )
        else()
            message(WARNING
                "DOCKSTATION_ENABLE_SANITIZERS is unsupported by this compiler; sanitizers are disabled.")
        endif()
    endif()
endif()

# Install the production C++ Gateway and its systemd deployment assets.
configure_file(
    "${PROJECT_SOURCE_DIR}/repka/dockstation-gateway.service.in"
    "${PROJECT_BINARY_DIR}/dockstation-gateway.service"
    @ONLY
)
configure_file(
    "${PROJECT_SOURCE_DIR}/repka/dockstation-gateway-uart@.service.in"
    "${PROJECT_BINARY_DIR}/dockstation-gateway-uart@.service"
    @ONLY
)
install(TARGETS dockstation_linux_bootstrap dockstation-credential-provision
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(FILES "${PROJECT_BINARY_DIR}/dockstation-gateway.service"
    DESTINATION "lib/systemd/system"
)
install(FILES "${PROJECT_BINARY_DIR}/dockstation-gateway-uart@.service"
    DESTINATION "lib/systemd/system"
)
install(FILES "${PROJECT_SOURCE_DIR}/repka/gateway.conf.example"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/dockstation"
)
