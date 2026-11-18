idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "." "../../../src"
    REQUIRES log esp_timer freertos heap controller_link_firmware_runtime controller_psk_nvs_credential_store
        controller_instance_identity controller_uart_transport controller_dht22_sensor
        controller_lid_limit_switches
)

# Force the link-only acceptance hook into the ELF without executing it.
target_link_options(${COMPONENT_LIB} INTERFACE
    "LINKER:-u,dockstation_link_check_psk_nvs_credential_store"
    "LINKER:-u,dockstation_link_check_controller_link_firmware_runtime"
)
