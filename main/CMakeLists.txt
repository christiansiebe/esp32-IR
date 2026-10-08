set(main_requires
    app_update
    esp_driver_uart
    esp_event
    esp_http_server
    esp_netif
    esp_timer
    esp_wifi
    nvs_flash
    sml_transport
)
if(CONFIG_ESPNOW_ROLE_SENDER)
    list(APPEND main_requires sml_parser)
endif()

idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES ${main_requires})