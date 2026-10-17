/* Host tests for the receiver idle light-sleep gate (main/receiver_sleep.h).
 *
 * The gate decides when the mains-powered receiver may park the CPU. The rules
 * that matter: never sleep during the boot grace window, never sleep while the
 * dashboard was active recently, and sleep again once it has been idle long
 * enough. Getting the grace window wrong would park the CPU before Wi-Fi/HTTP
 * are up. */
#include <stdio.h>

#include "receiver_sleep.h"

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

static void test_grace_window(void)
{
    /* During the grace window the receiver must never sleep, even if the
     * activity stamp is stale (e.g. it was just set at boot). */
    uint64_t activity = 0;
    CHECK(!receiver_sleep_should_sleep(1000, 1000, activity,
                                       RECEIVER_SLEEP_GRACE_MS,
                                       RECEIVER_SLEEP_IDLE_MS),
          "no sleep inside the boot grace window");
    CHECK(!receiver_sleep_should_sleep(RECEIVER_SLEEP_GRACE_MS - 1,
                                       RECEIVER_SLEEP_GRACE_MS - 1, activity,
                                       RECEIVER_SLEEP_GRACE_MS,
                                       RECEIVER_SLEEP_IDLE_MS),
          "no sleep just before the grace window ends");
}

static void test_idle_transition(void)
{
    uint64_t activity = 0; /* last activity at boot */
    uint64_t uptime = RECEIVER_SLEEP_GRACE_MS + 1;
    /* Past the grace window but the dashboard was active less than the idle
     * window ago: stay awake. */
    CHECK(!receiver_sleep_should_sleep(uptime, uptime, activity,
                                       RECEIVER_SLEEP_GRACE_MS,
                                       RECEIVER_SLEEP_IDLE_MS),
          "awake shortly after activity");
    /* Idle for exactly the window: allowed to sleep. */
    CHECK(receiver_sleep_should_sleep(RECEIVER_SLEEP_IDLE_MS, uptime, 0,
                                      RECEIVER_SLEEP_GRACE_MS,
                                      RECEIVER_SLEEP_IDLE_MS),
          "sleep at the idle threshold");
    /* Idle well past the window: sleep. */
    CHECK(receiver_sleep_should_sleep(10 * RECEIVER_SLEEP_IDLE_MS, uptime, 0,
                                      RECEIVER_SLEEP_GRACE_MS,
                                      RECEIVER_SLEEP_IDLE_MS),
          "sleep when long idle");
}

static void test_activity_resets(void)
{
    uint64_t uptime = RECEIVER_SLEEP_GRACE_MS + 1;
    /* A fresh HTTP/ESP-NOW activity stamp right at "now" pushes the sleep back. */
    uint64_t now = 5 * RECEIVER_SLEEP_IDLE_MS;
    CHECK(!receiver_sleep_should_sleep(now, uptime, now,
                                       RECEIVER_SLEEP_GRACE_MS,
                                       RECEIVER_SLEEP_IDLE_MS),
          "activity at now keeps the CPU awake");
    CHECK(!receiver_sleep_should_sleep(now + RECEIVER_SLEEP_IDLE_MS - 1, uptime,
                                       now, RECEIVER_SLEEP_GRACE_MS,
                                       RECEIVER_SLEEP_IDLE_MS),
          "awake until the idle window has fully elapsed");
    CHECK(receiver_sleep_should_sleep(now + RECEIVER_SLEEP_IDLE_MS, uptime, now,
                                      RECEIVER_SLEEP_GRACE_MS,
                                      RECEIVER_SLEEP_IDLE_MS),
          "sleep once the idle window has elapsed again");
}

static void test_clock_goes_backwards(void)
{
    /* A clock that jumped backwards must not be read as "idle forever". */
    CHECK(!receiver_sleep_should_sleep(100, RECEIVER_SLEEP_GRACE_MS + 1, 500,
                                       RECEIVER_SLEEP_GRACE_MS,
                                       RECEIVER_SLEEP_IDLE_MS),
          "backwards clock is not idle");
}

int main(void)
{
    test_grace_window();
    test_idle_transition();
    test_activity_resets();
    test_clock_goes_backwards();

    printf("receiver_sleep: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
