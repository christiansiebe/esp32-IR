/* Tiny command dispatcher behind the web debug console.
 *
 * The parsing/lookup core has no ESP-IDF dependency so it can be unit tested on
 * the host. The actual commands are registered from main.c where the device
 * state lives. */
#ifndef WEB_CONSOLE_H
#define WEB_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>

/* A command writes its human-readable result into out (always NUL terminated
 * within out_size). args is the remainder of the line after the command name
 * (may be empty, never NULL). */
typedef void (*web_console_handler_t)(const char *args, char *out, size_t out_size);

void web_console_register(const char *name, const char *help,
                          web_console_handler_t handler);

/* Parse one input line, look up the command and run it.
 * Returns true when a known command was executed, false for an unknown one
 * (out then holds an error hint). */
bool web_console_run(const char *line, char *out, size_t out_size);

/* Render the registered command list (name + help) into out. */
void web_console_help(char *out, size_t out_size);

#endif /* WEB_CONSOLE_H */
