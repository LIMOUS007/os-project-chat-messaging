/*
 * ui.c — serialised terminal output. See ui.h.
 */
#include "ui.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>

/* stdout_mutex: the input thread and the receiver thread both print; this
 * keeps lines whole and the prompt state consistent. */
static pthread_mutex_t stdout_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool is_interactive = false;
static bool prompt_showing = false;

void ui_init(bool interactive) { is_interactive = interactive; }

void ui_print(const char *fmt, ...)
{
    pthread_mutex_lock(&stdout_mutex);
    if (is_interactive && prompt_showing)
        fputs("\r\033[K", stdout);           /* erase the "> " line */
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    if (is_interactive && prompt_showing)
        fputs("> ", stdout);                 /* put the prompt back */
    fflush(stdout);
    pthread_mutex_unlock(&stdout_mutex);
}

void ui_show_prompt(void)
{
    pthread_mutex_lock(&stdout_mutex);
    if (is_interactive) {
        fputs("> ", stdout);
        fflush(stdout);
        prompt_showing = true;
    }
    pthread_mutex_unlock(&stdout_mutex);
}

void ui_prompt_used(void)
{
    pthread_mutex_lock(&stdout_mutex);
    prompt_showing = false;
    pthread_mutex_unlock(&stdout_mutex);
}
