idf_component_register(
    SRCS "controller_mbedtls_psk_server.cpp"
    INCLUDE_DIRS "include" "../../../../src"
    REQUIRES mbedtls
)
