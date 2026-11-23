/*
 * test_users.c — U10: users.txt parsing (comments, blank lines, Windows line
 * endings, invalid names).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test.h"
#include "users.h"

static const char *write_file(const char *content)
{
    static char path[] = "/tmp/test_users_XXXXXX";
    strcpy(path, "/tmp/test_users_XXXXXX");
    int fd = mkstemp(path);
    write(fd, content, strlen(content));
    close(fd);
    return path;
}

int main(void)
{
    char err[256];

    const char *p = write_file("# the whitelist\n\nalice\n  bob  \r\ncarol\r\n\n# end\n");
    CHECK_EQ(users_load(p, err, sizeof err), 0);
    unlink(p);
    CHECK_EQ(users_count(), 3);
    CHECK(users_exists("alice"));
    CHECK(users_exists("bob"));
    CHECK(users_exists("carol"));
    CHECK(!users_exists("mallory"));
    CHECK(!users_exists("# the whitelist"));
    CHECK(!users_exists(""));

    p = write_file("alice\nnot valid\n");
    CHECK_EQ(users_load(p, err, sizeof err), -1);
    CHECK(strstr(err, "line 2") != NULL);
    unlink(p);

    p = write_file("alice\nbob-smith\n");
    CHECK_EQ(users_load(p, err, sizeof err), -1);
    unlink(p);

    CHECK_EQ(users_load("/nonexistent/users.txt", err, sizeof err), -1);

    users_free();
    return TEST_SUMMARY();
}
