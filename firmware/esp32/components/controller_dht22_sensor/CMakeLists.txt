idf_component_register(
    SRCS
        "dht22_frame_decoder.cpp"
        "dht22_telemetry_adapter.cpp"
        "dht22_sampling_scheduler.cpp"
        "dht22_gpio_reader.cpp"
    INCLUDE_DIRS "include" "../../../../src"
    REQUIRES driver esp_timer esp_rom
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
