#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "espnow_meter";

#define ESPNOW_CHANNEL       1
#define UART_PORT_NUM        UART_NUM_1
#define UART_RX_GPIO         16
#define UART_BAUD_RATE       9600
#define UART_BUFFER_SIZE     8192
#define UART_PACKET_BYTES    64
#define UART_EVENT_QUEUE_LENGTH 32
#define UART_TRANSPORT_QUEUE_LENGTH 128
#define METER_MAGIC          0x48425A31u
#define METER_VERSION        1u
#define RAW_PACKET_MAGIC     0x52415731u
#define RAW_PACKET_VERSION   1u
#define RAW_HISTORY_COUNT    128
#define RAW_API_PAGE_SIZE    8
#define SML_MAX_TELEGRAM_BYTES 2048
#define SML_VALUE_HISTORY_COUNT 64
#define SML_API_PAGE_SIZE    16
#define SML_PARSER_QUEUE_LENGTH 32
#define ESPNOW_OTA_MAGIC     0x4F544131u
#define ESPNOW_OTA_VERSION   1u
#define ESPNOW_OTA_BEGIN     1u
#define ESPNOW_OTA_DATA      2u
#define ESPNOW_OTA_END       3u
#define ESPNOW_OTA_ABORT     4u
#define ESPNOW_OTA_ACK       0x80u
#define ESPNOW_OTA_CHUNK     220u
#define ESPNOW_OTA_TIMEOUT_MS 3000
#define WIFI_CONFIG_NAMESPACE "metercfg"
#define WIFI_SSID_KEY          "wifi_ssid"
#define WIFI_PASSWORD_KEY      "wifi_pass"
#define WIFI_SSID_MAX_LEN      32
#define WIFI_PASSWORD_MAX_LEN  64

#define METER_HTML_HEADER "<!doctype html><html><head><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"5\"><title>ESP32 IR Meter</title><style>body{font-family:Arial,sans-serif;margin:24px;background:#10151f;color:#e5edf7} .card{background:#182330;padding:20px;border-radius:12px;max-width:560px} .value{font-size:2rem;font-weight:700;color:#78d6ff} .small{font-size:0.8rem;color:#9fb4c8}.nav{display:flex;flex-wrap:wrap;gap:10px;margin-top:18px}.nav a,.action{display:inline-block;padding:10px 14px;border:0;border-radius:7px;background:#176b91;color:#fff;text-decoration:none;font:inherit;cursor:pointer}.nav a:hover,.action:hover{background:#2086b2}</style></head><body><div class=\"card\"><h1>ESP32 IR Meter</h1>"

#define METER_HTML_FOOTER "</div></body></html>"

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint32_t block_sequence;
    uint64_t timestamp_us;
    uint16_t raw_length;
    uint16_t fragment_offset;
    uint16_t fragment_length;
} raw_packet_header_t;

typedef struct {
    uint32_t sequence;
    uint64_t timestamp_us;
    uint16_t length;
    uint8_t data[UART_PACKET_BYTES];
} raw_record_t;

typedef struct {
    uint32_t transport_sequence;
    uint64_t timestamp_us;
    uint16_t length;
    uint8_t data[UART_PACKET_BYTES];
} sml_parser_item_t;

typedef struct {
    uint32_t sequence;
    uint64_t timestamp_us;
    char obis[24];
    char value[24];
    int8_t scaler;
    uint8_t unit;
} sml_value_record_t;

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

static const uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

static char g_wifi_ssid[WIFI_SSID_MAX_LEN + 1];
static char g_wifi_password[WIFI_PASSWORD_MAX_LEN + 1];
static bool g_wifi_configured;
static bool g_wifi_connected;
static esp_netif_t *g_ap_netif;
static esp_netif_t *g_sta_netif;
static uint8_t g_espnow_channel = ESPNOW_CHANNEL;
static raw_record_t g_raw_history[RAW_HISTORY_COUNT];
static size_t g_raw_history_count;
static size_t g_raw_history_next;
static uint32_t g_raw_latest_sequence;
static portMUX_TYPE g_raw_history_lock = portMUX_INITIALIZER_UNLOCKED;
#if CONFIG_ESPNOW_ROLE_SENDER
typedef struct {
    uint64_t timestamp_us;
    uint64_t delta_us;
    uint16_t length;
    uint8_t data[UART_PACKET_BYTES];
} uart_raw_chunk_t;

static SemaphoreHandle_t g_send_done;
static esp_now_send_status_t g_last_send_status;
static QueueHandle_t g_espnow_ota_rx_queue;
static QueueHandle_t g_uart_event_queue;
static QueueHandle_t g_uart_raw_queue;
static uint64_t g_uart_rx_bytes;
static uint32_t g_uart_fifo_overflows;
static uint32_t g_uart_rx_buffer_full;
static uint32_t g_uart_frame_errors;
static uint32_t g_uart_parity_errors;
static portMUX_TYPE g_uart_stats_lock = portMUX_INITIALIZER_UNLOCKED;
#else
static QueueHandle_t g_espnow_ota_ack_queue;
static SemaphoreHandle_t g_espnow_ota_mutex;
static uint8_t g_espnow_ota_sender_mac[ESP_NOW_ETH_ALEN];
static QueueHandle_t g_sml_parser_queue;
static sml_value_record_t g_sml_value_history[SML_VALUE_HISTORY_COUNT];
static size_t g_sml_value_history_count;
static size_t g_sml_value_history_next;
static uint32_t g_sml_latest_value_sequence;
static uint32_t g_sml_valid_telegrams;
static uint32_t g_sml_invalid_crc_telegrams;
static uint32_t g_sml_transport_gaps;
static uint32_t g_sml_parser_queue_drops;
static portMUX_TYPE g_sml_history_lock = portMUX_INITIALIZER_UNLOCKED;
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
    if (g_send_done != NULL) {
        xSemaphoreGive(g_send_done);
    }
}
#endif

static void store_raw_record(const raw_packet_header_t *header,
                            const uint8_t *raw_data,
                            size_t raw_length)
{
    if (header->raw_length > UART_PACKET_BYTES || raw_length != header->raw_length) {
        ESP_LOGW(TAG, "Raw block length invalid for history: %u",
                 (unsigned int)header->raw_length);
        return;
    }

    portENTER_CRITICAL(&g_raw_history_lock);
    raw_record_t *record = &g_raw_history[g_raw_history_next];
    record->sequence = header->block_sequence;
    record->timestamp_us = header->timestamp_us;
    record->length = (uint16_t)raw_length;
    memcpy(record->data, raw_data, raw_length);
    g_raw_history_next = (g_raw_history_next + 1) % RAW_HISTORY_COUNT;
    if (g_raw_history_count < RAW_HISTORY_COUNT) {
        ++g_raw_history_count;
    }
    g_raw_latest_sequence = header->block_sequence;
    portEXIT_CRITICAL(&g_raw_history_lock);
}

static bool is_espnow_ota_packet(const uint8_t *data, int data_len)
{
    if (data == NULL || data_len < (int)sizeof(espnow_ota_packet_t)) {
        return false;
    }
    espnow_ota_packet_t packet;
    memcpy(&packet, data, sizeof(packet));
    return packet.magic == ESPNOW_OTA_MAGIC && packet.version == ESPNOW_OTA_VERSION;
}

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

    if (data_len >= (int)sizeof(raw_packet_header_t)) {
        raw_packet_header_t raw_header;
        memcpy(&raw_header, data, sizeof(raw_header));
        if (raw_header.magic == RAW_PACKET_MAGIC &&
            raw_header.version == RAW_PACKET_VERSION) {
            size_t fragment_payload_len = (size_t)data_len - sizeof(raw_header);
            if (raw_header.raw_length <= UART_PACKET_BYTES &&
                raw_header.fragment_offset == 0 &&
                raw_header.fragment_length == raw_header.raw_length &&
                fragment_payload_len == raw_header.fragment_length) {
                store_raw_record(&raw_header, data + sizeof(raw_header), fragment_payload_len);
#if !CONFIG_ESPNOW_ROLE_SENDER
                sml_parser_item_t item = {
                    .transport_sequence = raw_header.block_sequence,
                    .timestamp_us = raw_header.timestamp_us,
                    .length = raw_header.raw_length,
                };
                memcpy(item.data, data + sizeof(raw_header), fragment_payload_len);
                if (g_sml_parser_queue == NULL ||
                    xQueueSend(g_sml_parser_queue, &item, 0) != pdTRUE) {
                    portENTER_CRITICAL(&g_sml_history_lock);
                    ++g_sml_parser_queue_drops;
                    portEXIT_CRITICAL(&g_sml_history_lock);
                    ESP_LOGE(TAG, "SML parser queue full; raw record retained but not parsed");
                }
                memcpy(g_espnow_ota_sender_mac, info->src_addr, ESP_NOW_ETH_ALEN);
#endif
                return;
            }

            ESP_LOGW(TAG, "Dropped malformed or fragmented raw packet "
                         "(block=%lu, offset=%u, fragment=%u, total=%u)",
                     (unsigned long)raw_header.block_sequence,
                     (unsigned int)raw_header.fragment_offset,
                     (unsigned int)raw_header.fragment_length,
                     (unsigned int)raw_header.raw_length);
            return;
        }
    }
    ESP_LOGW(TAG, "Dropped non-raw ESP-NOW packet from %02X:%02X:%02X:%02X:%02X:%02X (%d bytes)",
             (unsigned int)info->src_addr[0], (unsigned int)info->src_addr[1],
             (unsigned int)info->src_addr[2], (unsigned int)info->src_addr[3],
             (unsigned int)info->src_addr[4], (unsigned int)info->src_addr[5],
             data_len);
}

#if !CONFIG_ESPNOW_ROLE_SENDER
typedef struct {
    uint8_t type;
    size_t value_length;
    size_t item_count;
    const uint8_t *value;
    const uint8_t *children;
    const uint8_t *next;
} sml_tlv_node_t;

static bool sml_read_tlv(const uint8_t *data, const uint8_t *end,
                         unsigned int depth, sml_tlv_node_t *node)
{
    if (data >= end || depth > 16) {
        return false;
    }

    uint8_t first = *data++;
    uint8_t type = first >> 4;
    size_t encoded_length = first & 0x0f;
    size_t header_length = 1;
    if (encoded_length == 0x0f) {
        encoded_length = 0;
        uint8_t part;
        do {
            if (data >= end || encoded_length > SML_MAX_TELEGRAM_BYTES) {
                return false;
            }
            part = *data++;
            ++header_length;
            encoded_length = (encoded_length << 7) | (part & 0x7f);
        } while ((part & 0x80) != 0);
    }

    node->type = type;
    node->value = data;
    node->value_length = 0;
    node->item_count = 0;
    node->children = NULL;
    node->next = data;

    if (type == 7) {
        node->item_count = encoded_length;
        node->children = data;
        const uint8_t *cursor = data;
        for (size_t i = 0; i < node->item_count; ++i) {
            sml_tlv_node_t child;
            if (!sml_read_tlv(cursor, end, depth + 1, &child) ||
                child.next <= cursor) {
                return false;
            }
            cursor = child.next;
        }
        node->next = cursor;
        return true;
    }

    if (encoded_length < header_length ||
        encoded_length - header_length > (size_t)(end - data)) {
        return false;
    }
    node->value_length = encoded_length - header_length;
    node->next = data + node->value_length;
    return true;
}

static bool sml_list_child(const sml_tlv_node_t *list, size_t index,
                           const uint8_t *end, sml_tlv_node_t *child)
{
    if (list->type != 7 || index >= list->item_count) {
        return false;
    }
    const uint8_t *cursor = list->children;
    for (size_t i = 0; i <= index; ++i) {
        if (!sml_read_tlv(cursor, end, 0, child)) {
            return false;
        }
        cursor = child->next;
    }
    return true;
}

static bool sml_node_unsigned(const sml_tlv_node_t *node, uint64_t *value)
{
    if (node->type != 6 || node->value_length == 0 || node->value_length > 8) {
        return false;
    }
    uint64_t result = 0;
    for (size_t i = 0; i < node->value_length; ++i) {
        result = (result << 8) | node->value[i];
    }
    *value = result;
    return true;
}

static bool sml_node_integer_text(const sml_tlv_node_t *node,
                                  char *text, size_t text_size)
{
    if (node->value_length == 0 || node->value_length > 8 ||
        (node->type != 5 && node->type != 6)) {
        return false;
    }

    uint64_t value = 0;
    for (size_t i = 0; i < node->value_length; ++i) {
        value = (value << 8) | node->value[i];
    }

    int written;
    if (node->type == 5 && (node->value[0] & 0x80) != 0) {
        if (node->value_length < 8) {
            value |= UINT64_MAX << (node->value_length * 8);
        }
        written = snprintf(text, text_size, "%lld", (long long)(int64_t)value);
    } else {
        written = snprintf(text, text_size, "%llu", (unsigned long long)value);
    }
    return written > 0 && (size_t)written < text_size;
}

static uint16_t sml_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) != 0 ? (crc >> 1) ^ 0x8408 : crc >> 1;
        }
    }
    return (uint16_t)~crc;
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

    portENTER_CRITICAL(&g_sml_history_lock);
    record.sequence = ++g_sml_latest_value_sequence;
    g_sml_value_history[g_sml_value_history_next] = record;
    g_sml_value_history_next =
        (g_sml_value_history_next + 1) % SML_VALUE_HISTORY_COUNT;
    if (g_sml_value_history_count < SML_VALUE_HISTORY_COUNT) {
        ++g_sml_value_history_count;
    }
    portEXIT_CRITICAL(&g_sml_history_lock);

    ESP_LOGI(TAG, "SML value: OBIS=%s value=%s scaler=%d unit=%u",
             record.obis, record.value, (int)record.scaler,
             (unsigned int)record.unit);
}

static size_t sml_parse_get_list_response(const uint8_t *end,
                                         const sml_tlv_node_t *response,
                                         uint64_t timestamp_us)
{
    sml_tlv_node_t values;
    if (response->type != 7 ||
        !sml_list_child(response, 4, end, &values) ||
        values.type != 7) {
        return 0;
    }

    size_t decoded = 0;
    const uint8_t *cursor = values.children;
    for (size_t i = 0; i < values.item_count; ++i) {
        sml_tlv_node_t entry;
        if (!sml_read_tlv(cursor, end, 0, &entry) || entry.type != 7) {
            break;
        }
        cursor = entry.next;

        sml_tlv_node_t obis_node;
        sml_tlv_node_t unit_node;
        sml_tlv_node_t scaler_node;
        sml_tlv_node_t value_node;
        uint64_t unit_value;
        if (!sml_list_child(&entry, 0, end, &obis_node) ||
            obis_node.type != 0 || obis_node.value_length != 6 ||
            !sml_list_child(&entry, 3, end, &unit_node) ||
            !sml_node_unsigned(&unit_node, &unit_value) || unit_value > UINT8_MAX ||
            !sml_list_child(&entry, 4, end, &scaler_node) ||
            scaler_node.type != 5 || scaler_node.value_length != 1 ||
            !sml_list_child(&entry, 5, end, &value_node)) {
            continue;
        }

        char value_text[24];
        if (!sml_node_integer_text(&value_node, value_text, sizeof(value_text))) {
            continue;
        }
        int8_t scaler = (int8_t)scaler_node.value[0];
        store_sml_value(timestamp_us, obis_node.value, value_text,
                        scaler, (uint8_t)unit_value);
        ++decoded;
    }
    return decoded;
}

static size_t sml_parse_valid_telegram(const uint8_t *frame, size_t frame_length,
                                       uint64_t timestamp_us)
{
    if (frame_length < 16 || frame[frame_length - 8] != 0x1b ||
        frame[frame_length - 7] != 0x1b ||
        frame[frame_length - 6] != 0x1b ||
        frame[frame_length - 5] != 0x1b ||
        frame[frame_length - 4] != 0x1a) {
        return 0;
    }

    const uint8_t *content = frame + 8;
    const uint8_t *content_end = frame + frame_length - 8;
    size_t decoded = 0;
    while (content < content_end) {
        sml_tlv_node_t message;
        if (!sml_read_tlv(content, content_end, 0, &message) ||
            message.next <= content) {
            ESP_LOGW(TAG, "CRC-valid SML telegram contains malformed TLV data");
            break;
        }
        content = message.next;

        sml_tlv_node_t body;
        sml_tlv_node_t tag_node;
        sml_tlv_node_t response;
        uint64_t tag;
        if (message.type != 7 ||
            !sml_list_child(&message, 3, content_end, &body) ||
            body.type != 7 ||
            !sml_list_child(&body, 0, content_end, &tag_node) ||
            !sml_node_unsigned(&tag_node, &tag) || tag != 0x0701 ||
            !sml_list_child(&body, 1, content_end, &response)) {
            continue;
        }
        decoded += sml_parse_get_list_response(content_end, &response,
                                               timestamp_us);
    }
    return decoded;
}

static void sml_finish_frame(const uint8_t *frame, size_t frame_length,
                             uint64_t timestamp_us)
{
    if (frame_length < 16) {
        return;
    }
    uint16_t expected = (uint16_t)frame[frame_length - 2] |
                        ((uint16_t)frame[frame_length - 1] << 8);
    uint16_t actual = sml_crc16(frame, frame_length - 2);
    if (expected != actual) {
        portENTER_CRITICAL(&g_sml_history_lock);
        ++g_sml_invalid_crc_telegrams;
        portEXIT_CRITICAL(&g_sml_history_lock);
        ESP_LOGW(TAG, "SML telegram CRC invalid: expected=%04X calculated=%04X",
                 expected, actual);
        return;
    }

    portENTER_CRITICAL(&g_sml_history_lock);
    ++g_sml_valid_telegrams;
    portEXIT_CRITICAL(&g_sml_history_lock);
    size_t decoded = sml_parse_valid_telegram(frame, frame_length, timestamp_us);
    ESP_LOGI(TAG, "SML telegram CRC valid; decoded numeric values=%u",
             (unsigned int)decoded);
}

static void sml_parser_worker(void *arg)
{
    (void)arg;
    static const uint8_t start_marker[8] = {
        0x1b, 0x1b, 0x1b, 0x1b, 0x01, 0x01, 0x01, 0x01
    };
    static const uint8_t end_marker[5] = {
        0x1b, 0x1b, 0x1b, 0x1b, 0x1a
    };
    uint8_t frame[SML_MAX_TELEGRAM_BYTES];
    uint8_t start_window[8] = {0};
    size_t frame_length = 0;
    size_t start_window_length = 0;
    uint8_t end_tail_remaining = 0;
    bool in_frame = false;
    uint32_t previous_transport_sequence = 0;
    uint64_t frame_timestamp_us = 0;
    uint64_t last_stats_us = (uint64_t)esp_timer_get_time();
    sml_parser_item_t item;

    while (1) {
        if (xQueueReceive(g_sml_parser_queue, &item, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (previous_transport_sequence != 0 &&
                item.transport_sequence != previous_transport_sequence + 1) {
                in_frame = false;
                frame_length = 0;
                end_tail_remaining = 0;
                start_window_length = 0;
                portENTER_CRITICAL(&g_sml_history_lock);
                ++g_sml_transport_gaps;
                portEXIT_CRITICAL(&g_sml_history_lock);
                ESP_LOGW(TAG, "Raw transport sequence gap; discarded partial SML frame");
            }
            previous_transport_sequence = item.transport_sequence;

            for (size_t i = 0; i < item.length; ++i) {
                uint8_t byte = item.data[i];
                if (!in_frame) {
                    if (start_window_length < sizeof(start_window)) {
                        start_window[start_window_length++] = byte;
                    } else {
                        memmove(start_window, start_window + 1,
                                sizeof(start_window) - 1);
                        start_window[sizeof(start_window) - 1] = byte;
                    }
                    if (start_window_length == sizeof(start_window) &&
                        memcmp(start_window, start_marker, sizeof(start_marker)) == 0) {
                        memcpy(frame, start_marker, sizeof(start_marker));
                        frame_length = sizeof(start_marker);
                        frame_timestamp_us = item.timestamp_us;
                        in_frame = true;
                        end_tail_remaining = 0;
                        start_window_length = 0;
                    }
                    continue;
                }

                if (frame_length >= sizeof(frame)) {
                    ESP_LOGE(TAG, "SML telegram exceeds parser limit; waiting for next start");
                    in_frame = false;
                    frame_length = 0;
                    end_tail_remaining = 0;
                    start_window_length = 0;
                    continue;
                }
                frame[frame_length++] = byte;
                if (end_tail_remaining != 0) {
                    --end_tail_remaining;
                    if (end_tail_remaining == 0) {
                        sml_finish_frame(frame, frame_length, frame_timestamp_us);
                        in_frame = false;
                        frame_length = 0;
                    }
                } else if (frame_length >= sizeof(end_marker) &&
                           memcmp(frame + frame_length - sizeof(end_marker),
                                  end_marker, sizeof(end_marker)) == 0) {
                    end_tail_remaining = 3;
                }
            }
        }

        uint64_t now_us = (uint64_t)esp_timer_get_time();
        if (now_us - last_stats_us >= 10000000ULL) {
            uint32_t valid;
            uint32_t invalid;
            uint32_t gaps;
            uint32_t queue_drops;
            portENTER_CRITICAL(&g_sml_history_lock);
            valid = g_sml_valid_telegrams;
            invalid = g_sml_invalid_crc_telegrams;
            gaps = g_sml_transport_gaps;
            queue_drops = g_sml_parser_queue_drops;
            portEXIT_CRITICAL(&g_sml_history_lock);
            ESP_LOGI(TAG, "SML stats: crc_valid=%lu crc_invalid=%lu "
                          "transport_gaps=%lu parser_queue_drops=%lu",
                     (unsigned long)valid, (unsigned long)invalid,
                     (unsigned long)gaps, (unsigned long)queue_drops);
            last_stats_us = now_us;
        }
    }
}
#endif

#if CONFIG_ESPNOW_ROLE_SENDER
static void log_raw_chunk(const uart_raw_chunk_t *chunk)
{
    printf("%llu us | %llu us | %u | ",
           (unsigned long long)chunk->timestamp_us,
           (unsigned long long)chunk->delta_us,
           (unsigned int)chunk->length);
    for (size_t i = 0; i < chunk->length; ++i) {
        printf("%s%02X", (i == 0) ? "" : " ",
               (unsigned int)chunk->data[i]);
    }
    printf("\n");
    fflush(stdout);
}

static void send_raw_chunk(uint32_t transport_sequence,
                           const uart_raw_chunk_t *chunk)
{
    uint8_t packet[ESP_NOW_MAX_DATA_LEN];
    const size_t fragment_capacity = sizeof(packet) - sizeof(raw_packet_header_t);
    if (fragment_capacity == 0) {
        ESP_LOGE(TAG, "ESP-NOW payload cannot contain raw packet header");
        return;
    }

    size_t offset = 0;
    do {
        size_t fragment_length = chunk->length - offset;
        if (fragment_length > fragment_capacity) {
            fragment_length = fragment_capacity;
        }

        raw_packet_header_t header = {
            .magic = RAW_PACKET_MAGIC,
            .version = RAW_PACKET_VERSION,
            .block_sequence = transport_sequence,
            .timestamp_us = chunk->timestamp_us,
            .raw_length = chunk->length,
            .fragment_offset = (uint16_t)offset,
            .fragment_length = (uint16_t)fragment_length,
        };
        memcpy(packet, &header, sizeof(header));
        memcpy(packet + sizeof(header), chunk->data + offset, fragment_length);

        esp_err_t err = esp_now_send(broadcast_mac, packet,
                                     sizeof(header) + fragment_length);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ESP-NOW raw chunk send failed: %s", esp_err_to_name(err));
            return;
        }
        if (xSemaphoreTake(g_send_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "Timed out waiting for ESP-NOW raw fragment completion");
            return;
        }
        if (g_last_send_status != ESP_NOW_SEND_SUCCESS) {
            ESP_LOGE(TAG, "ESP-NOW did not deliver raw block fragment");
            return;
        }
        offset += fragment_length;
    } while (offset < chunk->length);
}

static void uart_capture_worker(void *arg)
{
    (void)arg;
    uint64_t previous_timestamp_us = 0;
    uint8_t rx_buffer[UART_PACKET_BYTES];

    while (1) {
        int received = uart_read_bytes(UART_PORT_NUM, rx_buffer, sizeof(rx_buffer),
                                       pdMS_TO_TICKS(50));
        if (received <= 0) {
            continue;
        }

        uart_raw_chunk_t chunk = {
            .timestamp_us = (uint64_t)esp_timer_get_time(),
            .length = (uint16_t)received,
        };
        chunk.delta_us = previous_timestamp_us == 0
                             ? 0
                             : chunk.timestamp_us - previous_timestamp_us;
        previous_timestamp_us = chunk.timestamp_us;
        memcpy(chunk.data, rx_buffer, (size_t)received);

        portENTER_CRITICAL(&g_uart_stats_lock);
        g_uart_rx_bytes += (uint64_t)received;
        portEXIT_CRITICAL(&g_uart_stats_lock);

        if (xQueueSend(g_uart_raw_queue, &chunk, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "UART transport queue rejected received data");
        }
    }
}

static void uart_transport_worker(void *arg)
{
    (void)arg;
    uint32_t transport_sequence = 0;
    uart_raw_chunk_t chunk;

    while (1) {
        if (xQueueReceive(g_uart_raw_queue, &chunk, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        log_raw_chunk(&chunk);
        send_raw_chunk(++transport_sequence, &chunk);
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

            portENTER_CRITICAL(&g_uart_stats_lock);
            rx_bytes = g_uart_rx_bytes;
            fifo_overflows = g_uart_fifo_overflows;
            rx_buffer_full = g_uart_rx_buffer_full;
            frame_errors = g_uart_frame_errors;
            parity_errors = g_uart_parity_errors;
            portEXIT_CRITICAL(&g_uart_stats_lock);

            ESP_LOGI(TAG,
                     "UART stats: rx_bytes=%llu fifo_overflows=%lu "
                     "rx_buffer_full=%lu frame_errors=%lu parity_errors=%lu",
                     (unsigned long long)rx_bytes,
                     (unsigned long)fifo_overflows,
                     (unsigned long)rx_buffer_full,
                     (unsigned long)frame_errors,
                     (unsigned long)parity_errors);
            next_stats_us = now_us + 10000000LL;
        }
    }
}
#endif

static esp_err_t root_handler(httpd_req_t *req)
{
    raw_record_t latest = {0};
    bool valid;
    portENTER_CRITICAL(&g_raw_history_lock);
    valid = g_raw_history_count != 0;
    if (valid) {
        size_t latest_index = (g_raw_history_next + RAW_HISTORY_COUNT - 1) % RAW_HISTORY_COUNT;
        latest = g_raw_history[latest_index];
    }
    portEXIT_CRITICAL(&g_raw_history_lock);

    char raw_hex[UART_PACKET_BYTES * 3 + 1] = {0};
    if (valid) {
        size_t used = 0;
        for (size_t i = 0; i < latest.length; ++i) {
            int written = snprintf(raw_hex + used, sizeof(raw_hex) - used,
                                   "%s%02X", (i == 0) ? "" : " ",
                                   (unsigned int)latest.data[i]);
            if (written < 0 || (size_t)written >= sizeof(raw_hex) - used) {
                return ESP_FAIL;
            }
            used += (size_t)written;
        }
    }

    char html[1536];
    int len = snprintf(html, sizeof(html),
                       "%s<p>Last UART block: %s</p>"
                       "<p>Sequence: %lu</p><p>Timestamp: %llu us since sender boot</p>"
                       "<p>Length: %u bytes</p><pre>%s</pre>"
                       "<p>FRITZ!Box: %s</p><nav class=\"nav\">"
                       "<a href=\"/log\">Live raw log</a><a href=\"/api\">JSON API</a>"
                       "<a href=\"/config\">Wi-Fi settings</a><a href=\"/update\">Firmware update</a>"
                       "</nav>%s",
                       METER_HTML_HEADER,
                       valid ? "received" : "waiting",
                       (unsigned long)(valid ? latest.sequence : 0),
                       (unsigned long long)(valid ? latest.timestamp_us : 0),
                       (unsigned int)(valid ? latest.length : 0),
                       valid ? raw_hex : "",
                       g_wifi_connected ? "connected" : "not connected",
                       METER_HTML_FOOTER);

    if (len < 0 || (size_t)len >= sizeof(html)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, len);
}

static esp_err_t json_handler(httpd_req_t *req)
{
    raw_record_t latest = {0};
    bool valid;
    portENTER_CRITICAL(&g_raw_history_lock);
    valid = g_raw_history_count != 0;
    if (valid) {
        size_t latest_index = (g_raw_history_next + RAW_HISTORY_COUNT - 1) % RAW_HISTORY_COUNT;
        latest = g_raw_history[latest_index];
    }
    portEXIT_CRITICAL(&g_raw_history_lock);

    char raw_hex[UART_PACKET_BYTES * 3 + 1] = {0};
    if (valid) {
        size_t used = 0;
        for (size_t i = 0; i < latest.length; ++i) {
            int written = snprintf(raw_hex + used, sizeof(raw_hex) - used,
                                   "%02X%s", (unsigned int)latest.data[i],
                                   (i + 1 < latest.length) ? " " : "");
            if (written < 0 || (size_t)written >= sizeof(raw_hex) - used) {
                return ESP_FAIL;
            }
            used += (size_t)written;
        }
    }

    char payload[512];
    int len = snprintf(payload, sizeof(payload),
                       "{\"raw_valid\":%s,\"block_sequence\":%lu,"
                       "\"timestamp_us\":%llu,\"raw_length\":%u,\"raw_hex\":\"%s\","
                       "\"fritzbox_connected\":%s}",
                       valid ? "true" : "false",
                       (unsigned long)(valid ? latest.sequence : 0),
                       (unsigned long long)(valid ? latest.timestamp_us : 0),
                       (unsigned int)(valid ? latest.length : 0),
                       valid ? raw_hex : "",
                       g_wifi_connected ? "true" : "false");
    if (len < 0 || (size_t)len >= sizeof(payload)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, payload, len);
}

static esp_err_t log_api_handler(httpd_req_t *req)
{
    char query[64];
    char after_text[16] = "0";
    uint32_t after = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "after", after_text, sizeof(after_text)) == ESP_OK) {
            char *end = NULL;
            unsigned long parsed = strtoul(after_text, &end, 10);
            if (end == after_text || *end != '\0' || parsed > UINT32_MAX) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid after cursor");
                return ESP_FAIL;
            }
            after = (uint32_t)parsed;
        }
    }

    raw_record_t records[RAW_API_PAGE_SIZE];
    size_t record_count = 0;
    uint32_t oldest_sequence = 0;
    uint32_t latest_sequence;
    portENTER_CRITICAL(&g_raw_history_lock);
    latest_sequence = g_raw_latest_sequence;
    size_t oldest_index = (g_raw_history_next + RAW_HISTORY_COUNT - g_raw_history_count) %
                          RAW_HISTORY_COUNT;
    if (g_raw_history_count != 0) {
        oldest_sequence = g_raw_history[oldest_index].sequence;
    }
    for (size_t i = 0; i < g_raw_history_count && record_count < RAW_API_PAGE_SIZE; ++i) {
        size_t index = (oldest_index + i) % RAW_HISTORY_COUNT;
        if ((int32_t)(g_raw_history[index].sequence - after) > 0) {
            records[record_count++] = g_raw_history[index];
        }
    }
    portEXIT_CRITICAL(&g_raw_history_lock);

    char header[160];
    bool overflow = oldest_sequence != 0 &&
                    (int32_t)(oldest_sequence - after) > 1;
    int header_len = snprintf(header, sizeof(header),
                              "{\"oldest_sequence\":%lu,\"latest_sequence\":%lu,"
                              "\"overflow\":%s,\"blocks\":[",
                              (unsigned long)oldest_sequence,
                              (unsigned long)latest_sequence,
                              overflow ? "true" : "false");
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send_chunk(req, header, header_len);
    for (size_t i = 0; err == ESP_OK && i < record_count; ++i) {
        char hex[UART_PACKET_BYTES * 3 + 1] = {0};
        size_t used = 0;
        for (size_t j = 0; j < records[i].length; ++j) {
            int written = snprintf(hex + used, sizeof(hex) - used, "%s%02X",
                                   j == 0 ? "" : " ",
                                   (unsigned int)records[i].data[j]);
            if (written < 0 || (size_t)written >= sizeof(hex) - used) {
                return ESP_FAIL;
            }
            used += (size_t)written;
        }
        char item[384];
        int item_len = snprintf(item, sizeof(item),
                                "%s{\"sequence\":%lu,\"timestamp_us\":%llu,"
                                "\"raw_length\":%u,\"raw_hex\":\"%s\"}",
                                i == 0 ? "" : ",",
                                (unsigned long)records[i].sequence,
                                (unsigned long long)records[i].timestamp_us,
                                (unsigned int)records[i].length, hex);
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
    uint32_t parser_queue_drops;
    portENTER_CRITICAL(&g_sml_history_lock);
    latest_sequence = g_sml_latest_value_sequence;
    valid_telegrams = g_sml_valid_telegrams;
    invalid_crc_telegrams = g_sml_invalid_crc_telegrams;
    transport_gaps = g_sml_transport_gaps;
    parser_queue_drops = g_sml_parser_queue_drops;
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
                              "\"parser_queue_drops\":%lu,\"values\":[",
                              (unsigned long)oldest_sequence,
                              (unsigned long)latest_sequence,
                              overflow ? "true" : "false",
                              (unsigned long)valid_telegrams,
                              (unsigned long)invalid_crc_telegrams,
                              (unsigned long)transport_gaps,
                              (unsigned long)parser_queue_drops);
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
#endif

static esp_err_t log_page_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>UART and SML log</title>"
        "<style>body{font-family:monospace;margin:20px;background:#10151f;color:#e5edf7}"
        "pre{white-space:pre-wrap;word-break:break-word}button,a{margin:4px;padding:8px 12px}"
        "a{color:#78d6ff}</style></head><body><h1>UART raw and SML decoded log</h1>"
        "<button id=\"pause\" type=\"button\">Pause display</button>"
        "<button id=\"download-raw\" type=\"button\">Download raw capture</button>"
        "<button id=\"download-sml\" type=\"button\">Download decoded values</button>"
        "<h2>Raw UART bytes</h2><p id=\"raw-status\">Connecting…</p><pre id=\"raw-log\"></pre>"
        "<h2>CRC-valid SML values</h2><p id=\"sml-status\">Waiting for valid telegrams…</p>"
        "<pre id=\"sml-log\"></pre><script>"
        "let rawCursor=0,smlCursor=0,paused=false,rawRendered=0,smlRendered=0;"
        "let rawGap=false,smlGap=false;const rawCaptured=[],smlCaptured=[];"
        "const rawOut=document.getElementById('raw-log'),smlOut=document.getElementById('sml-log');"
        "const rawStatus=document.getElementById('raw-status'),smlStatus=document.getElementById('sml-status');"
        "function render(){if(paused)return;for(;rawRendered<rawCaptured.length;rawRendered++){"
        "const b=rawCaptured[rawRendered];rawOut.textContent+=b.timestamp_us+' us | seq '+b.sequence+' | '+"
        "b.raw_length+' | '+b.raw_hex+'\\n';}for(;smlRendered<smlCaptured.length;smlRendered++){"
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
        "if(rawOut.textContent.length>200000)rawOut.textContent=rawOut.textContent.slice(-150000);"
        "if(smlOut.textContent.length>100000)smlOut.textContent=smlOut.textContent.slice(-75000);}"
        "document.getElementById('pause').onclick=()=>{paused=!paused;"
        "document.getElementById('pause').textContent=paused?'Resume display':'Pause display';render();};"
        "function download(rows,name){if(!rows.length)return false;const lines=rows.map(x=>JSON.stringify(x));"
        "const blob=new Blob([lines.join('\\n')+'\\n'],{type:'application/x-ndjson'});"
        "const url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;"
        "a.download=name+'-'+new Date().toISOString().replace(/[:.]/g,'-')+'.jsonl';"
        "document.body.appendChild(a);a.click();a.remove();"
        "setTimeout(()=>URL.revokeObjectURL(url),1000);return true;}"
        "document.getElementById('download-raw').onclick=()=>{if(!download(rawCaptured,'uart-raw'))"
        "rawStatus.textContent='No raw data captured yet';};"
        "document.getElementById('download-sml').onclick=()=>{if(!download(smlCaptured,'sml-values'))"
        "smlStatus.textContent='No decoded SML values captured yet';};"
        "async function pollRaw(){try{const r=await fetch('/api/log?after='+rawCursor,{cache:'no-store'});"
        "if(!r.ok)throw new Error(r.status);const d=await r.json();if(d.overflow){rawGap=true;"
        "rawOut.textContent+='[GAP: receiver raw history overflowed]\\n';}"
        "for(const b of d.blocks){rawCaptured.push(b);rawCursor=b.sequence;}render();"
        "rawStatus.textContent=(paused?'Display paused; capture continues':'Live')+'; cursor '+rawCursor+"
        "(rawGap?' | GAP DETECTED':'');setTimeout(pollRaw,d.blocks.length?0:50);}"
        "catch(e){rawStatus.textContent='Connection error: '+e;setTimeout(pollRaw,1000);}}"
        "async function pollSml(){try{const r=await fetch('/api/sml?after='+smlCursor,{cache:'no-store'});"
        "if(!r.ok)throw new Error(r.status);const d=await r.json();if(d.overflow){smlGap=true;"
        "smlOut.textContent+='[GAP: decoded-value history overflowed]\\n';}"
        "for(const v of d.values){smlCaptured.push(v);smlCursor=v.sequence;}render();"
        "smlStatus.textContent='CRC valid: '+d.crc_valid+' | CRC invalid: '+d.crc_invalid+"
        "' | transport gaps: '+d.transport_gaps+' | parser queue drops: '+d.parser_queue_drops+"
        "(smlGap?' | VALUE GAP DETECTED':'');setTimeout(pollSml,d.values.length?0:250);}"
        "catch(e){smlStatus.textContent='Connection error: '+e;setTimeout(pollSml,1000);}}"
        "pollRaw();pollSml();</script><p><a href=\"/\">Back</a></p></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, page);
}

static esp_err_t ota_page_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>Firmware update</title>"
        "<style>body{font-family:Arial;margin:24px;background:#10151f;color:#e5edf7}"
        "input,button{display:block;margin:12px 0;padding:8px}</style></head><body>"
        "<h1>Local firmware update</h1><p>Select the matching role firmware. "
        "For the sender, use sender.bin built with OTA partitions. Do not close this page during upload.</p>"
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
    config.max_uri_handlers = 12;
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
    httpd_uri_t log_api = {
        .uri = "/api/log",
        .method = HTTP_GET,
        .handler = log_api_handler,
    };
#if !CONFIG_ESPNOW_ROLE_SENDER
    httpd_uri_t sml_api = {
        .uri = "/api/sml",
        .method = HTTP_GET,
        .handler = sml_api_handler,
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
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &log_api));
#if !CONFIG_ESPNOW_ROLE_SENDER
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &sml_api));
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

    g_uart_raw_queue = xQueueCreate(UART_TRANSPORT_QUEUE_LENGTH,
                                    sizeof(uart_raw_chunk_t));
    if (g_uart_raw_queue == NULL) {
        ESP_LOGE(TAG, "Could not create UART raw transport queue");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    g_send_done = xSemaphoreCreateBinary();
    if (g_send_done == NULL) {
        ESP_LOGE(TAG, "Could not create ESP-NOW send completion semaphore");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_callback));

    ESP_LOGI(TAG, "Sender ready: UART%d RX GPIO=%d at %d baud (8N1, no flow control); "
                  "ESP-NOW channel %u",
             UART_PORT_NUM, UART_RX_GPIO, UART_BAUD_RATE, g_espnow_channel);
    ESP_LOGI(TAG, "UART RX ring buffer: %u bytes; transport queue: %u arbitrary read chunks",
             (unsigned int)UART_BUFFER_SIZE,
             (unsigned int)UART_TRANSPORT_QUEUE_LENGTH);
    ESP_LOGI(TAG, "UART log columns: timestamp_us | read_delta_us | length | raw bytes");

    BaseType_t task_result = xTaskCreate(uart_event_worker, "uart_events",
                                         4096, NULL, 6, NULL);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Could not start UART event/statistics worker");
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    task_result = xTaskCreate(uart_transport_worker, "uart_transport",
                              4096, NULL, 9, NULL);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Could not start UART transport worker");
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
#else
    g_espnow_ota_ack_queue = xQueueCreate(4, sizeof(espnow_ota_ack_t));
    g_espnow_ota_mutex = xSemaphoreCreateMutex();
    if (g_espnow_ota_ack_queue == NULL || g_espnow_ota_mutex == NULL) {
        ESP_LOGE(TAG, "Could not create receiver OTA synchronization objects");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    g_sml_parser_queue = xQueueCreate(SML_PARSER_QUEUE_LENGTH,
                                      sizeof(sml_parser_item_t));
    if (g_sml_parser_queue == NULL) {
        ESP_LOGE(TAG, "Could not create SML parser queue");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    BaseType_t parser_task_result = xTaskCreate(sml_parser_worker, "sml_parser",
                                                8192, NULL, 5, NULL);
    if (parser_task_result != pdPASS) {
        ESP_LOGE(TAG, "Could not start SML parser worker");
        ESP_ERROR_CHECK(ESP_FAIL);
    }
#endif
    ESP_ERROR_CHECK(esp_now_register_recv_cb(receive_callback));

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
