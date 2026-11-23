/*
 * test_commands.c — U9: the client's command parser.
 */
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "test.h"

static cmd_t cmd;

static bool parses(const char *line, cmd_type_t type)
{
    return parse_line(line, &cmd) && cmd.type == type;
}

static bool fails(const char *line, const char *error_part)
{
    return !parse_line(line, &cmd) && cmd.type == CMD_INVALID &&
           strstr(cmd.error, error_part) != NULL;
}

int main(void)
{
    /* connect */
    CHECK(parses("connect 192.168.1.20 alice", CMD_CONNECT));
    CHECK(strcmp(cmd.host, "192.168.1.20") == 0 && cmd.port == DEFAULT_PORT);
    CHECK(strcmp(cmd.name, "alice") == 0);
    CHECK(parses("connect server:6000 bob", CMD_CONNECT));
    CHECK(strcmp(cmd.host, "server") == 0 && cmd.port == 6000);
    CHECK(fails("connect host:99999 bob", "invalid port"));
    CHECK(fails("connect host", "Usage"));
    CHECK(fails("connect host bad-name", "invalid username"));

    /* sendmessage: the message is the rest of the line, spaces included */
    CHECK(parses("sendmessage bob hello there   world", CMD_SEND));
    CHECK(strcmp(cmd.name, "bob") == 0 && strcmp(cmd.text, "hello there   world") == 0);
    CHECK(fails("sendmessage bob", "Usage"));
    CHECK(fails("sendmessage", "Usage"));
    CHECK(fails("sendmessage #os hi", "sendgroup"));

    char longline[1200] = "sendmessage bob ";
    memset(longline + strlen(longline), 'x', 1001);
    CHECK(fails(longline, "too long"));

    /* sendtimed: seconds 1-3600 */
    CHECK(parses("sendtimed bob 30 secret stuff", CMD_SEND_TIMED));
    CHECK(cmd.seconds == 30 && strcmp(cmd.text, "secret stuff") == 0);
    CHECK(fails("sendtimed bob 0 x", "seconds must be 1-3600"));
    CHECK(fails("sendtimed bob 3601 x", "seconds must be 1-3600"));
    CHECK(fails("sendtimed bob ten x", "seconds must be 1-3600"));
    CHECK(fails("sendtimed bob 10", "Usage"));

    /* history / delete for users and #groups */
    CHECK(parses("getmessages bob", CMD_GET_HISTORY) && strcmp(cmd.name, "bob") == 0);
    CHECK(parses("getmessages #project", CMD_GET_HISTORY) && strcmp(cmd.name, "#project") == 0);
    CHECK(parses("deletemessages #project", CMD_DELETE_HISTORY));
    CHECK(fails("getmessages", "Usage"));
    CHECK(fails("getmessages #", "invalid group name"));
    CHECK(fails("getmessages bob extra", "Usage"));

    /* groups */
    CHECK(parses("creategroup #g u1 u2 u3", CMD_GROUP_CREATE));
    CHECK(cmd.n_members == 3 && strcmp(cmd.members[2], "u3") == 0);
    CHECK(fails("creategroup #g", "Usage"));
    CHECK(fails("creategroup g bob", "invalid group name"));
    CHECK(fails("creategroup #g bob b@d", "invalid username"));
    CHECK(parses("addmember #g dave", CMD_GROUP_ADD) && strcmp(cmd.user, "dave") == 0);
    CHECK(fails("addmember #g", "Usage"));
    CHECK(parses("sendgroup #g hi all", CMD_GROUP_SEND) && strcmp(cmd.text, "hi all") == 0);
    CHECK(parses("leavegroup #g", CMD_GROUP_LEAVE));
    CHECK(parses("mygroups", CMD_GROUP_LIST));

    /* simple commands */
    CHECK(parses("getuserlist", CMD_GET_USERS));
    CHECK(fails("getuserlist now", "Usage"));
    CHECK(parses("help", CMD_HELP));
    CHECK(parses("shutdown", CMD_SHUTDOWN));
    CHECK(parses("", CMD_EMPTY));
    CHECK(parses("    ", CMD_EMPTY));
    CHECK(fails("dance", "Unknown command"));

    /* encoding matches the SPEC §4.1 example */
    parse_line("sendmessage bob hi", &cmd);
    frame_builder_t fb;
    CHECK(cmd_encode(&cmd, &fb));
    const unsigned char expected[] = { 0, 0, 0, 0x0A, 0x02, 0, 3, 'b', 'o', 'b', 0, 2, 'h', 'i' };
    CHECK(fb.used == sizeof expected && memcmp(fb.buf, expected, sizeof expected) == 0);
    parse_line("help", &cmd);
    CHECK(!cmd_encode(&cmd, &fb));     /* local command: nothing to send */

    return TEST_SUMMARY();
}
