idf_component_register(
    SRCS "controller_psk_nvs_credential_store.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        controller_mbedtls_psk_server
        nvs_flash
        esp_partition
        bootloader_support
        mbedtls
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
