idf_component_register(
    SRCS "DHT.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer esp_rom freertos log
)
