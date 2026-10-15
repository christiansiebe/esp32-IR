/* Host tests for the web debug console dispatcher (main/web_console.c).
 *
 * The dispatcher is deliberately free of ESP-IDF so the parsing, command
 * lookup and error handling can be checked here. Device-specific commands are
 * registered from main.c and are only exercised on hardware. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "web_console.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        ++g_checks;                                                 \
        if (!(cond)) {                                              \
            ++g_failures;                                           \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);\
        }                                                           \
    } while (0)

static char g_last_args[64];
static int g_calls;

static void echo_handler(const char *args, char *out, size_t out_size)
{
    ++g_calls;
    snprintf(g_last_args, sizeof(g_last_args), "%s", args);
    snprintf(out, out_size, "echo:%s", args);
}

static void empty_handler(const char *args, char *out, size_t out_size)
{
    (void)args;
    ++g_calls;
    snprintf(out, out_size, "ok");
}

static void test_registration_and_lookup(void)
{
    printf("test_registration_and_lookup\n");
    char out[128];
    web_console_register("echo", "repeat the arguments", echo_handler);
    web_console_register("noop", "does nothing", empty_handler);

    g_calls = 0;
    CHECK(web_console_run("echo hello world", out, sizeof(out)),
          "known command reports success");
    CHECK(strcmp(out, "echo:hello world") == 0, "arguments are passed through");
    CHECK(strcmp(g_last_args, "hello world") == 0, "handler sees raw arguments");

    g_calls = 0;
    CHECK(web_console_run("noop", out, sizeof(out)),
          "command without arguments runs");
    CHECK(strcmp(out, "ok") == 0, "noop output");
    CHECK(g_calls == 1, "noop handler invoked exactly once");
}

static void test_unknown_and_empty(void)
{
    printf("test_unknown_and_empty\n");
    char out[128];
    CHECK(!web_console_run("frobnicate", out, sizeof(out)),
          "unknown command returns false");
    CHECK(strstr(out, "unknown command") != NULL, "unknown command hint");

    CHECK(!web_console_run("", out, sizeof(out)), "empty line returns false");
    CHECK(!web_console_run("    ", out, sizeof(out)), "blank line returns false");
    CHECK(!web_console_run("\r\n", out, sizeof(out)), "CRLF line returns false");
}

static void test_whitespace_normalisation(void)
{
    printf("test_whitespace_normalisation\n");
    char out[128];
    CHECK(web_console_run("  echo \t a\r\nb ", out, sizeof(out)),
          "leading/trailing whitespace tolerated");
    CHECK(strcmp(out, "echo:a b") == 0, "tabs and newlines become single spaces");
}

static void test_help_lists_commands(void)
{
    printf("test_help_lists_commands\n");
    char out[256];
    web_console_help(out, sizeof(out));
    CHECK(strstr(out, "echo") != NULL, "help lists registered command");
    CHECK(strstr(out, "noop") != NULL, "help lists second command");
    CHECK(strstr(out, "repeat the arguments") != NULL, "help shows description");
}

static void test_re_registration(void)
{
    printf("test_re_registration\n");
    char out[128];
    web_console_register("echo", "updated", echo_handler);
    web_console_register("echo", "updated", empty_handler);
    g_calls = 0;
    CHECK(web_console_run("echo", out, sizeof(out)), "re-registered command runs");
    CHECK(strcmp(out, "ok") == 0, "latest handler wins");
}

static void test_truncation_is_safe(void)
{
    printf("test_truncation_is_safe\n");
    char out[8];
    CHECK(web_console_run("echo 0123456789", out, sizeof(out)),
          "run with tiny output buffer");
    CHECK(strlen(out) < sizeof(out), "output stays NUL terminated");
}

int main(void)
{
    test_registration_and_lookup();
    test_unknown_and_empty();
    test_whitespace_normalisation();
    test_help_lists_commands();
    test_re_registration();
    test_truncation_is_safe();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
