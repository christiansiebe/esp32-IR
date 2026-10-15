#include "web_console.h"

#include <stdio.h>
#include <string.h>

#define WEB_CONSOLE_MAX_COMMANDS 16
#define WEB_CONSOLE_MAX_LINE 192

typedef struct {
    const char *name;
    const char *help;
    web_console_handler_t handler;
} web_console_entry_t;

static web_console_entry_t g_commands[WEB_CONSOLE_MAX_COMMANDS];
static size_t g_command_count;

void web_console_register(const char *name, const char *help,
                          web_console_handler_t handler)
{
    if (name == NULL || handler == NULL || name[0] == '\0') {
        return;
    }
    for (size_t i = 0; i < g_command_count; ++i) {
        if (strcmp(g_commands[i].name, name) == 0) {
            g_commands[i].help = help;
            g_commands[i].handler = handler;
            return;
        }
    }
    if (g_command_count >= WEB_CONSOLE_MAX_COMMANDS) {
        return;
    }
    g_commands[g_command_count].name = name;
    g_commands[g_command_count].help = help;
    g_commands[g_command_count].handler = handler;
    ++g_command_count;
}

void web_console_help(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < g_command_count; ++i) {
        int written = snprintf(out + used, out_size - used, "%-14s %s\n",
                               g_commands[i].name,
                               g_commands[i].help ? g_commands[i].help : "");
        if (written < 0 || (size_t)written >= out_size - used) {
            break;
        }
        used += (size_t)written;
    }
}

static const web_console_entry_t *find_command(const char *name)
{
    for (size_t i = 0; i < g_command_count; ++i) {
        if (strcmp(g_commands[i].name, name) == 0) {
            return &g_commands[i];
        }
    }
    return NULL;
}

bool web_console_run(const char *line, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (line == NULL) {
        return false;
    }

    char buffer[WEB_CONSOLE_MAX_LINE];
    size_t length = 0;
    bool last_space = true; /* collapse leading whitespace away */
    for (size_t i = 0; line[i] != '\0' && length < sizeof(buffer) - 1; ++i) {
        char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (last_space) {
                continue;
            }
            buffer[length++] = ' ';
            last_space = true;
        } else {
            buffer[length++] = c;
            last_space = false;
        }
    }
    while (length > 0 && buffer[length - 1] == ' ') {
        --length;
    }
    buffer[length] = '\0';

    char *cursor = buffer;
    if (*cursor == '\0') {
        return false;
    }

    char *name = cursor;
    while (*cursor != '\0' && *cursor != ' ') {
        ++cursor;
    }
    if (*cursor == ' ') {
        *cursor = '\0';
        ++cursor;
    }
    const char *args = cursor;

    const web_console_entry_t *entry = find_command(name);
    if (entry == NULL) {
        snprintf(out, out_size, "unknown command '%s' - type 'help'\n", name);
        return false;
    }
    entry->handler(args, out, out_size);
    return true;
}
