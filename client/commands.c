/*
 * commands.c — client command parsing and encoding. See commands.h.
 */
#include "commands.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Skips spaces and tabs. */
static const char *skip_spaces(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* Copies the next space-separated word into out and advances *p.
 * Returns the word length, 0 if there is no word, or -1 if it is too long. */
static int next_word(const char **p, char *out, size_t out_size)
{
    const char *s = skip_spaces(*p);
    size_t n = 0;
    while (s[n] != '\0' && s[n] != ' ' && s[n] != '\t')
        n++;
    *p = s + n;
    out[0] = '\0';
    if (n == 0)
        return 0;
    if (n >= out_size)
        return -1;
    memcpy(out, s, n);
    out[n] = '\0';
    return (int)n;
}

static bool invalid(cmd_t *cmd, const char *fmt, const char *arg)
{
    cmd->type = CMD_INVALID;
    snprintf(cmd->error, sizeof cmd->error, fmt, arg);
    return false;
}

/* Reads a username argument, with a clear error if missing or invalid. */
static bool read_user(const char **p, char *out, cmd_t *cmd, const char *usage)
{
    int n = next_word(p, out, NAME_BUF);
    if (n == 0)
        return invalid(cmd, "Usage: %s", usage);
    if (n < 0 || !valid_username(out))
        return invalid(cmd, "Error: invalid username '%s'", n < 0 ? "(too long)" : out);
    return true;
}

/* Reads a #group argument. */
static bool read_group(const char **p, char *out, cmd_t *cmd, const char *usage)
{
    int n = next_word(p, out, NAME_BUF);
    if (n == 0)
        return invalid(cmd, "Usage: %s", usage);
    if (n < 0 || !valid_groupname(out))
        return invalid(cmd, "Error: invalid group name '%s' (use #name)",
                       n < 0 ? "(too long)" : out);
    return true;
}

/* Reads a user OR a #group argument (getmessages / deletemessages). */
static bool read_peer(const char **p, char *out, cmd_t *cmd, const char *usage)
{
    const char *save = *p;
    char word[NAME_BUF];
    int n = next_word(&save, word, sizeof word);
    if (n > 0 && is_group_name(word))
        return read_group(p, out, cmd, usage);
    return read_user(p, out, cmd, usage);
}

/* The rest of the line is the message text. */
static bool read_text(const char *p, char *out, cmd_t *cmd, const char *usage)
{
    p = skip_spaces(p);
    size_t n = strlen(p);
    if (n == 0)
        return invalid(cmd, "Usage: %s", usage);
    if (n > MAX_TEXT) {
        cmd->type = CMD_INVALID;
        snprintf(cmd->error, sizeof cmd->error, "Error: message too long (max %d bytes)",
                 MAX_TEXT);
        return false;
    }
    memcpy(out, p, n + 1);
    return true;
}

/* Fails if anything is left on the line. */
static bool expect_end(const char *p, cmd_t *cmd, const char *usage)
{
    if (*skip_spaces(p) != '\0')
        return invalid(cmd, "Usage: %s", usage);
    return true;
}

/* Parses "host" or "host:port". */
static bool parse_address(const char *word, cmd_t *cmd)
{
    const char *colon = strrchr(word, ':');
    size_t host_len = colon ? (size_t)(colon - word) : strlen(word);
    if (host_len == 0 || host_len >= sizeof cmd->host)
        return invalid(cmd, "Error: invalid server address '%s'", word);
    memcpy(cmd->host, word, host_len);
    cmd->host[host_len] = '\0';
    cmd->port = DEFAULT_PORT;
    if (colon) {
        char *end;
        long port = strtol(colon + 1, &end, 10);
        if (*end != '\0' || port < 1 || port > 65535)
            return invalid(cmd, "Error: invalid port in '%s'", word);
        cmd->port = (int)port;
    }
    return true;
}

static bool parse_connect(const char *p, cmd_t *cmd)
{
    const char *usage = "connect <server IP or hostname>[:port] <user>";
    char addr[300];
    int n = next_word(&p, addr, sizeof addr);
    if (n <= 0)
        return invalid(cmd, "Usage: %s", usage);
    if (!parse_address(addr, cmd))
        return false;
    return read_user(&p, cmd->name, cmd, usage) && expect_end(p, cmd, usage);
}

static bool parse_send_timed(const char *p, cmd_t *cmd)
{
    const char *usage = "sendtimed <user> <seconds> <message>";
    if (!read_user(&p, cmd->name, cmd, usage))
        return false;
    char word[32];
    int n = next_word(&p, word, sizeof word);
    if (n == 0)
        return invalid(cmd, "Usage: %s", usage);
    char *end;
    long seconds = n > 0 ? strtol(word, &end, 10) : 0;
    if (n < 0 || *end != '\0' || seconds < 1 || seconds > MAX_TIMED_SECONDS)
        return invalid(cmd, "Error: seconds must be 1-%s", "3600");
    cmd->seconds = (uint32_t)seconds;
    return read_text(p, cmd->text, cmd, usage);
}

static bool parse_group_create(const char *p, cmd_t *cmd)
{
    const char *usage = "creategroup #group <user1> [user2 ...]";
    if (!read_group(&p, cmd->name, cmd, usage))
        return false;
    cmd->n_members = 0;
    for (;;) {
        char word[NAME_BUF];
        const char *before = p;
        int n = next_word(&p, word, sizeof word);
        if (n == 0)
            break;
        if (cmd->n_members == CMD_MAX_MEMBERS)
            return invalid(cmd, "Error: too many members (max %s)", "64");
        p = before;
        if (!read_user(&p, cmd->members[cmd->n_members], cmd, usage))
            return false;
        cmd->n_members++;
    }
    if (cmd->n_members == 0)
        return invalid(cmd, "Usage: %s", usage);
    return true;
}

bool parse_line(const char *line, cmd_t *cmd)
{
    memset(cmd, 0, sizeof *cmd);
    const char *p = line;
    char word[32];
    int n = next_word(&p, word, sizeof word);
    if (n == 0) {
        cmd->type = CMD_EMPTY;
        return true;
    }

    if (strcmp(word, "connect") == 0) {
        cmd->type = CMD_CONNECT;
        return parse_connect(p, cmd);
    }
    if (strcmp(word, "sendmessage") == 0) {
        const char *usage = "sendmessage <user> <message>";
        cmd->type = CMD_SEND;
        char peek[NAME_BUF];
        const char *q = p;
        if (next_word(&q, peek, sizeof peek) > 0 && is_group_name(peek))
            return invalid(cmd, "Error: use sendgroup to message %s", peek);
        return read_user(&p, cmd->name, cmd, usage) && read_text(p, cmd->text, cmd, usage);
    }
    if (strcmp(word, "sendtimed") == 0) {
        cmd->type = CMD_SEND_TIMED;
        return parse_send_timed(p, cmd);
    }
    if (strcmp(word, "getmessages") == 0 || strcmp(word, "deletemessages") == 0) {
        bool get = word[0] == 'g';
        const char *usage = get ? "getmessages <user|#group>" : "deletemessages <user|#group>";
        cmd->type = get ? CMD_GET_HISTORY : CMD_DELETE_HISTORY;
        return read_peer(&p, cmd->name, cmd, usage) && expect_end(p, cmd, usage);
    }
    if (strcmp(word, "creategroup") == 0) {
        cmd->type = CMD_GROUP_CREATE;
        return parse_group_create(p, cmd);
    }
    if (strcmp(word, "addmember") == 0) {
        const char *usage = "addmember #group <user>";
        cmd->type = CMD_GROUP_ADD;
        return read_group(&p, cmd->name, cmd, usage) && read_user(&p, cmd->user, cmd, usage) &&
               expect_end(p, cmd, usage);
    }
    if (strcmp(word, "sendgroup") == 0) {
        const char *usage = "sendgroup #group <message>";
        cmd->type = CMD_GROUP_SEND;
        return read_group(&p, cmd->name, cmd, usage) && read_text(p, cmd->text, cmd, usage);
    }
    if (strcmp(word, "leavegroup") == 0) {
        const char *usage = "leavegroup #group";
        cmd->type = CMD_GROUP_LEAVE;
        return read_group(&p, cmd->name, cmd, usage) && expect_end(p, cmd, usage);
    }

    /* Commands without arguments. */
    struct { const char *word; cmd_type_t type; } simple[] = {
        { "getuserlist", CMD_GET_USERS },
        { "mygroups", CMD_GROUP_LIST },
        { "help", CMD_HELP },
        { "shutdown", CMD_SHUTDOWN },
    };
    for (size_t i = 0; i < sizeof simple / sizeof simple[0]; i++) {
        if (strcmp(word, simple[i].word) == 0) {
            cmd->type = simple[i].type;
            return expect_end(p, cmd, simple[i].word);
        }
    }
    return invalid(cmd, "Unknown command '%s'. Type 'help' for the list of commands.", word);
}

bool cmd_encode(const cmd_t *cmd, frame_builder_t *fb)
{
    switch (cmd->type) {
    case CMD_CONNECT:
        fb_begin(fb, T_LOGIN);
        fb_str(fb, cmd->name);
        break;
    case CMD_SEND:
        fb_begin(fb, T_SEND);
        fb_str(fb, cmd->name);
        fb_str(fb, cmd->text);
        break;
    case CMD_SEND_TIMED:
        fb_begin(fb, T_SEND_TIMED);
        fb_str(fb, cmd->name);
        fb_u32(fb, cmd->seconds);
        fb_str(fb, cmd->text);
        break;
    case CMD_GET_HISTORY:
        fb_begin(fb, T_GET_HISTORY);
        fb_str(fb, cmd->name);
        break;
    case CMD_DELETE_HISTORY:
        fb_begin(fb, T_DELETE_HISTORY);
        fb_str(fb, cmd->name);
        break;
    case CMD_GET_USERS:
        fb_begin(fb, T_GET_USERS);
        break;
    case CMD_GROUP_CREATE:
        fb_begin(fb, T_GROUP_CREATE);
        fb_str(fb, cmd->name);
        fb_u16(fb, (uint16_t)cmd->n_members);
        for (int i = 0; i < cmd->n_members; i++)
            fb_str(fb, cmd->members[i]);
        break;
    case CMD_GROUP_ADD:
        fb_begin(fb, T_GROUP_ADD);
        fb_str(fb, cmd->name);
        fb_str(fb, cmd->user);
        break;
    case CMD_GROUP_SEND:
        fb_begin(fb, T_GROUP_SEND);
        fb_str(fb, cmd->name);
        fb_str(fb, cmd->text);
        break;
    case CMD_GROUP_LEAVE:
        fb_begin(fb, T_GROUP_LEAVE);
        fb_str(fb, cmd->name);
        break;
    case CMD_GROUP_LIST:
        fb_begin(fb, T_GROUP_LIST);
        break;
    case CMD_SHUTDOWN:
        fb_begin(fb, T_LOGOUT);
        break;
    default:
        return false;   /* local command: nothing to send */
    }
    return fb_finish(fb);
}

const char *help_text(void)
{
    return "Commands:\n"
           "  connect <server IP or hostname>[:port] <user>\n"
           "  sendmessage <user> <message>\n"
           "  sendtimed <user> <seconds 1-3600> <message>   (disappearing message)\n"
           "  getmessages <user|#group>\n"
           "  deletemessages <user|#group>\n"
           "  getuserlist\n"
           "  creategroup #group <user1> [user2 ...]\n"
           "  addmember #group <user>\n"
           "  sendgroup #group <message>\n"
           "  leavegroup #group\n"
           "  mygroups\n"
           "  help\n"
           "  shutdown";
}
