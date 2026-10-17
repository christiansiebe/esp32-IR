#include "web_guard.h"

#include <stdio.h>
#include <string.h>

void web_guard_default_policy(web_guard_policy_t *policy)
{
    policy->threshold_ms = 30000;
    policy->restart_timeout_ms = 15000;
    policy->reboot_timeout_ms = 90000;
    policy->min_uptime_ms = 45000;
}

void web_guard_init(web_guard_t *guard, const web_guard_policy_t *policy,
                    uint64_t now_ms)
{
    memset(guard, 0, sizeof(*guard));
    guard->policy = *policy;
    guard->started_at_ms = now_ms;
    guard->last_request_ms = now_ms;
    guard->last_probe_ms = now_ms;
}

void web_guard_note_request(web_guard_t *guard, uint64_t now_ms)
{
    guard->last_request_ms = now_ms;
}

void web_guard_note_probe(web_guard_t *guard, uint64_t now_ms, bool ok)
{
    if (ok) {
        guard->probe_works = true;
        guard->last_probe_ms = now_ms;
    }
}

void web_guard_note_restart_attempt(web_guard_t *guard, uint64_t now_ms, bool ok)
{
    if (guard->first_attempt_ms == 0) {
        guard->first_attempt_ms = now_ms;
    }
    guard->last_attempt_ms = now_ms;
    if (ok) {
        /* A fresh server has served nothing yet; treat it as healthy now. */
        guard->restarts++;
        guard->started_at_ms = now_ms;
        guard->last_request_ms = now_ms;
        guard->last_probe_ms = now_ms;
        guard->dead_since_ms = 0;
        guard->first_attempt_ms = 0;
        guard->dead = false;
    }
}

bool web_guard_is_active(const web_guard_t *guard)
{
    return guard->probe_works;
}

uint64_t web_guard_last_activity_ms(const web_guard_t *guard)
{
    return guard->last_request_ms > guard->last_probe_ms
               ? guard->last_request_ms
               : guard->last_probe_ms;
}

web_guard_action_t web_guard_tick(web_guard_t *guard, uint64_t now_ms)
{
    /* Never act before the self-probe path has proven itself once: a device
     * whose loopback cannot be probed must not be restarted/rebooted. */
    if (!guard->probe_works) {
        return WEB_GUARD_OK;
    }
    if (now_ms - guard->started_at_ms < guard->policy.min_uptime_ms) {
        return WEB_GUARD_OK;
    }

    uint64_t last = web_guard_last_activity_ms(guard);
    if (now_ms - last <= guard->policy.threshold_ms) {
        guard->dead_since_ms = 0;
        guard->dead = false;
        return WEB_GUARD_OK;
    }

    if (guard->dead_since_ms == 0) {
        /* Start the escalation clock at detection, not at the last activity:
         * before the uptime grace we could not act anyway, so the reboot
         * window must not be "pre-spent" by the grace period. */
        guard->dead_since_ms = now_ms;
    }
    guard->dead = true;

    /* Restart while the silence is long enough; if a restart has not brought
     * the dashboard back within the reboot window (measured from the first
     * restart attempt of this outage), escalate to a reboot. */
    uint64_t attempt_age = guard->first_attempt_ms != 0
                               ? now_ms - guard->first_attempt_ms
                               : 0;
    if (guard->first_attempt_ms != 0 &&
        attempt_age >= guard->policy.reboot_timeout_ms &&
        now_ms - guard->last_attempt_ms >= guard->policy.restart_timeout_ms) {
        return WEB_GUARD_REBOOT;
    }
    if (now_ms - last >= guard->policy.threshold_ms &&
        now_ms - guard->last_attempt_ms >= guard->policy.restart_timeout_ms) {
        return WEB_GUARD_RESTART;
    }
    return WEB_GUARD_OK;
}

#ifdef ESP_PLATFORM

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web_guard";

static web_guard_t s_guard;
static httpd_handle_t *s_server;
static httpd_config_t s_config;
static void (*s_register_handlers)(httpd_handle_t);

/* One cheap round-trip to our own server. The status code is irrelevant; a
 * completed connection+request proves the httpd task still answers. */
static bool web_guard_probe(void)
{
    char url[48];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/healthz",
             (unsigned int)s_config.server_port);

    esp_http_client_config_t cfg = {0};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 3000;
    cfg.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGW(TAG, "self-probe: client init failed");
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "self-probe failed: %s (http %d)", esp_err_to_name(err), status);
    }
    return err == ESP_OK;
}

static bool web_guard_restart_server(void)
{
    ESP_LOGW(TAG, "dashboard silent: restarting the HTTP server");
    if (*s_server != NULL) {
        httpd_stop(*s_server);
        *s_server = NULL;
    }
    esp_err_t err = httpd_start(s_server, &s_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return false;
    }
    if (s_register_handlers != NULL) {
        s_register_handlers(*s_server);
    }
    ESP_LOGW(TAG, "HTTP server restarted (%u)", (unsigned int)s_guard.restarts + 1);
    return true;
}

static void web_guard_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
        web_guard_note_probe(&s_guard, now_ms, web_guard_probe());

        web_guard_action_t action = web_guard_tick(&s_guard, now_ms);
        if (action == WEB_GUARD_RESTART) {
            bool ok = web_guard_restart_server();
            web_guard_note_restart_attempt(&s_guard, now_ms, ok);
        } else if (action == WEB_GUARD_REBOOT) {
            ESP_LOGE(TAG, "dashboard unreachable for %llu ms: rebooting",
                     (unsigned long long)(now_ms - s_guard.dead_since_ms));
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
    }
}

static esp_err_t web_guard_healthz_handler(httpd_req_t *req)
{
    web_guard_note_request(&s_guard,
                           (uint64_t)(esp_timer_get_time() / 1000));
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t web_guard_register_healthz(httpd_handle_t server)
{
    httpd_uri_t healthz = {
        .uri = "/healthz",
        .method = HTTP_GET,
        .handler = web_guard_healthz_handler,
    };
    return httpd_register_uri_handler(server, &healthz);
}

bool web_guard_active(void)
{
    return web_guard_is_active(&s_guard);
}

uint32_t web_guard_restart_count(void)
{
    return s_guard.restarts;
}

uint32_t web_guard_last_activity_age_ms(void)
{
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    uint64_t last = web_guard_last_activity_ms(&s_guard);
    if (now_ms < last) {
        return 0;
    }
    return (uint32_t)(now_ms - last);
}

void web_guard_start(httpd_handle_t *server, const httpd_config_t *config,
                     void (*register_handlers)(httpd_handle_t))
{
    s_server = server;
    s_config = *config;
    s_register_handlers = register_handlers;
    web_guard_policy_t policy;
    web_guard_default_policy(&policy);
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    web_guard_init(&s_guard, &policy, now_ms);
    if (xTaskCreate(web_guard_task, "web_guard", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "could not start the web-health guard");
    }
}

#endif /* ESP_PLATFORM */

