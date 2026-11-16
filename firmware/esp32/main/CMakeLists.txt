idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES log controller_link_firmware_runtime controller_psk_nvs_credential_store
)
