set(main_requires
    app_update
    esp_partition
    esp_driver_gpio
    esp_driver_uart
    esp_event
    esp_http_client
    esp_https_ota
    esp-tls
    mbedtls
    esp_http_server
    esp_netif
    esp_pm
    esp_timer
    esp_wifi
    mdns
    nvs_flash
    spiffs
    sml_transport
    sml_parser
)

# ESP-IDF v6: ESP-MQTT and cJSON are managed components. They are declared in
# idf_component.yml and become dependencies of "main" automatically, so they do
# not need to be named in REQUIRES (see the IDF v6 migration guide for cJSON).
set(main_srcs "main.c" "power_history.c" "web_console.c" "serial_console.c"
              "runtime_config.c"
              "boot_policy.c" "boot_confirm.c" "recovery_button.c"
              "recovery_start.c" "boot_log.c"
              "crash_journal.c" "crash_journal_store.c"
              "mqtt_topics.c" "history_backfill.c" "history_sync.c"
              "history_csv_import.c" "history_upload.c"
              "link_fallback.c" "liveness_guard.c" "ota_policy.c"
              "task_wdt_policy.c" "task_wdt_guard.c"
              "json_escape.c" "version.c")
if(CONFIG_ESPNOW_ROLE_RECEIVER)
    list(APPEND main_srcs "mqtt_bridge.c" "web_guard.c"
                          "health_policy.c" "health_task.c")
endif()
if(CONFIG_ESPNOW_ROLE_SENDER)
    list(APPEND main_srcs "tx_batch.c" "uart_sleep_window.c")
endif()

idf_component_register(SRCS ${main_srcs}
                    INCLUDE_DIRS "."
                    REQUIRES ${main_requires})

# Generate a valid, empty SPIFFS image for the power-history "storage"
# partition so it can be flashed explicitly if ever needed. It is deliberately
# NOT added to FLASH_IN_PROJECT: a plain `idf.py flash` (or any full flash)
# must never overwrite the partition that holds the measurement history. The
# device formats a blank partition on first boot (format_if_mount_failed), so
# nothing is required here for a fresh board. To (re)create it on purpose, use
# `idf.py storage-flash` after checking that the stored minutes are backed up.
spiffs_create_partition_image(storage ${CMAKE_CURRENT_SOURCE_DIR}/spiffs_root)
