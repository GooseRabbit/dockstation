idf_component_register(
    SRCS
        "dht22_frame_decoder.cpp"
        "dht22_telemetry_adapter.cpp"
    INCLUDE_DIRS "include" "../../../../src"
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
