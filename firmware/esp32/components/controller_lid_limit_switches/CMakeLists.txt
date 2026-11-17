idf_component_register(
    SRCS "lid_limit_switch_tracker.cpp"
    INCLUDE_DIRS "include"
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
