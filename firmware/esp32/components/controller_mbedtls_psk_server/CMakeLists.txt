idf_component_register(
    SRCS
        "controller_mbedtls_psk_server.cpp"
        "controller_link_peer_service.cpp"
        "../../../../src/hardware/interfaces/controller_codec.cpp"
        "../../../../src/hardware/interfaces/controller_frame_stream_decoder.cpp"
        "../../../../src/hardware/interfaces/controller_link_peer_session.cpp"
        "../../../../firmware/esp32/components/controller_station_logic/controller_station_command_policy.cpp"
        "../../../../firmware/esp32/components/controller_station_logic/controller_station_state.cpp"
        "../../../../firmware/esp32/components/controller_station_logic/controller_station_safety_input_adapter.cpp"
        "../../../../firmware/esp32/components/controller_lid_limit_switches/lid_limit_switch_tracker.cpp"
    INCLUDE_DIRS "include" "../../../../src"
    REQUIRES mbedtls bootloader_support
)

# Keep the firmware component on the same language baseline as the shared code.
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
