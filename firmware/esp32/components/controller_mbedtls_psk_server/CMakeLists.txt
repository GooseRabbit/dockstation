idf_component_register(
    SRCS
        "controller_mbedtls_psk_server.cpp"
        "controller_link_peer_service.cpp"
        "../../../../src/hardware/interfaces/controller_codec.cpp"
        "../../../../src/hardware/interfaces/controller_frame_stream_decoder.cpp"
        "../../../../src/hardware/interfaces/controller_link_peer_session.cpp"
    INCLUDE_DIRS "include" "../../../../src"
    REQUIRES mbedtls
)

# Keep the firmware component on the same language baseline as the shared code.
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
