idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES log controller_link_firmware_runtime controller_psk_nvs_credential_store
        controller_instance_identity
)

# Force the link-only acceptance hook into the ELF without executing it.
target_link_options(${COMPONENT_LIB} INTERFACE
    "LINKER:-u,dockstation_link_check_psk_nvs_credential_store"
    "LINKER:-u,dockstation_link_check_controller_link_firmware_runtime"
)
