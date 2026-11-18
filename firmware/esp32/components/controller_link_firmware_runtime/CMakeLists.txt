idf_component_register(
    SRCS
        "controller_link_firmware_runtime.cpp"
        "../../../../src/hardware/interfaces/controller_link_startup_readiness.cpp"
    INCLUDE_DIRS "include" "../../../../src"
    REQUIRES log esp_timer controller_mbedtls_psk_server
        controller_uart_transport controller_instance_identity
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
