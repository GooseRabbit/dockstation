idf_component_register(
    SRCS "controller_uart_transport.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_driver_uart mbedtls
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
