idf_component_register(
    SRCS "controller_link_firmware_runtime.cpp"
    INCLUDE_DIRS "include"
    REQUIRES controller_mbedtls_psk_server controller_uart_transport
        controller_instance_identity
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
