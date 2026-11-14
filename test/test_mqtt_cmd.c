/* Host tests for the MQTT remote command channel (reassembly + result payload).
 * No esp-mqtt: the framing arithmetic and the JSON result are the parts that
 * break silently, so they are exercised directly. */
#include <stdio.h>
#include <string.h>

#include "mqtt_cmd.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, msg)                                             \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            ++g_failures;                                            \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                            \
    } while (0)

static void test_rx_single(void)
{
    mqtt_cmd_rx_t rx;
    mqtt_cmd_rx_reset(&rx);
    const char *line = "config";
    mqtt_cmd_rx_result_t r =
        mqtt_cmd_rx_feed(&rx, line, (int)strlen(line), 0, (int)strlen(line));
    CHECK(r == MQTT_CMD_RX_DONE, "single chunk completes");
    CHECK(strcmp(mqtt_cmd_rx_line(&rx), "config") == 0, "line kept");
}

static void test_rx_split(void)
{
    mqtt_cmd_rx_t rx;
    mqtt_cmd_rx_reset(&rx);
    const char *full = "set radio-save 20";
    int total = (int)strlen(full);
    CHECK(mqtt_cmd_rx_feed(&rx, full, 4, 0, total) == MQTT_CMD_RX_INCOMPLETE,
          "first chunk incomplete");
    CHECK(mqtt_cmd_rx_feed(&rx, full + 4, total - 4, 4, total) == MQTT_CMD_RX_DONE,
          "second chunk completes");
    CHECK(strcmp(mqtt_cmd_rx_line(&rx), full) == 0, "split line reassembled");
}

static void test_rx_empty_message(void)
{
    /* A zero-length payload carries no command; it is rejected (and resets the
     * state) rather than run as an empty line. */
    mqtt_cmd_rx_t rx;
    mqtt_cmd_rx_reset(&rx);
    CHECK(mqtt_cmd_rx_feed(&rx, "", 0, 0, 0) == MQTT_CMD_RX_ERROR,
          "total_len 0 rejected");
}

static void test_rx_gap_and_orphan(void)
{
    mqtt_cmd_rx_t rx;
    mqtt_cmd_rx_reset(&rx);
    /* A chunk whose offset is not contiguous is a framing error. */
    CHECK(mqtt_cmd_rx_feed(&rx, "abc", 3, 5, 8) == MQTT_CMD_RX_ERROR,
          "non-zero offset without start rejected");
    /* After the error the state is clean, so a fresh message works. */
    CHECK(mqtt_cmd_rx_feed(&rx, "ok", 2, 0, 2) == MQTT_CMD_RX_DONE,
          "resync after error");
}

static void test_rx_too_long(void)
{
    mqtt_cmd_rx_t rx;
    mqtt_cmd_rx_reset(&rx);
    /* A real buffer of exactly MQTT_CMD_LINE_MAX - 1 bytes fills the line. */
    char maxline[MQTT_CMD_LINE_MAX];
    memset(maxline, 'x', MQTT_CMD_LINE_MAX - 1);
    maxline[MQTT_CMD_LINE_MAX - 1] = '\0';

    /* total_len >= MQTT_CMD_LINE_MAX cannot fit with the terminator. */
    CHECK(mqtt_cmd_rx_feed(&rx, maxline, MQTT_CMD_LINE_MAX - 1, 0,
                           MQTT_CMD_LINE_MAX) == MQTT_CMD_RX_ERROR,
          "over-long line rejected");
    CHECK(mqtt_cmd_rx_feed(&rx, maxline, MQTT_CMD_LINE_MAX - 1, 0,
                           MQTT_CMD_LINE_MAX - 1) == MQTT_CMD_RX_DONE,
          "max-length line accepted");
    CHECK(strlen(mqtt_cmd_rx_line(&rx)) == MQTT_CMD_LINE_MAX - 1,
          "max-length line fully stored");
}

static void test_payload(void)
{
    char out[256];
    CHECK(mqtt_cmd_result_payload("status", true, "ok\n", out, sizeof(out)) == 0,
          "payload builds");
    CHECK(strstr(out, "\"cmd\":\"status\"") != NULL, "cmd field present");
    CHECK(strstr(out, "\"ok\":true") != NULL, "ok true");
    CHECK(strstr(out, "\"out\":\"ok\\n\"") != NULL, "newline escaped");

    CHECK(mqtt_cmd_result_payload("x", false, "unknown command 'x'\n", out,
                                  sizeof(out)) == 0,
          "unknown command payload");
    CHECK(strstr(out, "\"ok\":false") != NULL, "ok false");

    /* Quotes and backslashes must be escaped, or HA/the sender sees bad JSON. */
    CHECK(mqtt_cmd_result_payload("a\"b", true, "c\\d", out, sizeof(out)) == 0,
          "quote/backslash payload");
    CHECK(strstr(out, "a\\\"b") != NULL, "quote escaped");
    CHECK(strstr(out, "c\\\\d") != NULL, "backslash escaped");
}

static void test_payload_truncates_safely(void)
{
    /* A small buffer must fail cleanly (empty), never emit broken JSON. */
    char small[16];
    CHECK(mqtt_cmd_result_payload("status", true, "some output", small,
                                  sizeof(small)) == -1,
          "too-small buffer rejected");
    CHECK(small[0] == '\0', "buffer left empty on overflow");
}

int main(void)
{
    test_rx_single();
    test_rx_split();
    test_rx_empty_message();
    test_rx_gap_and_orphan();
    test_rx_too_long();
    test_payload();
    test_payload_truncates_safely();

    printf("test_mqtt_cmd: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
