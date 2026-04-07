idf_component_register(
    SRCS    "main.cpp" 
    	    "my_lib/config_functions.cpp"
    		"my_lib/glb_params.cpp"
    		"my_lib/help_functions.cpp"
            "my_lib/auto_eq_help.cpp"
            "my_lib/usb_device.cpp"
            "my_lib/my_tasks.cpp"
    INCLUDE_DIRS    "."
                    "my_lib"
    REQUIRES    esp_psram esp_driver_spi esp_driver_i2s spiffs esp_driver_gptimer esp_driver_gpio freertos esp_ringbuf
)
spiffs_create_partition_image(storage ../partition FLASH_IN_PROJECT)