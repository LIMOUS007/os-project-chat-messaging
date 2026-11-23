/*
 * users.c — users.txt parsing and lookup.
 */
#include "users.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"

static char (*user_names)[NAME_BUF] = NULL;  /* sorted array of names */
static int user_total = 0;

static int compare_names(const void *a, const void *b) { return strcmp(a, b); }

/* Removes leading and trailing whitespace (including '\r' from Windows files). */
static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
    return s;
}

int users_load(const char *path, char *err, size_t err_size)
{
    users_free();
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        snprintf(err, err_size, "cannot open %s", path);
        return -1;
    }

    int cap = 16;
    user_names = malloc((size_t)cap * NAME_BUF);
    char line[256];
    int line_no = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        line_no++;
        char *name = trim(line);
        if (name[0] == '\0' || name[0] == '#')
            continue;
        if (!valid_username(name)) {
            snprintf(err, err_size, "%s line %d: invalid username '%s'", path, line_no, name);
            fclose(f);
            users_free();
            return -1;
        }
        if (user_total == cap) {
            cap *= 2;
            user_names = realloc(user_names, (size_t)cap * NAME_BUF);
        }
        strcpy(user_names[user_total++], name);
    }
    fclose(f);

    qsort(user_names, (size_t)user_total, NAME_BUF, compare_names);
    return 0;
}

bool users_exists(const char *name)
{
    if (user_total == 0)
        return false;
    return bsearch(name, user_names, (size_t)user_total, NAME_BUF, compare_names) != NULL;
}

int users_count(void) { return user_total; }

void users_free(void)
{
    free(user_names);
    user_names = NULL;
    user_total = 0;
}
