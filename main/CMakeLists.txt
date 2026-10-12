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

# ESP-IDF v6: ESP-MQTT and cJSON are managed components. They are declared in
# idf_component.yml and become dependencies of "main" automatically, so they do
# not need to be named in REQUIRES (see the IDF v6 migration guide for cJSON).
set(main_srcs "main.c" "power_history.c" "web_console.c"
              "boot_policy.c" "boot_confirm.c"
              "crash_journal.c" "crash_journal_store.c"
              "mqtt_topics.c")
if(CONFIG_ESPNOW_ROLE_RECEIVER)
    list(APPEND main_srcs "mqtt_bridge.c")
endif()
if(CONFIG_ESPNOW_ROLE_SENDER)
    list(APPEND main_srcs "tx_batch.c")
endif()

idf_component_register(SRCS ${main_srcs}
                    INCLUDE_DIRS "."
                    REQUIRES ${main_requires})

# Generate a valid, empty SPIFFS image for the power-history "storage"
# partition and include it in the flash targets, so a freshly flashed device
# already has a mountable partition and needs no first-boot formatting.
spiffs_create_partition_image(storage ${CMAKE_CURRENT_SOURCE_DIR}/spiffs_root FLASH_IN_PROJECT)
