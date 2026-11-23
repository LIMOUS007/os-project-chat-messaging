/*
 * client/main.c — the input thread (SPEC §7).
 *
 *   ./client          then:  connect <server IP>[:port] <user>
 *
 * The main thread reads commands from stdin, sends one at a time and waits
 * (up to 5 s) for the receiver thread to say the reply arrived. It polls stdin
 * and a "notify" pipe; the receiver and the Ctrl+C handler write to the pipe.
 *
 * The client writes nothing to disk (R14).
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "commands.h"
#include "net.h"
#include "receiver.h"
#include "ui.h"

#define REPLY_TIMEOUT_MS 5000
#define LINE_MAX_BYTES   4096

static int  notify_pipe[2];
static int  server_fd = -1;
static bool connected = false;

/* Ctrl+C: only async-signal-safe work here — write one byte to the pipe.
 * The input thread then shuts down exactly like the "shutdown" command. */
static void on_sigint(int sig)
{
    (void)sig;
    char q = 'Q';
    ssize_t ignored = write(notify_pipe[1], &q, 1);
    (void)ignored;
}

/* Waits for one byte from the notify pipe. Returns it, or 0 on timeout. */
static char wait_notify(int timeout_ms)
{
    struct pollfd pfd = { .fd = notify_pipe[0], .events = POLLIN };
    for (;;) {
        int rc = poll(&pfd, 1, timeout_ms);
        if (rc < 0 && errno == EINTR)
            continue;               /* Ctrl+C: its 'Q' byte is in the pipe now */
        if (rc <= 0)
            return 0;
        char byte;
        if (read(notify_pipe[0], &byte, 1) == 1)
            return byte;
    }
}

/* Drops stale bytes from the notify pipe (it is non-blocking for reads). */
static void drain_notify(void)
{
    char buf[64];
    while (read(notify_pipe[0], buf, sizeof buf) > 0) {
    }
}

/* Closes the connection, always in this order (Rev 3.1 errata [D]):
 * shutdown() wakes the receiver from recv, join it, then close the fd, then
 * forget the waiting command and stale notify bytes. */
static void disconnect(void)
{
    if (server_fd < 0)
        return;
    receiver_stopping();
    shutdown(server_fd, SHUT_RDWR);
    receiver_join();
    close(server_fd);
    server_fd = -1;
    connected = false;
    pending_clear();
    drain_notify();
}

/* Leaves the program. If the server ended the session (it printed "Server
 * shutting down..." or "Connection to server lost."), we exit quietly. */
static void quit(int code)
{
    bool server_ended = server_fd >= 0 && receiver_server_closed();
    if (connected && !server_ended) {
        frame_builder_t fb;
        fb_begin(&fb, T_LOGOUT);
        fb_finish(&fb);
        client_send(&fb);
    }
    disconnect();
    if (!server_ended)
        ui_print("Disconnected.");
    exit(code);
}

static void do_connect(const cmd_t *cmd)
{
    server_fd = tcp_connect(cmd->host, cmd->port);
    if (server_fd < 0) {
        ui_print("Error: cannot connect to %s:%d", cmd->host, cmd->port);
        exit(1);
    }
    pending_set(cmd);                       /* before the receiver can see a reply */
    receiver_start(server_fd, cmd->name);

    frame_builder_t fb;
    cmd_encode(cmd, &fb);
    client_send(&fb);

    char result = wait_notify(REPLY_TIMEOUT_MS);
    if (result == 'R') {
        connected = true;
        return;
    }
    if (result == 0)
        ui_print("No response from server.");
    disconnect();                           /* login failed: print was done, exit 1 */
    exit(1);
}

/* Sends a command and waits for its reply. */
static void do_remote_command(const cmd_t *cmd)
{
    frame_builder_t fb;
    if (!cmd_encode(cmd, &fb)) {
        ui_print("Error: command too long");
        return;
    }
    pending_set(cmd);
    client_send(&fb);   /* if it fails, the receiver sees EOF and sends 'Q' */

    char result = wait_notify(REPLY_TIMEOUT_MS);
    if (result == 'Q')
        quit(0);
    if (result == 0) {
        /* No request ids in the protocol: a late reply could be mistaken for
         * the next command's reply, so we disconnect (Rev 3.1 errata [D]). */
        ui_print("No response from server. Disconnecting.");
        disconnect();
    }
}

static void handle_line(const char *line)
{
    cmd_t cmd;
    parse_line(line, &cmd);
    switch (cmd.type) {
    case CMD_EMPTY:
        return;
    case CMD_INVALID:
        ui_print("%s", cmd.error);
        return;
    case CMD_HELP:
        ui_print("%s", help_text());
        return;
    case CMD_SHUTDOWN:
        quit(0);
        return;
    case CMD_CONNECT:
        if (connected)
            ui_print("Already connected. Use shutdown first.");
        else
            do_connect(&cmd);
        return;
    default:
        if (!connected)
            ui_print("Not connected. Use: connect <server IP> <user>");
        else
            do_remote_command(&cmd);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);       /* line-buffered, even into a pipe/file */
    ui_init(isatty(STDIN_FILENO));

    if (pipe(notify_pipe) < 0) {
        perror("pipe");
        return 1;
    }
    set_nonblocking(notify_pipe[0]);
    receiver_init(notify_pipe[1]);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (isatty(STDIN_FILENO))
        ui_print("Chat client. Type 'help' for commands, then: connect <server IP> <user>");

    /* Our own line reader on top of read(): stdio buffering would hide lines
     * from poll(). */
    char buf[LINE_MAX_BYTES];
    size_t have = 0;
    for (;;) {
        ui_show_prompt();
        struct pollfd pfds[2] = {
            { .fd = STDIN_FILENO, .events = POLLIN },
            { .fd = notify_pipe[0], .events = POLLIN },
        };
        if (poll(pfds, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pfds[1].revents & POLLIN) {
            char byte = wait_notify(0);
            if (byte == 'Q')
                quit(0);                    /* Ctrl+C, or the server closed */
            continue;                       /* a stale 'R'/'E': ignore */
        }
        if (!(pfds[0].revents & (POLLIN | POLLHUP)))
            continue;

        ssize_t n = read(STDIN_FILENO, buf + have, sizeof buf - 1 - have);
        if (n <= 0) {                       /* end of input behaves like shutdown */
            if (have > 0) {                 /* last line had no newline */
                buf[have] = '\0';
                ui_prompt_used();
                handle_line(buf);
            }
            quit(0);
        }
        have += (size_t)n;

        /* Handle every complete line; keep a partial last line. */
        size_t start = 0;
        for (size_t i = 0; i < have; i++) {
            if (buf[i] != '\n')
                continue;
            buf[i] = '\0';
            if (i > start && buf[i - 1] == '\r')
                buf[i - 1] = '\0';
            ui_prompt_used();
            handle_line(buf + start);
            start = i + 1;
        }
        memmove(buf, buf + start, have - start);
        have -= start;
        if (have == sizeof buf - 1) {       /* a line longer than the buffer */
            buf[have] = '\0';
            ui_prompt_used();
            handle_line(buf);
            have = 0;
        }
    }
    quit(0);
    return 0;
}
