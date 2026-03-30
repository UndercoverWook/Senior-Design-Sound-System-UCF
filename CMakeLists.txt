# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.

idf_component_register(
    SRCS main.cpp
    INCLUDE_DIRS        "."
    REQUIRES            esp_psram esp_driver_spi esp_driver_i2s spiffs esp_audio_codec esp_driver_gptimer esp_driver_gpio
)
spiffs_create_partition_image(storage ../partition FLASH_IN_PROJECT)