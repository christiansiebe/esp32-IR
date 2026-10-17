/* Host tests for the JSON string escaper used by the sender-diagnostics JSON.
 *
 * A control character or a quote in a diagnostic event text used to break the
 * JSON the browser parses ("bad control character in string literal"). These
 * tests pin the escaping rules and the snprintf-like truncation contract. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "json_escape.h"

static int checks;
static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++checks;                                                         \
        if (!(cond)) {                                                    \
            ++failures;                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                 \
    } while (0)

static void expect_escape(const char *in, const char *want)
{
    char out[128];
    size_t need = json_escape(in, out, sizeof(out));
    CHECK(strcmp(out, want) == 0);
    CHECK(need == strlen(want));
}

int main(void)
{
    /* Plain text is copied verbatim. */
    expect_escape("hello", "hello");
    expect_escape("", "");

    /* The two characters that break a JSON string literal. */
    expect_escape("a\"b", "a\\\"b");
    expect_escape("a\\b", "a\\\\b");

    /* Control characters: the browser rejects a raw \n / \r outright. */
    expect_escape("line1\nline2", "line1\\nline2");
    expect_escape("a\rb", "a\\rb");
    expect_escape("a\tb", "a\\tb");
    expect_escape("a\bb", "a\\bb");
    expect_escape("a\fb", "a\\fb");
    expect_escape("a\x01" "b", "a\\u0001b");

    /* The exact kind of text that broke the crash section: an ESP-IDF error
     * string with a trailing newline. */
    expect_escape("ESP_ERR_TIMEOUT\n", "ESP_ERR_TIMEOUT\\n");

    /* UTF-8 is passed through byte-for-byte (no needless escaping). */
    expect_escape("M\xc3\xbcller", "M\xc3\xbcller");

    /* A 40-char event text of only control bytes needs 6x that plus a NUL. */
    {
        char in[41];
        memset(in, '\n', sizeof(in) - 1);
        in[sizeof(in) - 1] = '\0';
        char out[41 * 6 + 1];
        size_t need = json_escape(in, out, sizeof(out));
        CHECK(need == 40 * 2); /* each \n is two output bytes */
        CHECK(strlen(out) == 40 * 2);
    }

    /* Truncation contract: the return value is the full needed length and the
     * destination stays NUL-terminated. */
    {
        char out[4];
        size_t need = json_escape("abcd", out, sizeof(out));
        CHECK(need == 4);
        CHECK(strcmp(out, "abc") == 0);
    }

    /* dst_size == 0 must not write and still reports the needed length. */
    {
        size_t need = json_escape("abc", NULL, 0);
        CHECK(need == 3);
    }

    /* NULL source is treated as empty. */
    {
        char out[8];
        size_t need = json_escape(NULL, out, sizeof(out));
        CHECK(need == 0);
        CHECK(out[0] == '\0');
    }

    if (failures == 0) {
        printf("json_escape: %d checks, 0 failures\n", checks);
    } else {
        printf("json_escape: %d checks, %d FAILURES\n", checks, failures);
    }
    return failures == 0 ? 0 : 1;
}
