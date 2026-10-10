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
