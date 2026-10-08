idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES app_update esp_driver_uart esp_event esp_http_server esp_netif esp_timer esp_wifi nvs_flash)