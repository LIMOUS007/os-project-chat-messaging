/*
 * commands.h — turns a line the user typed into a command, and a command into
 * a protocol frame (SPEC §7.3). Pure functions: no I/O, no threads, no locks,
 * so they are easy to unit test (U9).
 */
#ifndef COMMANDS_H
#define COMMANDS_H

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

#define CMD_MAX_MEMBERS 64

typedef enum {
    CMD_EMPTY,          /* blank line                         */
    CMD_INVALID,        /* parse error: see cmd->error        */
    CMD_HELP,
    CMD_CONNECT,        /* connect <host>[:port] <user>       */
    CMD_SEND,           /* sendmessage <user> <message>       */
    CMD_SEND_TIMED,     /* sendtimed <user> <sec> <message>   */
    CMD_GET_HISTORY,    /* getmessages <user|#group>          */
    CMD_DELETE_HISTORY, /* deletemessages <user|#group>       */
    CMD_GET_USERS,      /* getuserlist                        */
    CMD_GROUP_CREATE,   /* creategroup #g <u1> [u2 ...]       */
    CMD_GROUP_ADD,      /* addmember #g <user>                */
    CMD_GROUP_SEND,     /* sendgroup #g <message>             */
    CMD_GROUP_LEAVE,    /* leavegroup #g                      */
    CMD_GROUP_LIST,     /* mygroups                           */
    CMD_SHUTDOWN,       /* shutdown                           */
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    char       host[256];                          /* connect                    */
    int        port;                               /* connect                    */
    char       name[NAME_BUF];                     /* user, peer or #group       */
    char       user[NAME_BUF];                     /* addmember: the new member  */
    uint32_t   seconds;                            /* sendtimed                  */
    char       text[TEXT_BUF];                     /* message text               */
    char       members[CMD_MAX_MEMBERS][NAME_BUF]; /* creategroup                */
    int        n_members;
    char       error[200];                         /* full line to print if CMD_INVALID */
} cmd_t;

/* Parses one input line (without the newline). Everything after the
 * recipient (or the seconds) up to the end of the line is the message.
 * Returns false and sets cmd->type = CMD_INVALID with cmd->error filled in
 * if the line is not a valid command. */
bool parse_line(const char *line, cmd_t *cmd);

/* Builds the request frame for a network command. Returns false for local
 * commands (help, empty, invalid) that send nothing. */
bool cmd_encode(const cmd_t *cmd, frame_builder_t *fb);

/* The help text (several lines). */
const char *help_text(void);

#endif
