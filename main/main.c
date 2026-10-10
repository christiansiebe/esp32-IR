#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_now.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "power_history.h"
#include "sml_transport.h"
#if CONFIG_ESPNOW_ROLE_SENDER
#include "sml_adapter.h"
#endif

static const char *TAG = "espnow_meter";

#define ESPNOW_CHANNEL       1
#define UART_PORT_NUM        UART_NUM_1
#define UART_RX_GPIO         16
#define UART_BAUD_RATE       9600
#define UART_BUFFER_SIZE     8192
#define UART_PACKET_BYTES    64
#define UART_EVENT_QUEUE_LENGTH 32
#define SML_TX_QUEUE_LENGTH 16
#define METER_MAGIC          0x48425A31u
#define METER_VERSION        1u
#define SML_VALUE_HISTORY_COUNT 64
#define SML_API_PAGE_SIZE    16
#define SML_POWER_API_PAGE_SIZE 128
#define RSSI_HISTORY_COUNT   256
#define RSSI_API_PAGE_SIZE   32
#define SML_POWER_AVERAGE_WINDOW_US 900000000ULL
#define SML_POWER_SAMPLE_COUNT 900
#define POWER_HISTORY_INTERVAL_SECONDS 60LL
#define POWER_SAMPLE_STALE_US 30000000ULL
#define ESPNOW_OTA_MAGIC     0x4F544131u
#define ESPNOW_OTA_VERSION   1u
#define ESPNOW_OTA_BEGIN     1u
#define ESPNOW_OTA_DATA      2u
#define ESPNOW_OTA_END       3u
#define ESPNOW_OTA_ABORT     4u
#define ESPNOW_OTA_ACK       0x80u
#define ESPNOW_OTA_CHUNK     220u
#define ESPNOW_OTA_TIMEOUT_MS 3000
#define ESPNOW_DIAG_MAGIC     0x44494147u
#define ESPNOW_DIAG_VERSION   1u
#define ESPNOW_DIAG_TELEMETRY 1u
#define ESPNOW_DIAG_REBOOT    2u
#define ESPNOW_DIAG_REBOOT_ACK 3u
#define WIFI_CONFIG_NAMESPACE "metercfg"
#define WIFI_SSID_KEY          "wifi_ssid"
#define WIFI_PASSWORD_KEY      "wifi_pass"
#define WIFI_SSID_MAX_LEN      32
#define WIFI_PASSWORD_MAX_LEN  64

#define METER_HTML_HEADER "<!doctype html><html><head><meta charset=\"utf-8\"><title>ESP32 IR Meter</title><style>body{font-family:Arial,sans-serif;margin:24px;background:#10151f;color:#e5edf7} .card{background:#182330;padding:20px;border-radius:12px;max-width:100%} .value{font-size:2rem;font-weight:700;color:#78d6ff} .small{font-size:0.8rem;color:#9fb4c8}.nav{display:flex;flex-wrap:wrap;gap:10px;margin-top:18px}.nav a,.action{display:inline-block;padding:10px 14px;border:0;border-radius:7px;background:#176b91;color:#fff;text-decoration:none;font:inherit;cursor:pointer}.nav a:hover,.action:hover{background:#2086b2}</style></head><body><div class=\"card\"><h1>ESP32 IR Meter</h1>"

#define METER_HTML_FOOTER "</div></body></html>"

typedef struct {
    uint32_t sequence;
    uint64_t timestamp_us;
    char obis[24];
    char value[24];
    int8_t scaler;
    uint8_t unit;
} sml_value_record_t;

typedef struct {
    uint64_t timestamp_us;
    double watts;
} sml_power_sample_t;

typedef struct {
    uint32_t sequence;
    uint64_t timestamp_us;
    int8_t rssi_dbm;
} rssi_record_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint32_t transfer_id;
    uint32_t sequence;
    uint32_t offset;
    uint32_t total_size;
    uint16_t payload_length;
    uint16_t status;
} espnow_ota_packet_t;

typedef struct {
    uint8_t source[ESP_NOW_ETH_ALEN];
    uint16_t length;
    uint8_t data[ESP_NOW_MAX_DATA_LEN];
} espnow_ota_rx_item_t;

typedef struct {
    uint8_t source[ESP_NOW_ETH_ALEN];
    espnow_ota_packet_t packet;
} espnow_ota_ack_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t reserved;
    uint32_t sequence;
    uint32_t uptime_seconds;
    uint32_t reset_reason;
    uint32_t free_heap;
    uint32_t minimum_free_heap;
    uint64_t uart_rx_bytes;
    uint32_t tx_queue_drops;
    uint32_t uart_fifo_overflows;
    uint32_t uart_rx_buffer_full;
    uint32_t uart_frame_errors;
    uint32_t uart_parity_errors;
} espnow_diagnostic_packet_t;

static const uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

static char g_wifi_ssid[WIFI_SSID_MAX_LEN + 1];
static char g_wifi_password[WIFI_PASSWORD_MAX_LEN + 1];
static bool g_wifi_configured;
static bool g_wifi_connected;
static bool g_sntp_started;
static esp_netif_t *g_ap_netif;
static esp_netif_t *g_sta_netif;
static uint8_t g_espnow_channel = ESPNOW_CHANNEL;
#if CONFIG_ESPNOW_ROLE_SENDER
typedef struct {
    uint64_t timestamp_us;
    bool crc_valid;
    uint8_t value_count;
    sml_transport_value_t values[SML_TRANSPORT_MAX_VALUES];
} sml_tx_item_t;

static esp_now_send_status_t g_last_send_status;
static QueueHandle_t g_espnow_ota_rx_queue;
static QueueHandle_t g_uart_event_queue;
static QueueHandle_t g_sml_tx_queue;
static uint64_t g_uart_rx_bytes;
static uint32_t g_sml_tx_queue_drops;
static uint32_t g_uart_fifo_overflows;
static uint32_t g_uart_rx_buffer_full;
static uint32_t g_uart_frame_errors;
static uint32_t g_uart_parity_errors;
static portMUX_TYPE g_uart_stats_lock = portMUX_INITIALIZER_UNLOCKED;
#else
static QueueHandle_t g_espnow_ota_ack_queue;
static SemaphoreHandle_t g_espnow_ota_mutex;
static uint8_t g_espnow_ota_sender_mac[ESP_NOW_ETH_ALEN];
static espnow_diagnostic_packet_t g_sender_diagnostics;
static int64_t g_sender_diagnostics_received_us;
static uint32_t g_sender_reboot_ack_sequence;
static uint32_t g_sender_reboot_requested_sequence;
static bool g_sender_diagnostics_seen;
static portMUX_TYPE g_sender_diagnostics_lock = portMUX_INITIALIZER_UNLOCKED;
static sml_value_record_t g_sml_value_history[SML_VALUE_HISTORY_COUNT];
static size_t g_sml_value_history_count;
static size_t g_sml_value_history_next;
static uint32_t g_sml_latest_value_sequence;
static uint32_t g_sml_valid_telegrams;
static uint32_t g_sml_invalid_crc_telegrams;
static uint32_t g_sml_transport_gaps;
static uint32_t g_sml_last_transport_sequence;
static sml_value_record_t g_sml_latest_values[3];
static bool g_sml_latest_value_valid[3];
static sml_power_sample_t g_sml_power_samples[SML_POWER_SAMPLE_COUNT];
static size_t g_sml_power_sample_count;
static size_t g_sml_power_sample_next;
static double g_sml_power_sample_sum_w;
static double g_sml_latest_power_w;
static uint64_t g_sml_latest_power_received_us;
static bool g_sml_latest_power_valid;
static esp_timer_handle_t g_power_sample_timer;
static int64_t g_power_bucket_start_utc;
static int64_t g_power_bucket_first_sample_utc;
static int64_t g_power_bucket_last_sample_utc;
static double g_power_bucket_sum_w;
static uint32_t g_power_bucket_sample_count;
static portMUX_TYPE g_sml_history_lock = portMUX_INITIALIZER_UNLOCKED;
static rssi_record_t g_rssi_history[RSSI_HISTORY_COUNT];
static size_t g_rssi_history_count;
static size_t g_rssi_history_next;
static uint32_t g_rssi_latest_sequence;
static bool g_rssi_capture_active;
static portMUX_TYPE g_rssi_history_lock = portMUX_INITIALIZER_UNLOCKED;
static void store_sml_value(uint64_t timestamp_us, const uint8_t *obis,
                            const char *value, int8_t scaler, uint8_t unit);
#if !CONFIG_ESPNOW_ROLE_SENDER
static void power_sample_timer_callback(void *arg);
#endif
#endif

static void diagnostic_receive_callback(const esp_now_recv_info_t *info,
                                        const uint8_t *data, int data_len);
#if !CONFIG_ESPNOW_ROLE_SENDER
static esp_err_t sender_diagnostics_api_handler(httpd_req_t *req);
static esp_err_t sender_reboot_handler(httpd_req_t *req);
static esp_err_t diagnostics_page_handler(httpd_req_t *req);
static esp_err_t power_samples_api_handler(httpd_req_t *req);
#endif

static esp_err_t load_wifi_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_len = sizeof(g_wifi_ssid);
    size_t password_len = sizeof(g_wifi_password);
    err = nvs_get_str(handle, WIFI_SSID_KEY, g_wifi_ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, WIFI_PASSWORD_KEY, g_wifi_password, &password_len);
    }
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        g_wifi_ssid[0] = '\0';
        g_wifi_password[0] = '\0';
        return ESP_OK;
    }
    if (err == ESP_OK) {
        g_wifi_configured = g_wifi_ssid[0] != '\0';
    }
    return err;
}

static esp_err_t save_wifi_credentials(const char *ssid, const char *password)
{
    if (strcmp(ssid, g_wifi_ssid) == 0 && strcmp(password, g_wifi_password) == 0) {
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, WIFI_SSID_KEY, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_PASSWORD_KEY, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        strlcpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid));
        strlcpy(g_wifi_password, password, sizeof(g_wifi_password));
        g_wifi_configured = g_wifi_ssid[0] != '\0';
    }
    return err;
}

static void fill_station_config(wifi_config_t *config)
{
    memset(config, 0, sizeof(*config));
    strlcpy((char *)config->sta.ssid, g_wifi_ssid, sizeof(config->sta.ssid));
    strlcpy((char *)config->sta.password, g_wifi_password, sizeof(config->sta.password));
}

static void time_sync_notification(struct timeval *tv)
{
    (void)tv;
    ESP_LOGI(TAG, "System clock synchronized; minute history enabled");
}

static bool system_time_is_valid(void)
{
    return time(NULL) >= 1704067200;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *event = event_data;
        g_wifi_connected = true;
        g_espnow_channel = event->channel;
        ESP_LOGI(TAG, "Connected to configured Wi-Fi on channel %u", event->channel);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;
        g_wifi_connected = false;
        ESP_LOGW(TAG, "Wi-Fi station disconnected (reason %u)", event->reason);
        if (g_wifi_configured) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Wi-Fi reconnect failed: %s", esp_err_to_name(err));
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG, "FRITZ!Box network address: " IPSTR, IP2STR(&event->ip_info.ip));
        if (!g_sntp_started) {
            esp_sntp_config_t sntp_config =
                ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            sntp_config.wait_for_sync = false;
            sntp_config.sync_cb = time_sync_notification;
            esp_err_t err = esp_netif_sntp_init(&sntp_config);
            if (err == ESP_OK) {
                g_sntp_started = true;
                ESP_LOGI(TAG, "SNTP time synchronization started");
            } else {
                ESP_LOGE(TAG, "Could not start SNTP: %s", esp_err_to_name(err));
            }
        }
    }
}

#if CONFIG_ESPNOW_ROLE_SENDER
static uint8_t scan_receiver_ap_channel(void)
{
    wifi_scan_config_t scan_config = {
        .ssid = (uint8_t *)"ESP32-IR-Meter",
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .show_hidden = false,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Receiver AP scan failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t count = 16;
    wifi_ap_record_t records[16];
    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not read receiver AP scan results: %s", esp_err_to_name(err));
        return 0;
    }

    for (uint16_t i = 0; i < count; ++i) {
        if (strcmp((const char *)records[i].ssid, "ESP32-IR-Meter") == 0) {
            return records[i].primary;
        }
    }
    return 0;
}
#endif

static void initialize_wifi_and_espnow(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

#if CONFIG_ESPNOW_ROLE_SENDER
    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "Could not create default station netif");
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#else
    g_ap_netif = esp_netif_create_default_wifi_ap();
    if (g_ap_netif == NULL) {
        ESP_LOGE(TAG, "Could not create default AP netif");
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    g_sta_netif = esp_netif_create_default_wifi_sta();
    if (g_sta_netif == NULL) {
        ESP_LOGE(TAG, "Could not create default station netif");
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(load_wifi_credentials());
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t ap_config = {0};
    memcpy(ap_config.ap.ssid, "ESP32-IR-Meter", sizeof("ESP32-IR-Meter"));
    ap_config.ap.ssid_len = strlen("ESP32-IR-Meter");
    ap_config.ap.channel = ESPNOW_CHANNEL;
    memcpy(ap_config.ap.password, "12345678", sizeof("12345678"));
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.ssid_hidden = 0;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    wifi_config_t sta_config;
    fill_station_config(&sta_config);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
#endif

    ESP_ERROR_CHECK(esp_wifi_start());

#if CONFIG_ESPNOW_ROLE_SENDER
    uint8_t detected_channel = 0;
    for (unsigned int attempt = 0; attempt < 30 && detected_channel == 0; ++attempt) {
        detected_channel = scan_receiver_ap_channel();
        if (detected_channel == 0) {
            ESP_LOGI(TAG, "Waiting for receiver AP on channel %d", ESPNOW_CHANNEL);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    if (detected_channel != 0) {
        g_espnow_channel = detected_channel;
        ESP_LOGI(TAG, "Using receiver AP channel %u for ESP-NOW", g_espnow_channel);
    } else {
        ESP_LOGE(TAG, "Receiver AP not found; using fallback ESP-NOW channel %u",
                 g_espnow_channel);
    }
    ESP_ERROR_CHECK(esp_wifi_set_channel(g_espnow_channel, WIFI_SECOND_CHAN_NONE));
    wifi_interface_t espnow_interface = WIFI_IF_STA;
#else
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_AP,
                                          WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                                          WIFI_PROTOCOL_11N));
    if (g_wifi_configured) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else {
        ESP_LOGI(TAG, "No FRITZ!Box credentials saved; use the local AP configuration page");
    }
    wifi_interface_t espnow_interface = WIFI_IF_AP;
#endif
    ESP_ERROR_CHECK(esp_now_init());

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, broadcast_mac, sizeof(broadcast_mac));
#if CONFIG_ESPNOW_ROLE_SENDER
    peer.channel = g_espnow_channel;
#else
    peer.channel = 0;
#endif
    peer.ifidx = espnow_interface;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

#if !CONFIG_ESPNOW_ROLE_SENDER
    ESP_LOGI(TAG, "AP started: SSID=ESP32-IR-Meter, channel=%u, IP=192.168.4.1",
             g_espnow_channel);
#endif
}

static esp_err_t add_espnow_peer(const uint8_t *mac, wifi_interface_t interface)
{
    if (esp_now_is_peer_exist(mac)) {
        return ESP_OK;
    }
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
    peer.channel = 0;
    peer.ifidx = interface;
    peer.encrypt = false;
    return esp_now_add_peer(&peer);
}

#if CONFIG_ESPNOW_ROLE_SENDER
static void send_sender_ota_ack(const uint8_t *destination,
                                const espnow_ota_packet_t *request,
                                esp_err_t status)
{
    esp_err_t err = add_espnow_peer(destination, WIFI_IF_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not add OTA acknowledgement peer: %s", esp_err_to_name(err));
        return;
    }

    espnow_ota_packet_t ack = *request;
    ack.type = ESPNOW_OTA_ACK;
    ack.status = (uint16_t)status;
    ack.payload_length = 0;
    err = esp_now_send(destination, (const uint8_t *)&ack, sizeof(ack));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not send OTA acknowledgement: %s", esp_err_to_name(err));
    }
}

static void espnow_ota_worker(void *arg)
{
    (void)arg;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = NULL;
    uint32_t active_transfer = 0;
    uint32_t expected_offset = 0;
    uint32_t expected_sequence = 0;
    uint32_t expected_size = 0;
    bool ota_active = false;

    while (1) {
        espnow_ota_rx_item_t item;
        if (xQueueReceive(g_espnow_ota_rx_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        espnow_ota_packet_t request;
        memcpy(&request, item.data, sizeof(request));
        size_t payload_available = item.length - sizeof(request);
        esp_err_t status = ESP_OK;

        if (request.payload_length != payload_available) {
            status = ESP_ERR_INVALID_SIZE;
        } else if (request.type == ESPNOW_OTA_BEGIN) {
            if (request.payload_length != 0 || request.total_size == 0) {
                status = ESP_ERR_INVALID_ARG;
            } else if (ota_active && request.transfer_id == active_transfer &&
                       request.total_size == expected_size) {
                status = ESP_OK;
            } else if (ota_active) {
                status = ESP_ERR_INVALID_STATE;
            } else {
                update_partition = esp_ota_get_next_update_partition(NULL);
                if (update_partition == NULL || request.total_size > update_partition->size) {
                    status = ESP_ERR_INVALID_SIZE;
                } else {
                    status = esp_ota_begin(update_partition, request.total_size, &ota_handle);
                    if (status == ESP_OK) {
                        active_transfer = request.transfer_id;
                        expected_offset = 0;
                        expected_sequence = 0;
                        expected_size = request.total_size;
                        ota_active = true;
                        ESP_LOGI(TAG, "ESP-NOW OTA started: %lu bytes",
                                 (unsigned long)expected_size);
                    }
                }
            }
        } else if (request.type == ESPNOW_OTA_DATA) {
            if (!ota_active || request.transfer_id != active_transfer) {
                status = ESP_ERR_INVALID_STATE;
            } else if (request.total_size != expected_size ||
                       request.offset != expected_offset ||
                       request.sequence != expected_sequence ||
                       payload_available == 0 ||
                       payload_available > ESPNOW_OTA_CHUNK ||
                       payload_available > expected_size - expected_offset) {
                if (ota_active && request.transfer_id == active_transfer &&
                    request.total_size == expected_size &&
                    request.offset + payload_available == expected_offset &&
                    request.sequence + 1 == expected_sequence &&
                    payload_available <= ESPNOW_OTA_CHUNK) {
                    status = ESP_OK;
                } else {
                    status = ESP_ERR_INVALID_SIZE;
                }
            } else {
                status = esp_ota_write(ota_handle, item.data + sizeof(request),
                                       payload_available);
                if (status == ESP_OK) {
                    expected_offset += (uint32_t)payload_available;
                    ++expected_sequence;
                }
            }
        } else if (request.type == ESPNOW_OTA_END) {
            if (!ota_active || request.transfer_id != active_transfer ||
                request.payload_length != 0 ||
                expected_offset != expected_size) {
                status = ESP_ERR_INVALID_STATE;
            } else {
                status = esp_ota_end(ota_handle);
                ota_active = false;
                if (status == ESP_OK) {
                    status = esp_ota_set_boot_partition(update_partition);
                }
                if (status == ESP_OK) {
                    ESP_LOGI(TAG, "ESP-NOW OTA complete; rebooting into %s",
                             update_partition->label);
                } else {
                    ESP_LOGE(TAG, "ESP-NOW OTA validation/activation failed: %s",
                             esp_err_to_name(status));
                }
            }
        } else if (request.type == ESPNOW_OTA_ABORT) {
            if (ota_active && request.transfer_id == active_transfer) {
                esp_ota_abort(ota_handle);
                ota_active = false;
            }
        } else {
            status = ESP_ERR_NOT_SUPPORTED;
        }

        send_sender_ota_ack(item.source, &request, status);
        if (request.type == ESPNOW_OTA_END && status == ESP_OK) {
            int64_t restart_deadline = esp_timer_get_time() + 10000000;
            while (esp_timer_get_time() < restart_deadline) {
                int64_t remaining_us = restart_deadline - esp_timer_get_time();
                espnow_ota_rx_item_t retry_item;
                TickType_t wait_ticks = pdMS_TO_TICKS((remaining_us + 999) / 1000);
                if (xQueueReceive(g_espnow_ota_rx_queue, &retry_item, wait_ticks) != pdTRUE) {
                    break;
                }
                espnow_ota_packet_t retry;
                memcpy(&retry, retry_item.data, sizeof(retry));
                if (retry.magic == ESPNOW_OTA_MAGIC &&
                    retry.version == ESPNOW_OTA_VERSION &&
                    retry.type == ESPNOW_OTA_END &&
                    retry.transfer_id == request.transfer_id) {
                    send_sender_ota_ack(retry_item.source, &retry, ESP_OK);
                }
            }
            esp_restart();
        }
        if (status != ESP_OK && request.type != ESPNOW_OTA_BEGIN) {
            ESP_LOGW(TAG, "ESP-NOW OTA packet rejected: type=%u seq=%lu err=%s",
                     request.type, (unsigned long)request.sequence, esp_err_to_name(status));
        }
    }
}
#endif

#if CONFIG_ESPNOW_ROLE_SENDER
static void send_callback(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "ESP-NOW reports that the broadcast send failed");
    }
    if (tx_info == NULL || tx_info->des_addr == NULL ||
        memcmp(tx_info->des_addr, broadcast_mac, ESP_NOW_ETH_ALEN) != 0) {
        return;
    }
    g_last_send_status = status;
}
#endif

static bool is_espnow_ota_packet(const uint8_t *data, int data_len)
{
    if (data == NULL || data_len < (int)sizeof(espnow_ota_packet_t)) {
        return false;
    }
    espnow_ota_packet_t packet;
    memcpy(&packet, data, sizeof(packet));
    return packet.magic == ESPNOW_OTA_MAGIC && packet.version == ESPNOW_OTA_VERSION;
}

#if !CONFIG_ESPNOW_ROLE_SENDER
static void record_rssi_sample(int8_t rssi_dbm)
{
    portENTER_CRITICAL(&g_rssi_history_lock);
    bool capture_active = g_rssi_capture_active;
    portEXIT_CRITICAL(&g_rssi_history_lock);
    if (!capture_active) {
        return;
    }

    uint64_t timestamp_us = (uint64_t)esp_timer_get_time();
    portENTER_CRITICAL(&g_rssi_history_lock);
    if (g_rssi_capture_active) {
        rssi_record_t *record = &g_rssi_history[g_rssi_history_next];
        record->sequence = ++g_rssi_latest_sequence;
        record->timestamp_us = timestamp_us;
        record->rssi_dbm = rssi_dbm;
        g_rssi_history_next = (g_rssi_history_next + 1) % RSSI_HISTORY_COUNT;
        if (g_rssi_history_count < RSSI_HISTORY_COUNT) {
            ++g_rssi_history_count;
        }
    }
    portEXIT_CRITICAL(&g_rssi_history_lock);
}
#endif

static void receive_callback(const esp_now_recv_info_t *info, const uint8_t *data, int data_len)
{
    if (info == NULL || info->src_addr == NULL || data == NULL || data_len <= 0) {
        ESP_LOGW(TAG, "Received an invalid ESP-NOW packet");
        return;
    }

#if CONFIG_ESPNOW_ROLE_SENDER
    if (is_espnow_ota_packet(data, data_len)) {
        if (g_espnow_ota_rx_queue == NULL || data_len > ESP_NOW_MAX_DATA_LEN) {
            ESP_LOGE(TAG, "Sender OTA receiver is unavailable or packet is oversized");
            return;
        }
        espnow_ota_rx_item_t item = {
            .length = (uint16_t)data_len,
        };
        memcpy(item.source, info->src_addr, ESP_NOW_ETH_ALEN);
        memcpy(item.data, data, (size_t)data_len);
        if (xQueueSend(g_espnow_ota_rx_queue, &item, 0) != pdTRUE) {
            ESP_LOGE(TAG, "Sender OTA receive queue full; packet rejected");
        }
        return;
    }
#else
    if (is_espnow_ota_packet(data, data_len)) {
        espnow_ota_packet_t packet;
        memcpy(&packet, data, sizeof(packet));
        if (packet.type == ESPNOW_OTA_ACK && g_espnow_ota_ack_queue != NULL &&
            packet.payload_length == 0) {
            espnow_ota_ack_t ack = {
                .packet = packet,
            };
            memcpy(ack.source, info->src_addr, ESP_NOW_ETH_ALEN);
            if (xQueueSend(g_espnow_ota_ack_queue, &ack, 0) != pdTRUE) {
                ESP_LOGW(TAG, "Sender OTA acknowledgement queue full");
            }
        }
        return;
    }
#endif

#if !CONFIG_ESPNOW_ROLE_SENDER
    if (data_len < (int)sizeof(sml_transport_header_t)) {
        ESP_LOGW(TAG, "Dropped undersized SML transport packet (%d bytes)", data_len);
        return;
    }

    sml_transport_packet_t packet = {0};
    memcpy(&packet.header, data, sizeof(packet.header));
    size_t expected_length = sizeof(packet.header) +
                             packet.header.value_count * sizeof(packet.values[0]);
    if (packet.header.magic != SML_TRANSPORT_MAGIC ||
        packet.header.version != SML_TRANSPORT_VERSION ||
        packet.header.crc_valid > 1 ||
        packet.header.value_count > SML_TRANSPORT_MAX_VALUES ||
        (size_t)data_len != expected_length ||
        (!packet.header.crc_valid && packet.header.value_count != 0)) {
        ESP_LOGW(TAG, "Dropped malformed SML transport packet");
        return;
    }

    if (packet.header.sequence == 0) {
        ESP_LOGW(TAG, "Dropped SML transport packet with sequence zero");
        return;
    }
    if (info->rx_ctrl != NULL) {
        record_rssi_sample(info->rx_ctrl->rssi);
    }
    if (g_sml_last_transport_sequence != 0 &&
        packet.header.sequence != g_sml_last_transport_sequence + 1) {
        portENTER_CRITICAL(&g_sml_history_lock);
        ++g_sml_transport_gaps;
        g_sml_power_sample_count = 0;
        g_sml_power_sample_next = 0;
        g_sml_power_sample_sum_w = 0.0;
        g_sml_latest_power_valid = false;
        portEXIT_CRITICAL(&g_sml_history_lock);
    }
    g_sml_last_transport_sequence = packet.header.sequence;
    memcpy(packet.values, data + sizeof(packet.header),
           packet.header.value_count * sizeof(packet.values[0]));

    portENTER_CRITICAL(&g_sml_history_lock);
    if (packet.header.crc_valid) {
        ++g_sml_valid_telegrams;
    } else {
        ++g_sml_invalid_crc_telegrams;
    }
    portEXIT_CRITICAL(&g_sml_history_lock);

    if (packet.header.crc_valid) {
        for (uint8_t i = 0; i < packet.header.value_count; ++i) {
            char value_text[24];
            int written = snprintf(value_text, sizeof(value_text), "%lld",
                                   (long long)packet.values[i].value);
            if (written < 0 || (size_t)written >= sizeof(value_text)) {
                ESP_LOGE(TAG, "Could not format received SML value");
                continue;
            }
            store_sml_value(packet.header.timestamp_us, packet.values[i].obis,
                            value_text, packet.values[i].scaler,
                            packet.values[i].unit);
        }
    }
    memcpy(g_espnow_ota_sender_mac, info->src_addr, ESP_NOW_ETH_ALEN);
    return;
#else
    ESP_LOGW(TAG, "Dropped non-OTA ESP-NOW packet at sender");
#endif
}

#if !CONFIG_ESPNOW_ROLE_SENDER
static void append_power_sample_locked(uint64_t timestamp_us, double watts)
{
    while (g_sml_power_sample_count > 0) {
        size_t oldest_index =
            (g_sml_power_sample_next + SML_POWER_SAMPLE_COUNT -
             g_sml_power_sample_count) % SML_POWER_SAMPLE_COUNT;
        if (timestamp_us - g_sml_power_samples[oldest_index].timestamp_us <=
            SML_POWER_AVERAGE_WINDOW_US) {
            break;
        }
        g_sml_power_sample_sum_w -= g_sml_power_samples[oldest_index].watts;
        --g_sml_power_sample_count;
    }
    if (g_sml_power_sample_count == SML_POWER_SAMPLE_COUNT) {
        g_sml_power_sample_sum_w -=
            g_sml_power_samples[g_sml_power_sample_next].watts;
    } else {
        ++g_sml_power_sample_count;
    }
    g_sml_power_samples[g_sml_power_sample_next] = (sml_power_sample_t) {
        .timestamp_us = timestamp_us,
        .watts = watts,
    };
    g_sml_power_sample_sum_w += watts;
    g_sml_power_sample_next =
        (g_sml_power_sample_next + 1) % SML_POWER_SAMPLE_COUNT;
}

static void power_sample_timer_callback(void *arg)
{
    (void)arg;
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const time_t wall_clock = time(NULL);
    power_history_record_t completed_interval = {0};
    bool enqueue_completed_interval = false;

    portENTER_CRITICAL(&g_sml_history_lock);
    if (g_sml_latest_power_valid &&
        now_us - g_sml_latest_power_received_us <= POWER_SAMPLE_STALE_US) {
        append_power_sample_locked(now_us, g_sml_latest_power_w);
        if (wall_clock >= 1704067200) {
            int64_t bucket_start = ((int64_t)wall_clock /
                                    POWER_HISTORY_INTERVAL_SECONDS) *
                                   POWER_HISTORY_INTERVAL_SECONDS;
            if (g_power_bucket_start_utc != 0 &&
                bucket_start > g_power_bucket_start_utc &&
                g_power_bucket_sample_count != 0) {
                double average_mw = 1000.0 * g_power_bucket_sum_w /
                                    (double)g_power_bucket_sample_count;
                if (average_mw > INT32_MAX) {
                    average_mw = INT32_MAX;
                } else if (average_mw < INT32_MIN) {
                    average_mw = INT32_MIN;
                }
                completed_interval = (power_history_record_t) {
                    .interval_start_utc = g_power_bucket_start_utc,
                    .average_milliwatts = (int32_t)average_mw,
                    .sample_count = g_power_bucket_sample_count,
                };
                enqueue_completed_interval = true;
            }
            if (g_power_bucket_start_utc == 0 ||
                bucket_start > g_power_bucket_start_utc) {
                g_power_bucket_start_utc = bucket_start;
                g_power_bucket_first_sample_utc = 0;
                g_power_bucket_last_sample_utc = 0;
                g_power_bucket_sum_w = 0.0;
                g_power_bucket_sample_count = 0;
            }
            if (bucket_start == g_power_bucket_start_utc &&
                g_power_bucket_sample_count != UINT32_MAX) {
                if (g_power_bucket_sample_count == 0) {
                    g_power_bucket_first_sample_utc = (int64_t)wall_clock;
                }
                g_power_bucket_last_sample_utc = (int64_t)wall_clock;
                g_power_bucket_sum_w += g_sml_latest_power_w;
                ++g_power_bucket_sample_count;
            }
        }
    }
    portEXIT_CRITICAL(&g_sml_history_lock);

    if (enqueue_completed_interval) {
        (void)power_history_enqueue(&completed_interval);
    }
}

static void store_sml_value(uint64_t timestamp_us, const uint8_t *obis,
                            const char *value, int8_t scaler, uint8_t unit)
{
    sml_value_record_t record = {
        .timestamp_us = timestamp_us,
        .scaler = scaler,
        .unit = unit,
    };
    snprintf(record.obis, sizeof(record.obis), "%u.%u.%u.%u.%u.%u",
             obis[0], obis[1], obis[2], obis[3], obis[4], obis[5]);
    strlcpy(record.value, value, sizeof(record.value));

    int latest_index = -1;
    if (strcmp(record.obis, "1.0.1.8.0.255") == 0) {
        latest_index = 0;
    } else if (strcmp(record.obis, "1.0.2.8.0.255") == 0) {
        latest_index = 1;
    } else if (strcmp(record.obis, "1.0.16.7.0.255") == 0) {
        latest_index = 2;
    }
    double watts = strtod(value, NULL);
    if (latest_index == 2 && unit == 27) {
        for (int exponent = 0; exponent < scaler; ++exponent) {
            watts *= 10.0;
        }
        for (int exponent = 0; exponent > scaler; --exponent) {
            watts /= 10.0;
        }
    }

    portENTER_CRITICAL(&g_sml_history_lock);
    record.sequence = ++g_sml_latest_value_sequence;
    g_sml_value_history[g_sml_value_history_next] = record;
    g_sml_value_history_next =
        (g_sml_value_history_next + 1) % SML_VALUE_HISTORY_COUNT;
    if (g_sml_value_history_count < SML_VALUE_HISTORY_COUNT) {
        ++g_sml_value_history_count;
    }

    if (latest_index >= 0) {
        g_sml_latest_values[latest_index] = record;
        g_sml_latest_value_valid[latest_index] = true;
    }
    if (latest_index == 2 && unit == 27 && isfinite(watts)) {
        g_sml_latest_power_w = watts;
        g_sml_latest_power_received_us = (uint64_t)esp_timer_get_time();
        g_sml_latest_power_valid = true;
    }
    portEXIT_CRITICAL(&g_sml_history_lock);

    ESP_LOGI(TAG, "SML value: OBIS=%s value=%s scaler=%d unit=%u",
             record.obis, record.value, (int)record.scaler,
             (unsigned int)record.unit);
}

#endif

#if CONFIG_ESPNOW_ROLE_SENDER
void sml_adapter_telegram_callback(uint64_t timestamp_us, bool crc_valid,
                                  const sml_transport_value_t *values,
                                  uint8_t value_count)
{
    if (value_count > SML_TRANSPORT_MAX_VALUES ||
        (value_count != 0 && values == NULL) || (!crc_valid && value_count != 0)) {
        ESP_LOGE(TAG, "Parser produced an invalid SML telegram callback");
        return;
    }

    sml_tx_item_t item = {
        .timestamp_us = timestamp_us,
        .crc_valid = crc_valid,
        .value_count = value_count,
    };
    if (value_count != 0) {
        memcpy(item.values, values, value_count * sizeof(item.values[0]));
    }
    if (g_sml_tx_queue == NULL || xQueueSend(g_sml_tx_queue, &item, 0) != pdTRUE) {
        portENTER_CRITICAL(&g_uart_stats_lock);
        ++g_sml_tx_queue_drops;
        portEXIT_CRITICAL(&g_uart_stats_lock);
        ESP_LOGE(TAG, "SML transport queue full; validated telegram was not queued");
    }
}

static void send_sml_telegram(uint32_t sequence, const sml_tx_item_t *item)
{
    sml_transport_packet_t packet = {
        .header = {
            .magic = SML_TRANSPORT_MAGIC,
            .version = SML_TRANSPORT_VERSION,
            .crc_valid = item->crc_valid ? 1 : 0,
            .sequence = sequence,
            .timestamp_us = item->timestamp_us,
            .value_count = item->value_count,
        },
    };
    if (item->value_count != 0) {
        memcpy(packet.values, item->values,
               item->value_count * sizeof(packet.values[0]));
    }

    size_t packet_length = sizeof(packet.header) +
                           item->value_count * sizeof(packet.values[0]);
    esp_err_t err = esp_now_send(broadcast_mac, (const uint8_t *)&packet,
                                 packet_length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW SML telegram send failed: %s", esp_err_to_name(err));
        return;
    }
    g_last_send_status = ESP_NOW_SEND_SUCCESS;
    ESP_LOGD(TAG, "Sent SML telegram sequence=%lu crc_valid=%u values=%u",
             (unsigned long)sequence, packet.header.crc_valid,
             (unsigned int)packet.header.value_count);
}

static void uart_capture_worker(void *arg)
{
    (void)arg;
    uint8_t rx_buffer[UART_PACKET_BYTES];

    while (1) {
        int received = uart_read_bytes(UART_PORT_NUM, rx_buffer, sizeof(rx_buffer),
                                       pdMS_TO_TICKS(50));
        if (received <= 0) {
            continue;
        }

        uint64_t timestamp_us = (uint64_t)esp_timer_get_time();

        portENTER_CRITICAL(&g_uart_stats_lock);
        g_uart_rx_bytes += (uint64_t)received;
        portEXIT_CRITICAL(&g_uart_stats_lock);

        for (int i = 0; i < received; ++i) {
            (void)sml_adapter_feed_byte(rx_buffer[i], timestamp_us);
        }
    }
}

static void uart_transport_worker(void *arg)
{
    (void)arg;
    uint32_t sequence = 0;
    sml_tx_item_t item;

    while (1) {
        if (xQueueReceive(g_sml_tx_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        send_sml_telegram(++sequence, &item);
    }
}

static void uart_event_worker(void *arg)
{
    (void)arg;
    uart_event_t event;
    int64_t next_stats_us = esp_timer_get_time() + 10000000LL;

    while (1) {
        if (xQueueReceive(g_uart_event_queue, &event, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (event.type) {
            case UART_FIFO_OVF:
                portENTER_CRITICAL(&g_uart_stats_lock);
                ++g_uart_fifo_overflows;
                portEXIT_CRITICAL(&g_uart_stats_lock);
                ESP_LOGE(TAG, "UART hardware FIFO overflow detected");
                break;
            case UART_BUFFER_FULL:
                portENTER_CRITICAL(&g_uart_stats_lock);
                ++g_uart_rx_buffer_full;
                portEXIT_CRITICAL(&g_uart_stats_lock);
                ESP_LOGE(TAG, "UART RX ring buffer full detected");
                break;
            case UART_FRAME_ERR:
                portENTER_CRITICAL(&g_uart_stats_lock);
                ++g_uart_frame_errors;
                portEXIT_CRITICAL(&g_uart_stats_lock);
                ESP_LOGE(TAG, "UART frame error detected");
                break;
            case UART_PARITY_ERR:
                portENTER_CRITICAL(&g_uart_stats_lock);
                ++g_uart_parity_errors;
                portEXIT_CRITICAL(&g_uart_stats_lock);
                ESP_LOGE(TAG, "UART parity error detected");
                break;
            default:
                break;
            }
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us >= next_stats_us) {
            uint64_t rx_bytes;
            uint32_t fifo_overflows;
            uint32_t rx_buffer_full;
            uint32_t frame_errors;
            uint32_t parity_errors;
            uint32_t transport_queue_drops;

            portENTER_CRITICAL(&g_uart_stats_lock);
            rx_bytes = g_uart_rx_bytes;
            fifo_overflows = g_uart_fifo_overflows;
            rx_buffer_full = g_uart_rx_buffer_full;
            frame_errors = g_uart_frame_errors;
            parity_errors = g_uart_parity_errors;
            transport_queue_drops = g_sml_tx_queue_drops;
            portEXIT_CRITICAL(&g_uart_stats_lock);

            ESP_LOGI(TAG,
                     "UART stats: rx_bytes=%llu fifo_overflows=%lu "
                     "rx_buffer_full=%lu frame_errors=%lu parity_errors=%lu "
                     "sml_transport_queue_drops=%lu",
                     (unsigned long long)rx_bytes,
                     (unsigned long)fifo_overflows,
                     (unsigned long)rx_buffer_full,
                     (unsigned long)frame_errors,
                     (unsigned long)parity_errors,
                     (unsigned long)transport_queue_drops);
            next_stats_us = now_us + 10000000LL;
        }
    }
}
#endif

static esp_err_t root_handler(httpd_req_t *req)
{
    char html[768];
    int len = snprintf(html, sizeof(html), "%s<p>FRITZ!Box: %s</p>",
                       METER_HTML_HEADER,
                       g_wifi_connected ? "connected" : "not connected");
    if (len < 0 || (size_t)len >= sizeof(html)) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send_chunk(req, html, len);
    if (err != ESP_OK) {
        return err;
    }

#if !CONFIG_ESPNOW_ROLE_SENDER
    static const char sml_page[] =
        "<style>.dashboard{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));"
        "gap:12px;margin:20px 0}.meter{background:#182330;padding:18px;border-radius:10px}"
        ".meter h2{font-size:1rem;margin:0 0 12px;color:#9fb4c8}.meter strong{font-size:1.7rem;"
        "color:#78d6ff}.meter small{display:block;color:#9fb4c8;margin-top:8px}"
        "#meter-status,#history-status{color:#9fb4c8}.history{margin-top:24px}"
        "#power-history{display:block;width:100%;height:260px;background:#121b27;border-radius:8px}"
        "</style><h1>Zählerübersicht</h1><div class=\"dashboard\">"
        "<section class=\"meter\"><h2>Verbrauch gesamt</h2><strong id=\"meter-import\">—</strong>"
        "<small>1-0:1.8.0 · kWh</small></section>"
        "<section class=\"meter\"><h2>Einspeisung gesamt</h2><strong id=\"meter-export\">—</strong>"
        "<small>1-0:2.8.0 · kWh</small></section>"
        "<section class=\"meter\"><h2>Momentanleistung</h2><strong id=\"meter-power\">—</strong>"
        "<small id=\"meter-direction\">1-0:16.7.0 · W</small></section>"
        "<section class=\"meter\"><h2>Durchschnitt letzte 15 Minuten</h2>"
        "<strong id=\"meter-average\">—</strong><small id=\"meter-average-direction\">W</small>"
        "</section></div><p id=\"meter-status\">Warte auf CRC-gültige SML-Werte…</p>"
        "<section class=\"history\"><h2>Minutenmittelwerte · letzte 2 Stunden</h2>"
        "<p>Verbrauch oberhalb, Einspeisung unterhalb der Nulllinie. "
        "<a href=\"/api/power-history.csv\">Gesamten gespeicherten Verlauf als CSV herunterladen</a></p>"
        "<canvas id=\"power-history\" aria-label=\"Diagramm der Minutenmittelwerte\"></canvas>"
        "<p id=\"history-status\">Warte auf synchronisierte Netzwerkzeit und gespeicherte Minutenwerte…</p>"
        "</section>"
        "<script>"
        "const byId=id=>document.getElementById(id);"
        "function scaled(v){return Number(v.value)*Math.pow(10,v.scaler);}"
        "function fmt(value,digits=2){return value.toLocaleString('de-DE',{minimumFractionDigits:digits,"
        "maximumFractionDigits:digits});}"
        "function showEnergy(id,v){byId(id).textContent=v?fmt(scaled(v)/1000):'Warte…';}"
        "async function updateMeter(){try{const r=await fetch('/api/dashboard',{cache:'no-store'});"
        "if(!r.ok)throw new Error('HTTP '+r.status);const d=await r.json();"
        "showEnergy('meter-import',d.import);showEnergy('meter-export',d.export);"
        "if(d.power){const watts=scaled(d.power);byId('meter-power').textContent="
        "(watts>0?'+':'')+fmt(watts,0)+' W';byId('meter-direction').textContent="
        "(watts<0?'Einspeisung':'Verbrauch')+' · 1-0:16.7.0';}else{"
        "byId('meter-power').textContent='Warte…';byId('meter-direction').textContent="
        "'1-0:16.7.0 · W';}"
        "if(d.average_power_w!==null&&d.average_samples){"
        "byId('meter-average').textContent=fmt(Math.abs(d.average_power_w),0)+' W';"
        "byId('meter-average-direction').textContent="
        "(d.average_power_w<0?'Einspeisung':'Verbrauch')+' · '+"
        "(d.average_duration_s>=899?'letzte 15 Minuten':"
        "'seit Start, sammelt 15 Minuten');}"
        "else{byId('meter-average').textContent='Sammle…';}"
        "byId('meter-status').textContent='SML CRC gültig: '+d.crc_valid+"
        "' · CRC fehlerhaft: '+d.crc_invalid+' · Transportlücken: '+d.transport_gaps;"
        "}catch(e){byId('meter-status').textContent='Verbindung zum Zähler fehlgeschlagen: '+e;}"
        "setTimeout(updateMeter,1000);}updateMeter();"
        "function drawHistory(rows){const canvas=byId('power-history'),rect=canvas.getBoundingClientRect();"
        "if(!rect.width)return;const ratio=window.devicePixelRatio||1;"
        "canvas.width=Math.floor(rect.width*ratio);canvas.height=Math.floor(260*ratio);"
        "const ctx=canvas.getContext('2d');ctx.scale(ratio,ratio);const w=rect.width,h=260,"
        "left=42,right=12,top=14,bottom=25,pw=w-left-right,ph=h-top-bottom,zero=top+ph/2;"
        "ctx.clearRect(0,0,w,h);ctx.strokeStyle='#526174';ctx.lineWidth=1;"
        "ctx.beginPath();ctx.moveTo(left,zero);ctx.lineTo(w-right,zero);ctx.stroke();"
        "const max=Math.max(1,...rows.map(x=>Math.abs(x.average_milliwatts/1000)))*1.1,"
        "now=Date.now(),start=now-7200000;"
        "ctx.fillStyle='#9fb4c8';ctx.font='12px sans-serif';ctx.fillText('+'+max.toFixed(0)+' W',2,top+10);"
        "ctx.fillText('0 W',2,zero+4);ctx.fillText('-'+max.toFixed(0)+' W',2,h-bottom);"
        "ctx.fillText('-2 h',left,h-6);ctx.fillText('-1 h',left+pw/2-15,h-6);"
        "ctx.fillText('jetzt',w-right-28,h-6);const barWidth=Math.max(2,pw/100*0.68);"
        "for(const r of rows){const t=Number(r.timestamp);if(t<start||t>now)continue;"
        "const value=r.watts,x=left+(t-start)/7200000*pw,"
        "bar=value/max*(ph/2-5);ctx.fillStyle=value<0?'#63d79a':'#78d6ff';"
        "ctx.fillRect(x,bar<0?zero:zero-bar,barWidth,Math.abs(bar));}"
        "}"
        "async function updateHistory(){try{const r=await fetch('/api/power-history?recent=128',"
        "{cache:'no-store'});if(!r.ok)throw new Error('HTTP '+r.status);const d=await r.json();"
        "drawHistory(d.records);const latest=d.records.length?new Date("
        "Number(d.records[d.records.length-1].timestamp)).toLocaleString('de-DE'):"
        "'noch kein abgeschlossener Minutenwert';"
        "byId('history-status').textContent=(d.clock_synced?'Netzwerkzeit synchronisiert':'Warte auf Netzwerkzeit')+"
        "' · gespeicherte Minutenwerte: '+d.record_count+' / 30000 · letztes: '+latest+"
        "' · aktuelle Messwerte im Intervall: '+d.current_sample_count+"
        "' · Speicher: '+(d.storage_ready?'bereit':'nicht bereit')+"
        "' · Schreibfehler: '+d.write_errors+' · Warteschlangenverluste: '+d.queue_drops;"
        "}catch(e){byId('history-status').textContent='Verlauf nicht verfügbar: '+e;}"
        "setTimeout(updateHistory,60000);}window.addEventListener('resize',()=>updateHistory());"
        "updateHistory();</script>";
    err = httpd_resp_sendstr_chunk(req, sml_page);
    if (err != ESP_OK) {
        return err;
    }
#endif

    static const char page_footer[] =
        "<nav class=\"nav\"><a href=\"/log\">SML logger</a>"
        "<a href=\"/diagnostics\">Sender diagnosis</a>"
        "<a href=\"/api\">JSON API</a><a href=\"/config\">Wi-Fi settings</a>"
        "<a href=\"/update\">Firmware update</a></nav>" METER_HTML_FOOTER;
    err = httpd_resp_sendstr_chunk(req, page_footer);
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t json_handler(httpd_req_t *req)
{
    char payload[64];
    int len = snprintf(payload, sizeof(payload),
                       "{\"fritzbox_connected\":%s}",
                       g_wifi_connected ? "true" : "false");
    if (len < 0 || (size_t)len >= sizeof(payload)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, payload, len);
}

#if !CONFIG_ESPNOW_ROLE_SENDER
static esp_err_t sml_api_handler(httpd_req_t *req)
{
    char query[64];
    char after_text[16] = "0";
    uint32_t after = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "after", after_text, sizeof(after_text)) == ESP_OK) {
        char *end = NULL;
        unsigned long parsed = strtoul(after_text, &end, 10);
        if (end == after_text || *end != '\0' || parsed > UINT32_MAX) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid after cursor");
            return ESP_FAIL;
        }
        after = (uint32_t)parsed;
    }

    sml_value_record_t records[SML_API_PAGE_SIZE];
    size_t record_count = 0;
    uint32_t oldest_sequence = 0;
    uint32_t latest_sequence;
    uint32_t valid_telegrams;
    uint32_t invalid_crc_telegrams;
    uint32_t transport_gaps;
    portENTER_CRITICAL(&g_sml_history_lock);
    latest_sequence = g_sml_latest_value_sequence;
    valid_telegrams = g_sml_valid_telegrams;
    invalid_crc_telegrams = g_sml_invalid_crc_telegrams;
    transport_gaps = g_sml_transport_gaps;
    size_t oldest_index = (g_sml_value_history_next +
                           SML_VALUE_HISTORY_COUNT -
                           g_sml_value_history_count) % SML_VALUE_HISTORY_COUNT;
    if (g_sml_value_history_count != 0) {
        oldest_sequence = g_sml_value_history[oldest_index].sequence;
    }
    for (size_t i = 0; i < g_sml_value_history_count; ++i) {
        size_t index = (oldest_index + i) % SML_VALUE_HISTORY_COUNT;
        if ((int32_t)(g_sml_value_history[index].sequence - after) > 0) {
            records[record_count++] = g_sml_value_history[index];
            if (record_count == SML_API_PAGE_SIZE) {
                break;
            }
        }
    }
    portEXIT_CRITICAL(&g_sml_history_lock);

    char header[256];
    bool overflow = oldest_sequence != 0 &&
                    (int32_t)(oldest_sequence - after) > 1;
    int header_len = snprintf(header, sizeof(header),
                              "{\"oldest_sequence\":%lu,\"latest_sequence\":%lu,"
                              "\"overflow\":%s,\"crc_valid\":%lu,"
                              "\"crc_invalid\":%lu,\"transport_gaps\":%lu,"
                              "\"values\":[",
                              (unsigned long)oldest_sequence,
                              (unsigned long)latest_sequence,
                              overflow ? "true" : "false",
                              (unsigned long)valid_telegrams,
                              (unsigned long)invalid_crc_telegrams,
                              (unsigned long)transport_gaps);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send_chunk(req, header, header_len);
    for (size_t i = 0; err == ESP_OK && i < record_count; ++i) {
        char item[192];
        int item_len = snprintf(item, sizeof(item),
                                "%s{\"sequence\":%lu,\"timestamp_us\":%llu,"
                                "\"obis\":\"%s\",\"value\":\"%s\","
                                "\"scaler\":%d,\"unit\":%u}",
                                i == 0 ? "" : ",",
                                (unsigned long)records[i].sequence,
                                (unsigned long long)records[i].timestamp_us,
                                records[i].obis, records[i].value,
                                (int)records[i].scaler,
                                (unsigned int)records[i].unit);
        if (item_len < 0 || (size_t)item_len >= sizeof(item)) {
            return ESP_FAIL;
        }
        err = httpd_resp_send_chunk(req, item, item_len);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "]}", 2);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t sml_dashboard_api_handler(httpd_req_t *req)
{
    sml_value_record_t latest[3];
    bool valid[3];
    uint32_t crc_valid;
    uint32_t crc_invalid;
    uint32_t transport_gaps;
    double power_sum;
    size_t power_count = 0;
    uint64_t power_reference_us = 0;
    uint64_t oldest_power_us = 0;

    portENTER_CRITICAL(&g_sml_history_lock);
    memcpy(latest, g_sml_latest_values, sizeof(latest));
    memcpy(valid, g_sml_latest_value_valid, sizeof(valid));
    crc_valid = g_sml_valid_telegrams;
    crc_invalid = g_sml_invalid_crc_telegrams;
    transport_gaps = g_sml_transport_gaps;

    power_sum = g_sml_power_sample_sum_w;
    power_count = g_sml_power_sample_count;
    if (valid[2] && power_count > 0) {
        power_reference_us = latest[2].timestamp_us;
        size_t oldest_index =
            (g_sml_power_sample_next + SML_POWER_SAMPLE_COUNT -
             g_sml_power_sample_count) % SML_POWER_SAMPLE_COUNT;
        oldest_power_us = g_sml_power_samples[oldest_index].timestamp_us;
    }
    portEXIT_CRITICAL(&g_sml_history_lock);

    double average_power_w =
        power_count != 0 ? power_sum / (double)power_count : 0.0;
    uint64_t average_duration_s =
        power_count != 0 ? (power_reference_us - oldest_power_us) / 1000000ULL : 0;
    if (average_duration_s > 900) {
        average_duration_s = 900;
    }

    char readings[3][128];
    for (size_t i = 0; i < 3; ++i) {
        if (valid[i]) {
            int written = snprintf(readings[i], sizeof(readings[i]),
                                   "{\"value\":\"%s\",\"scaler\":%d,\"unit\":%u}",
                                   latest[i].value, (int)latest[i].scaler,
                                   (unsigned int)latest[i].unit);
            if (written < 0 || (size_t)written >= sizeof(readings[i])) {
                return ESP_FAIL;
            }
        } else {
            strlcpy(readings[i], "null", sizeof(readings[i]));
        }
    }

    char payload[768];
    char average_json[48] = "null";
    if (power_count != 0) {
        int average_len = snprintf(average_json, sizeof(average_json),
                                   "%.2f", average_power_w);
        if (average_len < 0 || (size_t)average_len >= sizeof(average_json)) {
            return ESP_FAIL;
        }
    }
    int len = snprintf(payload, sizeof(payload),
                       "{\"import\":%s,\"export\":%s,\"power\":%s,"
                       "\"average_power_w\":%s,\"average_samples\":%u,"
                       "\"average_duration_s\":%llu,\"crc_valid\":%lu,"
                       "\"crc_invalid\":%lu,\"transport_gaps\":%lu}",
                       readings[0], readings[1], readings[2],
                       average_json,
                       (unsigned int)power_count,
                       (unsigned long long)average_duration_s,
                       (unsigned long)crc_valid,
                       (unsigned long)crc_invalid,
                       (unsigned long)transport_gaps);
    if (len < 0 || (size_t)len >= sizeof(payload)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, payload, len);
}

static esp_err_t power_samples_api_handler(httpd_req_t *req)
{
    uint64_t after_timestamp_us = 0;
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char value[24];
        if (httpd_query_key_value(query, "after_us", value, sizeof(value)) == ESP_OK) {
            char *end = NULL;
            unsigned long long parsed = strtoull(value, &end, 10);
            if (end == value || *end != '\0') {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "Invalid power-sample cursor");
                return ESP_FAIL;
            }
            after_timestamp_us = (uint64_t)parsed;
        }
    }

    sml_power_sample_t records[SML_POWER_API_PAGE_SIZE];
    size_t record_count = 0;
    portENTER_CRITICAL(&g_sml_history_lock);
    size_t oldest_index = (g_sml_power_sample_next + SML_POWER_SAMPLE_COUNT -
                           g_sml_power_sample_count) % SML_POWER_SAMPLE_COUNT;
    for (size_t i = 0; i < g_sml_power_sample_count; ++i) {
        size_t index = (oldest_index + i) % SML_POWER_SAMPLE_COUNT;
        if (g_sml_power_samples[index].timestamp_us > after_timestamp_us) {
            records[record_count++] = g_sml_power_samples[index];
            if (record_count == SML_POWER_API_PAGE_SIZE) {
                break;
            }
        }
    }
    portEXIT_CRITICAL(&g_sml_history_lock);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr_chunk(
        req, "{\"resolution\":\"1s\",\"time_source\":\"monotonic_us\",\"records\":[");
    for (size_t i = 0; err == ESP_OK && i < record_count; ++i) {
        char item[96];
        int item_len = snprintf(item, sizeof(item),
                                "%s{\"timestamp_us\":%llu,\"watts\":%.3f}",
                                i == 0 ? "" : ",",
                                (unsigned long long)records[i].timestamp_us,
                                records[i].watts);
        if (item_len < 0 || (size_t)item_len >= sizeof(item)) {
            return ESP_FAIL;
        }
        err = httpd_resp_send_chunk(req, item, item_len);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "]}", 2);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t power_history_api_handler(httpd_req_t *req)
{
    uint32_t after_sequence = 0;
    uint32_t recent_count = 0;
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char value[16];
        if (httpd_query_key_value(query, "after", value, sizeof(value)) == ESP_OK) {
            char *end = NULL;
            unsigned long parsed = strtoul(value, &end, 10);
            if (end == value || *end != '\0' || parsed > UINT32_MAX) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "Invalid history cursor");
                return ESP_FAIL;
            }
            after_sequence = (uint32_t)parsed;
        }
        if (httpd_query_key_value(query, "recent", value, sizeof(value)) == ESP_OK) {
            char *end = NULL;
            unsigned long parsed = strtoul(value, &end, 10);
            if (end == value || *end != '\0' || parsed > POWER_HISTORY_CAPACITY) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "Invalid recent history count");
                return ESP_FAIL;
            }
            recent_count = (uint32_t)parsed;
        }
    }

    power_history_record_t records[POWER_HISTORY_PAGE_SIZE];
    size_t record_count = 0;
    power_history_status_t status;
    bool overflow = false;
    esp_err_t err = power_history_read(after_sequence, recent_count, records,
                                       POWER_HISTORY_PAGE_SIZE, &record_count,
                                       &status, &overflow);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Verlaufs-Backend (SPIFFS/RAM) gerade nicht bereit, z. B. weil die
         * SPIFFS-Partition nicht montiert werden kann. Das ist kein
         * Serverfehler: wir liefern einen gueltigen, leeren Datensatz, damit
         * die Dashboard-Seite weiterlaeuft und storage_ready=false anzeigt. */
        record_count = 0;
        status.ready = false;
        ESP_LOGW(TAG, "Power history backend not ready; serving empty dataset");
    } else if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Persistent power history is unavailable");
        return err;
    }
    size_t current_sample_count;
    portENTER_CRITICAL(&g_sml_history_lock);
    current_sample_count = g_sml_power_sample_count;
    portEXIT_CRITICAL(&g_sml_history_lock);

    char header[320];
    int header_len = snprintf(
        header, sizeof(header),
        "{\"clock_synced\":%s,\"storage_ready\":%s,\"current_sample_count\":%u,"
        "\"oldest_sequence\":%lu,"
        "\"latest_sequence\":%lu,\"record_count\":%lu,"
        "\"queue_drops\":%lu,\"write_errors\":%lu,\"overflow\":%s,"
        "\"records\":[",
        system_time_is_valid() ? "true" : "false",
        status.ready ? "true" : "false",
        (unsigned int)current_sample_count,
        (unsigned long)status.oldest_sequence,
        (unsigned long)status.latest_sequence,
        (unsigned long)status.record_count,
        (unsigned long)status.queue_drops,
        (unsigned long)status.write_errors,
        overflow ? "true" : "false");
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send_chunk(req, header, header_len);
    for (size_t i = 0; err == ESP_OK && i < record_count; ++i) {
        char item[144];
        int item_len = snprintf(
            item, sizeof(item),
            "%s{\"sequence\":%lu,\"timestamp\":%lld,"
            "\"watts\":%.3f,\"average_milliwatts\":%ld,\"sample_count\":%lu}",
            i == 0 ? "" : ",",
            (unsigned long)records[i].sequence,
            (long long)records[i].interval_start_utc * 1000LL,
            (double)records[i].average_milliwatts / 1000.0,
            (long)records[i].average_milliwatts,
            (unsigned long)records[i].sample_count);
        if (item_len < 0 || (size_t)item_len >= sizeof(item)) {
            return ESP_FAIL;
        }
        err = httpd_resp_send_chunk(req, item, item_len);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "]}", 2);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t power_history_csv_handler(httpd_req_t *req)
{
    power_history_record_t records[POWER_HISTORY_PAGE_SIZE];
    size_t record_count = 0;
    uint32_t after_sequence = 0;
    power_history_status_t status;
    bool overflow = false;
    esp_err_t err = power_history_read(after_sequence, 0, records,
                                       POWER_HISTORY_PAGE_SIZE, &record_count,
                                       &status, &overflow);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Wie im JSON-Handler: ein fehlendes Backend ist kein HTTP-500,
         * sondern ein leerer CSV-Export. */
        record_count = 0;
        status.ready = false;
        ESP_LOGW(TAG, "Power history backend not ready; serving empty CSV");
    } else if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Persistent power history is unavailable");
        return err;
    }

    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"power-history.csv\"");
    err = httpd_resp_sendstr_chunk(
        req, "sequence,interval_start_ms,average_w,sample_count\r\n");
    while (err == ESP_OK && record_count != 0) {
        for (size_t i = 0; err == ESP_OK && i < record_count; ++i) {
            time_t interval_start =
                (time_t)records[i].interval_start_utc;
            struct tm interval_utc;
            if (gmtime_r(&interval_start, &interval_utc) == NULL) {
                return ESP_FAIL;
            }
            char interval_text[24];
            if (strftime(interval_text, sizeof(interval_text),
                         "%Y-%m-%dT%H:%M:%SZ", &interval_utc) == 0) {
                return ESP_FAIL;
            }
            char row[128];
            int row_len = snprintf(row, sizeof(row), "%lu,%s,%.3f,%lu\r\n",
                                   (unsigned long)records[i].sequence,
                                   interval_text,
                                   (double)records[i].average_milliwatts / 1000.0,
                                   (unsigned long)records[i].sample_count);
            if (row_len < 0 || (size_t)row_len >= sizeof(row)) {
                return ESP_FAIL;
            }
            err = httpd_resp_send_chunk(req, row, row_len);
            after_sequence = records[i].sequence;
        }
        if (err == ESP_OK) {
            err = power_history_read(after_sequence, 0, records,
                                     POWER_HISTORY_PAGE_SIZE, &record_count,
                                     &status, &overflow);
        }
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t rssi_api_handler(httpd_req_t *req)
{
    char query[96];
    char capture_value[8];
    uint32_t after = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "capture", capture_value,
                                  sizeof(capture_value)) == ESP_OK) {
            if (strcmp(capture_value, "1") == 0) {
                portENTER_CRITICAL(&g_rssi_history_lock);
                g_rssi_history_count = 0;
                g_rssi_history_next = 0;
                g_rssi_latest_sequence = 0;
                g_rssi_capture_active = true;
                portEXIT_CRITICAL(&g_rssi_history_lock);
            } else if (strcmp(capture_value, "0") == 0) {
                portENTER_CRITICAL(&g_rssi_history_lock);
                g_rssi_capture_active = false;
                portEXIT_CRITICAL(&g_rssi_history_lock);
            } else {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "capture must be 0 or 1");
                return ESP_FAIL;
            }
        }
        char after_value[16];
        if (httpd_query_key_value(query, "after", after_value,
                                  sizeof(after_value)) == ESP_OK) {
            char *end = NULL;
            unsigned long parsed = strtoul(after_value, &end, 10);
            if (end == after_value || *end != '\0' || parsed > UINT32_MAX) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "Invalid RSSI sequence");
                return ESP_FAIL;
            }
            after = (uint32_t)parsed;
        }
    }

    rssi_record_t records[RSSI_API_PAGE_SIZE];
    size_t record_count = 0;
    uint32_t oldest_sequence = 0;
    uint32_t latest_sequence;
    bool capture_active;
    portENTER_CRITICAL(&g_rssi_history_lock);
    latest_sequence = g_rssi_latest_sequence;
    capture_active = g_rssi_capture_active;
    size_t oldest_index = (g_rssi_history_next + RSSI_HISTORY_COUNT -
                           g_rssi_history_count) % RSSI_HISTORY_COUNT;
    if (g_rssi_history_count != 0) {
        oldest_sequence = g_rssi_history[oldest_index].sequence;
    }
    for (size_t i = 0; i < g_rssi_history_count; ++i) {
        size_t index = (oldest_index + i) % RSSI_HISTORY_COUNT;
        if ((int32_t)(g_rssi_history[index].sequence - after) > 0) {
            records[record_count++] = g_rssi_history[index];
            if (record_count == RSSI_API_PAGE_SIZE) {
                break;
            }
        }
    }
    portEXIT_CRITICAL(&g_rssi_history_lock);

    bool overflow = oldest_sequence != 0 &&
                    (int32_t)(oldest_sequence - after) > 1;
    char header[192];
    int header_len = snprintf(header, sizeof(header),
                              "{\"active\":%s,\"overflow\":%s,"
                              "\"latest_sequence\":%lu,\"samples\":[",
                              capture_active ? "true" : "false",
                              overflow ? "true" : "false",
                              (unsigned long)latest_sequence);
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send_chunk(req, header, header_len);
    for (size_t i = 0; err == ESP_OK && i < record_count; ++i) {
        char item[96];
        int item_len = snprintf(item, sizeof(item),
                                "%s{\"sequence\":%lu,\"timestamp_us\":%llu,"
                                "\"rssi_dbm\":%d}",
                                i == 0 ? "" : ",",
                                (unsigned long)records[i].sequence,
                                (unsigned long long)records[i].timestamp_us,
                                (int)records[i].rssi_dbm);
        if (item_len < 0 || (size_t)item_len >= sizeof(item)) {
            return ESP_FAIL;
        }
        err = httpd_resp_send_chunk(req, item, item_len);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "]}", 2);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}
#endif

static esp_err_t log_page_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>SML logger</title>"
        "<style>body{font-family:monospace;margin:20px;background:#10151f;color:#e5edf7}"
        "pre{white-space:pre-wrap;word-break:break-word}button,a{margin:4px;padding:8px 12px}"
        "a{color:#78d6ff}</style></head><body><h1>CRC-valid SML logger</h1>"
        "<button id=\"pause\" type=\"button\">Pause display</button>"
        "<button id=\"download-sml\" type=\"button\">Download decoded values</button>"
        "<button id=\"rssi-capture\" type=\"button\">Start RSSI capture</button>"
        "<button id=\"download-rssi\" type=\"button\">Download RSSI capture</button>"
        "<h2>CRC-valid SML values</h2><p id=\"sml-status\">Waiting for valid telegrams…</p>"
        "<pre id=\"sml-log\"></pre><h2>On-demand reception-strength log</h2>"
        "<p id=\"rssi-status\">Inactive; start capture to record signal strength.</p>"
        "<pre id=\"rssi-log\"></pre><script>"
        "let smlCursor=0,rssiCursor=0,paused=false,smlRendered=0,rssiRendered=0;"
        "let smlGap=false,rssiActive=false;const smlCaptured=[],rssiCaptured=[];"
        "const smlOut=document.getElementById('sml-log'),rssiOut=document.getElementById('rssi-log');"
        "const smlStatus=document.getElementById('sml-status'),rssiStatus=document.getElementById('rssi-status');"
        "function render(){if(paused)return;for(;smlRendered<smlCaptured.length;smlRendered++){"
        "const v=smlCaptured[smlRendered],unit={14:'m3',16:'m3/h',27:'W',30:'Wh',33:'A',35:'V'}[v.unit]"
        "||('unit-'+v.unit),label={'1.0.1.8.0.255':'Active energy import',"
        "'1.0.2.8.0.255':'Active energy export','1.0.16.7.0.255':'Active power'}[v.obis]"
        "||'SML value';let scaled;const integer=BigInt(v.value),negative=integer<0n;"
        "let digits=(negative?-integer:integer).toString();if(Math.abs(v.scaler)>12)"
        "scaled=v.value+' x10^'+v.scaler;else if(v.scaler>=0)"
        "scaled=(negative?'-':'')+digits+'0'.repeat(v.scaler);else{const places=-v.scaler;"
        "if(digits.length<=places)digits='0'.repeat(places+1-digits.length)+digits;"
        "const point=digits.length-places;digits=digits.slice(0,point)+'.'+digits.slice(point);"
        "digits=digits.replace(/0+$/,'').replace(/\\.$/,'');scaled=(negative?'-':'')+digits;}"
        "smlOut.textContent+=v.timestamp_us+' us | '+label+' ('+v.obis+') | '+"
        "String(scaled)+' '+unit+' | raw '+v.value+' x10^'+v.scaler+'\\n';}"
        "for(;rssiRendered<rssiCaptured.length;rssiRendered++){const x=rssiCaptured[rssiRendered];"
        "rssiOut.textContent+=x.timestamp_us+' us | '+x.rssi_dbm+' dBm\\n';}"
        "if(smlOut.textContent.length>100000)smlOut.textContent=smlOut.textContent.slice(-75000);}"
        "document.getElementById('pause').onclick=()=>{paused=!paused;"
        "document.getElementById('pause').textContent=paused?'Resume display':'Pause display';render();};"
        "function download(rows,name){if(!rows.length)return false;const lines=rows.map(x=>JSON.stringify(x));"
        "const blob=new Blob([lines.join('\\n')+'\\n'],{type:'application/x-ndjson'});"
        "const url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;"
        "a.download=name+'-'+new Date().toISOString().replace(/[:.]/g,'-')+'.jsonl';"
        "document.body.appendChild(a);a.click();a.remove();"
        "setTimeout(()=>URL.revokeObjectURL(url),1000);return true;}"
        "document.getElementById('download-sml').onclick=()=>{if(!download(smlCaptured,'sml-values'))"
        "smlStatus.textContent='No decoded SML values captured yet';};"
        "document.getElementById('download-rssi').onclick=()=>{if(!download(rssiCaptured,'rssi'))"
        "rssiStatus.textContent='No RSSI samples captured yet';};"
        "async function pollSml(){try{const r=await fetch('/api/sml?after='+smlCursor,{cache:'no-store'});"
        "if(!r.ok)throw new Error(r.status);const d=await r.json();if(d.overflow){smlGap=true;"
        "smlOut.textContent+='[GAP: decoded-value history overflowed]\\n';}"
        "for(const v of d.values){smlCaptured.push(v);smlCursor=v.sequence;}render();"
        "smlStatus.textContent='CRC valid: '+d.crc_valid+' | CRC invalid: '+d.crc_invalid+"
        "' | transport gaps: '+d.transport_gaps+"
        "(smlGap?' | VALUE GAP DETECTED':'');setTimeout(pollSml,d.values.length?0:250);}"
        "catch(e){smlStatus.textContent='Connection error: '+e;setTimeout(pollSml,1000);}}"
        "async function pollRssi(){if(!rssiActive)return;try{const r=await fetch('/api/rssi?after='+rssiCursor,"
        "{cache:'no-store'});if(!r.ok)throw new Error(r.status);const d=await r.json();"
        "if(d.overflow)rssiOut.textContent+='[RSSI history overflow]\\n';"
        "for(const x of d.samples){rssiCaptured.push(x);rssiCursor=x.sequence;}render();"
        "rssiStatus.textContent='Recording signal strength; '+rssiCaptured.length+' samples';"
        "setTimeout(pollRssi,1000);}catch(e){rssiStatus.textContent='RSSI capture error: '+e;"
        "setTimeout(pollRssi,1000);}}"
        "document.getElementById('rssi-capture').onclick=async()=>{const button="
        "document.getElementById('rssi-capture');if(!rssiActive){rssiCaptured.length=0;"
        "rssiCursor=0;rssiRendered=0;rssiOut.textContent='';try{const r=await fetch('/api/rssi?capture=1',"
        "{cache:'no-store'});if(!r.ok)throw new Error(r.status);rssiActive=true;"
        "button.textContent='Stop RSSI capture';rssiStatus.textContent='Waiting for received packets…';"
        "pollRssi();}catch(e){rssiStatus.textContent='Could not start RSSI capture: '+e;}}"
        "else{try{const r=await fetch('/api/rssi?capture=0&after='+rssiCursor,"
        "{cache:'no-store'});if(!r.ok)throw new Error(r.status);const d=await r.json();"
        "for(const x of d.samples){rssiCaptured.push(x);rssiCursor=x.sequence;}render();"
        "rssiActive=false;"
        "button.textContent='Start RSSI capture';rssiStatus.textContent='Capture stopped; '+"
        "rssiCaptured.length+' samples recorded.';}catch(e){rssiStatus.textContent='Could not stop RSSI capture: '+e;}}};"
        "pollSml();</script><p><a href=\"/\">Back</a></p></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, page);
}

static esp_err_t ota_page_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>Firmware update</title>"
        "<style>body{font-family:Arial;margin:24px;background:#10151f;color:#e5edf7}"
        "input,button{display:block;margin:12px 0;padding:8px}</style></head><body>"
        "<h1>Local firmware update</h1><p>Select the matching role firmware: "
        "receiver-sml.bin for this device or sender-sml.bin for the sender. "
        "Do not close this page during upload.</p>"
        "<label>Wi-Fi key (FRITZ!Box password when connected there; otherwise AP password)</label>"
        "<input id=\"key\" type=\"password\"><label>Target</label><select id=\"target\">"
        "<option value=\"/update\">Receiver (this device)</option>"
        "<option value=\"/update-sender\">Sender via ESP-NOW</option></select>"
        "<input id=\"file\" type=\"file\" accept=\".bin\">"
        "<button onclick=\"upload()\">Upload and reboot</button><p id=\"status\"></p>"
        "<script>async function upload(){const f=document.getElementById('file').files[0];"
        "if(!f){alert('Select a firmware .bin file');return;}const s=document.getElementById('status');"
        "s.textContent='Uploading…';try{const r=await fetch(document.getElementById('target').value,{method:'POST',"
        "headers:{'X-OTA-Key':document.getElementById('key').value},body:f});"
        "s.textContent=await r.text();if(!r.ok)throw new Error('HTTP '+r.status);}"
        "catch(e){s.textContent='Update failed: '+e;}}</script><p><a href=\"/\">Back</a></p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, page);
}

static bool ota_key_is_valid(httpd_req_t *req)
{
    size_t header_len = httpd_req_get_hdr_value_len(req, "X-OTA-Key");
    if (header_len == 0 || header_len > WIFI_PASSWORD_MAX_LEN) {
        return false;
    }
    char key[WIFI_PASSWORD_MAX_LEN + 1];
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Key", key, sizeof(key)) != ESP_OK) {
        return false;
    }
    const char *expected = g_wifi_configured ? g_wifi_password : "12345678";
    return strcmp(key, expected) == 0;
}

static esp_err_t send_http_error_status(httpd_req_t *req,
                                        const char *status,
                                        const char *message)
{
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, message);
}

#if !CONFIG_ESPNOW_ROLE_SENDER
static esp_err_t send_sender_ota_command(espnow_ota_packet_t *command,
                                         const uint8_t *payload,
                                         bool broadcast)
{
    uint8_t frame[ESP_NOW_MAX_DATA_LEN];
    if (command->payload_length > ESPNOW_OTA_CHUNK ||
        sizeof(*command) + command->payload_length > sizeof(frame)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(frame, command, sizeof(*command));
    if (command->payload_length > 0) {
        memcpy(frame + sizeof(*command), payload, command->payload_length);
    }

    for (unsigned int attempt = 0; attempt < 3; ++attempt) {
        const uint8_t *destination = broadcast ? broadcast_mac : g_espnow_ota_sender_mac;
        esp_err_t err = esp_now_send(destination, frame,
                                     sizeof(*command) + command->payload_length);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ESP-NOW OTA send attempt %u failed: %s",
                     attempt + 1, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int64_t deadline = esp_timer_get_time() +
                           (int64_t)ESPNOW_OTA_TIMEOUT_MS * 1000;
        while (esp_timer_get_time() < deadline) {
            int64_t remaining_us = deadline - esp_timer_get_time();
            TickType_t wait_ticks = pdMS_TO_TICKS((remaining_us + 999) / 1000);
            espnow_ota_ack_t ack;
            if (xQueueReceive(g_espnow_ota_ack_queue, &ack, wait_ticks) != pdTRUE) {
                break;
            }
            if (ack.packet.transfer_id != command->transfer_id ||
                ack.packet.type != ESPNOW_OTA_ACK ||
                ack.packet.sequence != command->sequence ||
                ack.packet.offset != command->offset) {
                continue;
            }
            memcpy(g_espnow_ota_sender_mac, ack.source, ESP_NOW_ETH_ALEN);
            err = add_espnow_peer(g_espnow_ota_sender_mac, WIFI_IF_AP);
            if (err != ESP_OK) {
                return err;
            }
            if (ack.packet.status != ESP_OK) {
                return (esp_err_t)ack.packet.status;
            }
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Timed out waiting for ESP-NOW OTA ACK (seq=%lu, attempt=%u)",
                 (unsigned long)command->sequence, attempt + 1);
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t relay_sender_firmware(httpd_req_t *req)
{
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty sender firmware upload");
        return ESP_FAIL;
    }
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL || (size_t)req->content_len > partition->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware exceeds sender OTA partition size");
        return ESP_FAIL;
    }

    if (xSemaphoreTake(g_espnow_ota_mutex, 0) != pdTRUE) {
        send_http_error_status(req, "409 Conflict", "Another OTA transfer is active");
        return ESP_FAIL;
    }

    while (xQueueReceive(g_espnow_ota_ack_queue, &(espnow_ota_ack_t){0}, 0) == pdTRUE) {
    }

    uint32_t transfer_id = (uint32_t)esp_random();
    if (transfer_id == 0) {
        transfer_id = 1;
    }
    espnow_ota_packet_t command = {
        .magic = ESPNOW_OTA_MAGIC,
        .version = ESPNOW_OTA_VERSION,
        .type = ESPNOW_OTA_BEGIN,
        .transfer_id = transfer_id,
        .total_size = (uint32_t)req->content_len,
    };
    esp_err_t err = send_sender_ota_command(&command, NULL, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Sender did not accept OTA start: %s", esp_err_to_name(err));
        send_http_error_status(req, "504 Gateway Timeout",
                               "Sender did not acknowledge OTA start");
        goto cleanup;
    }

    uint8_t chunk[ESPNOW_OTA_CHUNK];
    uint32_t offset = 0;
    uint32_t sequence = 0;
    while (offset < (uint32_t)req->content_len) {
        size_t expected = (uint32_t)req->content_len - offset;
        if (expected > sizeof(chunk)) {
            expected = sizeof(chunk);
        }
        size_t received_total = 0;
        while (received_total < expected) {
            int received = httpd_req_recv(req, (char *)chunk + received_total,
                                          expected - received_total);
            if (received <= 0) {
                err = ESP_FAIL;
                ESP_LOGE(TAG, "Sender firmware HTTP upload interrupted");
                goto abort_transfer;
            }
            received_total += (size_t)received;
        }

        command.type = ESPNOW_OTA_DATA;
        command.sequence = sequence;
        command.offset = offset;
        command.total_size = (uint32_t)req->content_len;
        command.payload_length = (uint16_t)received_total;
        err = send_sender_ota_command(&command, chunk, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Sender OTA data transfer failed at offset %lu: %s",
                     (unsigned long)offset, esp_err_to_name(err));
            goto abort_transfer;
        }
        offset += (uint32_t)received_total;
        ++sequence;
    }

    command.type = ESPNOW_OTA_END;
    command.sequence = sequence;
    command.offset = offset;
    command.total_size = (uint32_t)req->content_len;
    command.payload_length = 0;
    err = send_sender_ota_command(&command, NULL, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Sender OTA finalization failed: %s", esp_err_to_name(err));
        send_http_error_status(req, "502 Bad Gateway",
                               "Sender did not confirm firmware validation");
        goto cleanup;
    }

    httpd_resp_sendstr(req, "Sender firmware validated and activated. Sender is restarting.");
    goto cleanup;

abort_transfer:
    command.type = ESPNOW_OTA_ABORT;
    command.sequence = sequence;
    command.offset = offset;
    command.total_size = (uint32_t)req->content_len;
    command.payload_length = 0;
    (void)send_sender_ota_command(&command, NULL, false);
    send_http_error_status(req, "502 Bad Gateway",
                           "ESP-NOW sender firmware transfer failed");

cleanup:
    xSemaphoreGive(g_espnow_ota_mutex);
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t ota_sender_update_handler(httpd_req_t *req)
{
    if (!ota_key_is_valid(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid Wi-Fi key");
        return ESP_FAIL;
    }
    return relay_sender_firmware(req);
}
#endif

static esp_err_t ota_update_handler(httpd_req_t *req)
{
    if (!ota_key_is_valid(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid Wi-Fi key");
        return ESP_FAIL;
    }
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty firmware upload");
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL || (size_t)req->content_len > update_partition->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware exceeds OTA partition size");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, (size_t)req->content_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not begin OTA: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not start OTA");
        return ESP_FAIL;
    }

    uint8_t buffer[1024];
    size_t remaining = (size_t)req->content_len;
    while (remaining > 0) {
        size_t amount = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        int received = httpd_req_recv(req, (char *)buffer, amount);
        if (received <= 0) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware upload interrupted");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buffer, (size_t)received);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            ESP_LOGE(TAG, "OTA flash write failed: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Firmware write failed");
            return ESP_FAIL;
        }
        remaining -= (size_t)received;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA image validation failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware image is invalid");
        return ESP_FAIL;
    }
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not select OTA boot partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not activate firmware");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA image accepted; rebooting into partition %s", update_partition->label);
    httpd_resp_sendstr(req, "Firmware accepted. Device is restarting.");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decode_form_value(const char *source, char *destination, size_t capacity)
{
    size_t out = 0;
    for (size_t i = 0; source[i] != '\0'; ++i) {
        char value = source[i];
        if (value == '+') {
            value = ' ';
        } else if (value == '%') {
            int high = hex_value(source[i + 1]);
            int low = source[i + 2] != '\0' ? hex_value(source[i + 2]) : -1;
            if (high < 0 || low < 0) {
                return false;
            }
            value = (char)((high << 4) | low);
            i += 2;
        }
        if (value == '\0' || out + 1 >= capacity) {
            return false;
        }
        destination[out++] = value;
    }
    destination[out] = '\0';
    return true;
}

static esp_err_t config_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>Wi-Fi settings</title>"
        "<style>body{font-family:Arial;margin:24px;background:#10151f;color:#e5edf7}"
        "input{display:block;margin:8px 0 16px;padding:8px;width:280px}</style></head>"
        "<body><h1>FRITZ!Box Wi-Fi</h1><p>Leave empty to disable the station connection. "
        "The ESP32 remains available through its setup access point.</p>"
        "<form method=\"post\" action=\"/save\">"
        "<label>2.4 GHz Wi-Fi SSID</label><input name=\"ssid\" maxlength=\"32\" required>"
        "<label>Wi-Fi password</label><input name=\"password\" type=\"password\" maxlength=\"64\">"
        "<button type=\"submit\">Save and connect</button></form>"
        "<p><a href=\"/\">Back to meter</a></p></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, page);
}

static esp_err_t save_wifi_handler(httpd_req_t *req)
{
    char body[256];
    char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    char password[WIFI_PASSWORD_MAX_LEN + 1] = {0};
    bool have_ssid = false;
    bool have_password = false;

    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form size");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Could not read form");
            return ESP_FAIL;
        }
        received += (size_t)count;
    }
    body[received] = '\0';

    char *field = body;
    while (field != NULL) {
        char *next = strchr(field, '&');
        if (next != NULL) {
            *next++ = '\0';
        }
        char *equals = strchr(field, '=');
        if (equals == NULL) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed form field");
            return ESP_FAIL;
        }
        *equals++ = '\0';

        if (strcmp(field, "ssid") == 0) {
            have_ssid = decode_form_value(equals, ssid, sizeof(ssid));
            if (!have_ssid) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SSID");
                return ESP_FAIL;
            }
        } else if (strcmp(field, "password") == 0) {
            have_password = decode_form_value(equals, password, sizeof(password));
            if (!have_password) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid password field");
                return ESP_FAIL;
            }
        }
        field = next;
    }

    size_t password_len = strlen(password);
    if (!have_ssid || !have_password || ssid[0] == '\0' ||
        (password_len != 0 && (password_len < 8 || password_len > 64))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID or password length is invalid");
        return ESP_FAIL;
    }

    esp_err_t err = save_wifi_credentials(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save Wi-Fi credentials: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save settings");
        return ESP_FAIL;
    }

    (void)esp_wifi_disconnect();
    wifi_config_t sta_config;
    fill_station_config(&sta_config);
    err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start Wi-Fi connection: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Settings saved, but connection could not start");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req,
                              "<!doctype html><html><body><p>Settings saved. "
                              "Connecting to Wi-Fi; check the serial log for the IP address.</p>"
                              "<a href=\"/\">Back</a></body></html>");
}

static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 24;
    config.stack_size = 6144;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
    };

    httpd_uri_t data = {
        .uri = "/api",
        .method = HTTP_GET,
        .handler = json_handler,
    };
#if !CONFIG_ESPNOW_ROLE_SENDER
    httpd_uri_t sender_diagnostics_api = {
        .uri = "/api/sender-diagnostics",
        .method = HTTP_GET,
        .handler = sender_diagnostics_api_handler,
    };
    httpd_uri_t sender_reboot = {
        .uri = "/api/sender-reboot",
        .method = HTTP_POST,
        .handler = sender_reboot_handler,
    };
    httpd_uri_t diagnostics_page = {
        .uri = "/diagnostics",
        .method = HTTP_GET,
        .handler = diagnostics_page_handler,
    };
    httpd_uri_t sml_api = {
        .uri = "/api/sml",
        .method = HTTP_GET,
        .handler = sml_api_handler,
    };
    httpd_uri_t sml_dashboard_api = {
        .uri = "/api/dashboard",
        .method = HTTP_GET,
        .handler = sml_dashboard_api_handler,
    };
    httpd_uri_t power_samples_api = {
        .uri = "/api/power-samples",
        .method = HTTP_GET,
        .handler = power_samples_api_handler,
    };
    httpd_uri_t power_history_api = {
        .uri = "/api/power-history",
        .method = HTTP_GET,
        .handler = power_history_api_handler,
    };
    httpd_uri_t power_history_csv = {
        .uri = "/api/power-history.csv",
        .method = HTTP_GET,
        .handler = power_history_csv_handler,
    };
    httpd_uri_t rssi_api = {
        .uri = "/api/rssi",
        .method = HTTP_GET,
        .handler = rssi_api_handler,
    };
#endif
    httpd_uri_t log_page = {
        .uri = "/log",
        .method = HTTP_GET,
        .handler = log_page_handler,
    };
    httpd_uri_t update_page = {
        .uri = "/update",
        .method = HTTP_GET,
        .handler = ota_page_handler,
    };
    httpd_uri_t update = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = ota_update_handler,
    };
#if !CONFIG_ESPNOW_ROLE_SENDER
    httpd_uri_t update_sender = {
        .uri = "/update-sender",
        .method = HTTP_POST,
        .handler = ota_sender_update_handler,
    };
#endif
    httpd_uri_t config_uri = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = config_handler,
    };
    httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_wifi_handler,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &data));
#if !CONFIG_ESPNOW_ROLE_SENDER
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &sender_diagnostics_api));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &sender_reboot));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &diagnostics_page));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &sml_api));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &sml_dashboard_api));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &power_samples_api));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &power_history_api));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &power_history_csv));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &rssi_api));
#endif
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &log_page));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &config_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &update_page));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &update));
#if !CONFIG_ESPNOW_ROLE_SENDER
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &update_sender));
#endif
    ESP_LOGI(TAG, "Webserver available through the setup AP at http://192.168.4.1/");
}

#if CONFIG_ESPNOW_ROLE_SENDER
static void run_sender(void)
{
    const uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUFFER_SIZE, 0,
                                        UART_EVENT_QUEUE_LENGTH,
                                        &g_uart_event_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_PIN_NO_CHANGE, UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    g_sml_tx_queue = xQueueCreate(SML_TX_QUEUE_LENGTH, sizeof(sml_tx_item_t));
    if (g_sml_tx_queue == NULL) {
        ESP_LOGE(TAG, "Could not create SML transport queue");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    sml_adapter_reset();

    // Send-completion semaphore removed: the transport worker drains the
    // SML queue at line rate (esp_now_send is non-blocking), which prevents
    // the transport queue from filling up and dropping validated telegrams.
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_callback));

    ESP_LOGI(TAG, "Sender ready: UART%d RX GPIO=%d at %d baud (8N1, no flow control); "
                  "ESP-NOW channel %u",
             UART_PORT_NUM, UART_RX_GPIO, UART_BAUD_RATE, g_espnow_channel);
    ESP_LOGI(TAG, "UART RX ring buffer: %u bytes; SML transport queue: %u telegrams",
             (unsigned int)UART_BUFFER_SIZE, (unsigned int)SML_TX_QUEUE_LENGTH);
    ESP_LOGI(TAG, "Only CRC-checked SML values and telegram status are transmitted");

    BaseType_t task_result = xTaskCreate(uart_event_worker, "uart_events",
                                         4096, NULL, 6, NULL);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Could not start UART event/statistics worker");
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    task_result = xTaskCreate(uart_transport_worker, "sml_transport",
                              4096, NULL, 9, NULL);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Could not start SML transport worker");
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    task_result = xTaskCreate(uart_capture_worker, "uart_capture",
                              4096, NULL, 10, NULL);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Could not start UART capture worker");
        ESP_ERROR_CHECK(ESP_FAIL);
    }
}
#endif

#if CONFIG_ESPNOW_ROLE_SENDER
static void sender_diagnostics_task(void *arg)
{
    (void)arg;
    uint32_t sequence = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        espnow_diagnostic_packet_t packet = {
            .magic = ESPNOW_DIAG_MAGIC,
            .version = ESPNOW_DIAG_VERSION,
            .type = ESPNOW_DIAG_TELEMETRY,
            .sequence = ++sequence,
            .uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000LL),
            .reset_reason = (uint32_t)esp_reset_reason(),
            .free_heap = (uint32_t)esp_get_free_heap_size(),
            .minimum_free_heap = (uint32_t)esp_get_minimum_free_heap_size(),
        };
        portENTER_CRITICAL(&g_uart_stats_lock);
        packet.uart_rx_bytes = g_uart_rx_bytes;
        packet.tx_queue_drops = g_sml_tx_queue_drops;
        packet.uart_fifo_overflows = g_uart_fifo_overflows;
        packet.uart_rx_buffer_full = g_uart_rx_buffer_full;
        packet.uart_frame_errors = g_uart_frame_errors;
        packet.uart_parity_errors = g_uart_parity_errors;
        portEXIT_CRITICAL(&g_uart_stats_lock);

        esp_err_t err = esp_now_send(broadcast_mac, (const uint8_t *)&packet,
                                     sizeof(packet));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Could not send sender diagnostics: %s",
                     esp_err_to_name(err));
        }
    }
}

static void sender_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}
#endif

#if !CONFIG_ESPNOW_ROLE_SENDER
static esp_err_t sender_diagnostics_api_handler(httpd_req_t *req)
{
    espnow_diagnostic_packet_t packet;
    int64_t received_us;
    uint32_t reboot_ack_sequence;
    uint32_t reboot_requested_sequence;
    bool seen;
    portENTER_CRITICAL(&g_sender_diagnostics_lock);
    packet = g_sender_diagnostics;
    received_us = g_sender_diagnostics_received_us;
    reboot_ack_sequence = g_sender_reboot_ack_sequence;
    reboot_requested_sequence = g_sender_reboot_requested_sequence;
    seen = g_sender_diagnostics_seen;
    portEXIT_CRITICAL(&g_sender_diagnostics_lock);

    int64_t age_ms = seen ? (esp_timer_get_time() - received_us) / 1000 : -1;
    char payload[512];
    int len = snprintf(
        payload, sizeof(payload),
        "{\"seen\":%s,\"age_ms\":%lld,\"sequence\":%lu,"
        "\"uptime_seconds\":%lu,\"reset_reason\":%lu,"
        "\"free_heap\":%lu,\"minimum_free_heap\":%lu,"
        "\"uart_rx_bytes\":%llu,\"tx_queue_drops\":%lu,"
        "\"uart_fifo_overflows\":%lu,\"uart_rx_buffer_full\":%lu,"
        "\"uart_frame_errors\":%lu,\"uart_parity_errors\":%lu,"
        "\"reboot_ack_sequence\":%lu,\"reboot_acknowledged\":%s}",
        seen ? "true" : "false", (long long)age_ms,
        (unsigned long)packet.sequence,
        (unsigned long)packet.uptime_seconds,
        (unsigned long)packet.reset_reason,
        (unsigned long)packet.free_heap,
        (unsigned long)packet.minimum_free_heap,
        (unsigned long long)packet.uart_rx_bytes,
        (unsigned long)packet.tx_queue_drops,
        (unsigned long)packet.uart_fifo_overflows,
        (unsigned long)packet.uart_rx_buffer_full,
        (unsigned long)packet.uart_frame_errors,
        (unsigned long)packet.uart_parity_errors,
        (unsigned long)reboot_ack_sequence,
        reboot_requested_sequence != 0 &&
                reboot_ack_sequence == reboot_requested_sequence
            ? "true"
            : "false");
    if (len < 0 || (size_t)len >= sizeof(payload)) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, payload, len);
}

static esp_err_t sender_reboot_handler(httpd_req_t *req)
{
    espnow_diagnostic_packet_t request = {
        .magic = ESPNOW_DIAG_MAGIC,
        .version = ESPNOW_DIAG_VERSION,
        .type = ESPNOW_DIAG_REBOOT,
        .sequence = (uint32_t)esp_timer_get_time(),
    };
    if (request.sequence == 0) {
        request.sequence = 1;
    }
    esp_err_t err = esp_now_send(broadcast_mac, (const uint8_t *)&request,
                                 sizeof(request));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not send sender reboot request: %s",
                 esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Could not send reboot request");
        return err;
    }
    portENTER_CRITICAL(&g_sender_diagnostics_lock);
    g_sender_reboot_requested_sequence = request.sequence;
    portEXIT_CRITICAL(&g_sender_diagnostics_lock);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"sent\":true}");
}

static esp_err_t diagnostics_page_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>Sender diagnostics</title>"
        "<style>body{font:16px Arial;margin:24px;background:#10151f;color:#e5edf7}"
        "pre{padding:16px;background:#182330;white-space:pre-wrap}button,a{padding:10px;"
        "margin-right:8px}</style></head><body><h1>Sender diagnosis</h1>"
        "<p id=\"status\">Waiting for sender diagnostic packets…</p><pre id=\"data\"></pre>"
        "<button id=\"reboot\" type=\"button\">Restart sender</button>"
        "<a href=\"/\">Back to meter</a><script>"
        "const status=document.getElementById('status'),output=document.getElementById('data');"
        "async function refresh(){try{const r=await fetch('/api/sender-diagnostics',"
        "{cache:'no-store'});if(!r.ok)throw new Error('HTTP '+r.status);const d=await r.json();"
        "status.textContent=d.reboot_acknowledged?'Sender acknowledged the restart; waiting for reboot':"
        "(!d.seen?'No sender packet received yet':"
        "(d.age_ms>30000?'STALE: last packet '+Math.round(d.age_ms/1000)+' s ago':"
        "'Sender online · last packet '+Math.round(d.age_ms/1000)+' s ago'));"
        "output.textContent=d.seen?JSON.stringify({uptime_seconds:d.uptime_seconds,"
        "reset_reason:d.reset_reason,free_heap_bytes:d.free_heap,"
        "minimum_free_heap_bytes:d.minimum_free_heap,uart_rx_bytes:d.uart_rx_bytes,"
        "telemetry_queue_drops:d.tx_queue_drops,uart_fifo_overflows:d.uart_fifo_overflows,"
        "uart_rx_buffer_full:d.uart_rx_buffer_full,uart_frame_errors:d.uart_frame_errors,"
        "uart_parity_errors:d.uart_parity_errors,reboot_acknowledged:d.reboot_acknowledged,"
        "reboot_ack_sequence:d.reboot_ack_sequence},null,2):'';"
        "}catch(e){status.textContent='Diagnostics unavailable: '+e;}"
        "setTimeout(refresh,3000);}document.getElementById('reboot').onclick=async()=>{"
        "if(!confirm('Restart the sender now?'))return;status.textContent='Sending restart request…';"
        "try{const r=await fetch('/api/sender-reboot',{method:'POST',cache:'no-store'});"
        "if(!r.ok)throw new Error('HTTP '+r.status);status.textContent="
        "'Request sent; waiting for acknowledgement and next boot…';setTimeout(refresh,1000);"
        "}catch(e){status.textContent='Restart request failed: '+e;}};refresh();"
        "</script></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, page);
}
#endif

static void diagnostic_receive_callback(const esp_now_recv_info_t *info,
                                        const uint8_t *data, int data_len)
{
    if (info != NULL && data != NULL &&
        data_len == (int)sizeof(espnow_diagnostic_packet_t)) {
        espnow_diagnostic_packet_t packet;
        memcpy(&packet, data, sizeof(packet));
        if (packet.magic == ESPNOW_DIAG_MAGIC &&
            packet.version == ESPNOW_DIAG_VERSION) {
#if CONFIG_ESPNOW_ROLE_SENDER
            if (packet.type == ESPNOW_DIAG_REBOOT) {
                espnow_diagnostic_packet_t ack = {
                    .magic = ESPNOW_DIAG_MAGIC,
                    .version = ESPNOW_DIAG_VERSION,
                    .type = ESPNOW_DIAG_REBOOT_ACK,
                    .sequence = packet.sequence,
                };
                esp_err_t err = esp_now_send(broadcast_mac,
                                             (const uint8_t *)&ack,
                                             sizeof(ack));
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Could not acknowledge sender reboot: %s",
                             esp_err_to_name(err));
                }
                BaseType_t result = xTaskCreate(sender_reboot_task,
                                                "sender_reboot", 2048,
                                                NULL, 5, NULL);
                if (result != pdPASS) {
                    ESP_LOGE(TAG, "Could not start requested sender reboot");
                }
            }
#else
            if (packet.type == ESPNOW_DIAG_TELEMETRY) {
                portENTER_CRITICAL(&g_sender_diagnostics_lock);
                g_sender_diagnostics = packet;
                g_sender_diagnostics_received_us = esp_timer_get_time();
                g_sender_diagnostics_seen = true;
                portEXIT_CRITICAL(&g_sender_diagnostics_lock);
            } else if (packet.type == ESPNOW_DIAG_REBOOT_ACK) {
                portENTER_CRITICAL(&g_sender_diagnostics_lock);
                g_sender_reboot_ack_sequence = packet.sequence;
                portEXIT_CRITICAL(&g_sender_diagnostics_lock);
            }
#endif
            return;
        }
    }
    receive_callback(info, data, data_len);
}

void app_main(void)
{
    initialize_wifi_and_espnow();
#if CONFIG_ESPNOW_ROLE_SENDER
    g_espnow_ota_rx_queue = xQueueCreate(4, sizeof(espnow_ota_rx_item_t));
    if (g_espnow_ota_rx_queue == NULL) {
        ESP_LOGE(TAG, "Could not create sender OTA receive queue");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    BaseType_t task_result = xTaskCreate(espnow_ota_worker, "espnow_ota",
                                         6144, NULL, 5, NULL);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Could not start sender OTA worker");
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    task_result = xTaskCreate(sender_diagnostics_task, "sender_diagnostics",
                              3072, NULL, 3, NULL);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Could not start sender diagnostics task");
        ESP_ERROR_CHECK(ESP_FAIL);
    }
#else
    g_espnow_ota_ack_queue = xQueueCreate(4, sizeof(espnow_ota_ack_t));
    g_espnow_ota_mutex = xSemaphoreCreateMutex();
    if (g_espnow_ota_ack_queue == NULL || g_espnow_ota_mutex == NULL) {
        ESP_LOGE(TAG, "Could not create receiver OTA synchronization objects");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    esp_err_t history_err = power_history_init();
    if (history_err != ESP_OK) {
        ESP_LOGE(TAG, "Persistent minute history is disabled: %s",
                 esp_err_to_name(history_err));
    }
    const esp_timer_create_args_t power_sample_timer_args = {
        .callback = power_sample_timer_callback,
        .name = "power_samples",
    };
    ESP_ERROR_CHECK(esp_timer_create(&power_sample_timer_args,
                                     &g_power_sample_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(g_power_sample_timer, 1000000));
#endif
    ESP_ERROR_CHECK(esp_now_register_recv_cb(diagnostic_receive_callback));

    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    if (running_partition == NULL) {
        ESP_LOGE(TAG, "Could not determine the running app partition");
    } else if (running_partition->type == ESP_PARTITION_TYPE_APP &&
               running_partition->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
               running_partition->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        esp_err_t ota_state_err = esp_ota_mark_app_valid_cancel_rollback();
        if (ota_state_err != ESP_OK &&
            ota_state_err != ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
            ESP_LOGE(TAG, "Could not mark current OTA image valid: %s",
                     esp_err_to_name(ota_state_err));
        }
    }

#if CONFIG_ESPNOW_ROLE_SENDER
    run_sender();
#else
    start_webserver();
    ESP_LOGI(TAG, "Receiver ready: http://192.168.4.1/ | /log | /config | /update");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
}
