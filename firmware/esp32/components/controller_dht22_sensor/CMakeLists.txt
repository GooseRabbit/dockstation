idf_component_register(
    SRCS "dht22_frame_decoder.cpp"
    INCLUDE_DIRS "include"
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
