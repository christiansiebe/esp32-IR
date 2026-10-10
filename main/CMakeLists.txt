set(main_requires
    app_update
    esp_partition
    esp_driver_uart
    esp_event
    esp_http_server
    esp_netif
    esp_timer
    esp_wifi
    nvs_flash
    spiffs
    sml_transport
    sml_parser
)

idf_component_register(SRCS "main.c" "power_history.c"
                    INCLUDE_DIRS "."
                    REQUIRES ${main_requires})

# Generate a valid, empty SPIFFS image for the power-history "storage"
# partition and include it in the flash targets, so a freshly flashed device
# already has a mountable partition and needs no first-boot formatting.
spiffs_create_partition_image(storage ${CMAKE_CURRENT_SOURCE_DIR}/spiffs_root FLASH_IN_PROJECT)
