/*
 * ui.h — terminal output for the client (SPEC §7.2).
 *
 * Two threads print (the input thread and the receiver thread), so every line
 * goes through ui_print, which holds stdout_mutex while it writes.
 *
 * The "> " prompt is only shown when stdin is a terminal (so tests can pipe
 * commands in). When a message arrives while the prompt is showing, ui_print
 * clears the prompt line ("\r" + clear-line), prints the message and shows the
 * prompt again.
 */
#ifndef UI_H
#define UI_H

#include <stdbool.h>

/* interactive = isatty(stdin). */
void ui_init(bool interactive);

/* Prints one line (printf-style, without "\n"). Locks: stdout_mutex. */
void ui_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Shows "> " (interactive only) and remembers it is showing. */
void ui_show_prompt(void);

/* The user pressed Enter, so the prompt line is used up. */
void ui_prompt_used(void);

#endif
