idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES log controller_mbedtls_psk_server controller_uart_transport
)
