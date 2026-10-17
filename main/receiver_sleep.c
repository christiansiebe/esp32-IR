#include "receiver_sleep.h"

#ifdef ESP_PLATFORM

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "soc/soc_caps.h"

#define TAG "recv_sleep"

#if SOC_LIGHT_SLEEP_SUPPORTED

static bool s_enabled;
static bool s_locked = true; /* NO_LIGHT_SLEEP is held by default (awake) */
static esp_pm_lock_handle_t s_lock;
static volatile int64_t s_last_activity_us;
static volatile bool s_sleeping;

/* Peer is loopback (127.0.0.0/8): the web-health self-probe, which must not
 * count as activity or the guard keeps the receiver awake forever. */
static bool peer_is_loopback(const struct sockaddr *addr)
{
    if (addr == NULL) {
        return false;
    }
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
        return (ntohl(in->sin_addr.s_addr) >> 24) == 127u;
    }
#if LWIP_IPV6
    if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)addr;
        if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
            return in6->sin6_addr.s6_addr[12] == 127u;
        }
        return IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr);
    }
#endif
    return false;
}

/* httpd open_fn runs on the httpd task for every new connection. Note real
 * (non-loopback) activity first, then let the connection proceed (ESP_OK). */
static esp_err_t receiver_sleep_http_open_cb(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getpeername(sockfd, (struct sockaddr *)&addr, &len) == 0 &&
        !peer_is_loopback((const struct sockaddr *)&addr)) {
        receiver_sleep_note_http();
    }
    return ESP_OK;
}

static void receiver_sleep_lock(bool lock)
{
    if (s_lock == NULL || lock == s_locked) {
        return;
    }
    if (lock) {
        esp_pm_lock_acquire(s_lock);
    } else {
        esp_pm_lock_release(s_lock);
    }
    s_locked = lock;
}

void receiver_sleep_init(bool enabled)
{
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "recv_sleep", &s_lock) != ESP_OK) {
        ESP_LOGW(TAG, "Could not create the light-sleep lock; staying awake");
        s_enabled = false;
        return;
    }
    s_enabled = enabled;
    s_last_activity_us = esp_timer_get_time();
    /* Start awake; the tick releases the lock once the idle window has passed. */
    esp_pm_lock_acquire(s_lock);
    s_locked = true;
    ESP_LOGI(TAG, "receiver idle sleep %s", enabled ? "enabled" : "disabled");
}

void receiver_sleep_note_http(void)
{
    s_last_activity_us = esp_timer_get_time();
}

void receiver_sleep_note_espnow(void)
{
    s_last_activity_us = esp_timer_get_time();
}

void receiver_sleep_tick(void)
{
    if (s_lock == NULL) {
        return;
    }
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    uint64_t last_ms = (uint64_t)(s_last_activity_us / 1000);
    /* esp_timer_get_time() is the uptime, so the grace window is measured from
     * boot with the same clock as the activity stamp. */
    bool may = s_enabled && receiver_sleep_should_sleep(
                                now_ms, now_ms, last_ms,
                                RECEIVER_SLEEP_GRACE_MS, RECEIVER_SLEEP_IDLE_MS);
    receiver_sleep_lock(!may);
    s_sleeping = may;
}

bool receiver_sleep_enabled(void)
{
    return s_enabled;
}

bool receiver_sleep_is_sleeping(void)
{
    return s_sleeping;
}

uint32_t receiver_sleep_idle_age_ms(void)
{
    int64_t now_us = esp_timer_get_time();
    int64_t last_us = s_last_activity_us;
    if (now_us < last_us) {
        return 0;
    }
    return (uint32_t)((now_us - last_us) / 1000);
}

/* httpd open_fn installed on the receiver's server. */
esp_err_t receiver_sleep_http_open(httpd_handle_t hd, int sockfd)
{
    return receiver_sleep_http_open_cb(hd, sockfd);
}

#else /* SOC_LIGHT_SLEEP_SUPPORTED */

void receiver_sleep_init(bool enabled) { (void)enabled; }
void receiver_sleep_note_http(void) {}
void receiver_sleep_note_espnow(void) {}
void receiver_sleep_tick(void) {}
bool receiver_sleep_enabled(void) { return false; }
bool receiver_sleep_is_sleeping(void) { return false; }
uint32_t receiver_sleep_idle_age_ms(void) { return 0; }
esp_err_t receiver_sleep_http_open(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    (void)sockfd;
    return ESP_OK;
}

#endif /* SOC_LIGHT_SLEEP_SUPPORTED */

#else /* host build: no power management to apply */

void receiver_sleep_init(bool enabled) { (void)enabled; }
void receiver_sleep_note_http(void) {}
void receiver_sleep_note_espnow(void) {}
void receiver_sleep_tick(void) {}
bool receiver_sleep_enabled(void) { return false; }
bool receiver_sleep_is_sleeping(void) { return false; }
uint32_t receiver_sleep_idle_age_ms(void) { return 0; }

#endif /* ESP_PLATFORM */
