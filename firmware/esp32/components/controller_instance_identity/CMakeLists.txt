idf_component_register(
    SRCS "controller_instance_id_provider.cpp"
    INCLUDE_DIRS "include"
    REQUIRES bootloader_support esp_hw_support
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
