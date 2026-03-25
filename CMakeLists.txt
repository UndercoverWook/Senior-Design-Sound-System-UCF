idf_component_register(
    SRCS "WavStream_and_BLE.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        spiffs
        nvs_flash
        bt
)