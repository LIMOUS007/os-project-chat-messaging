/*
 * test_db.c — U6, U7, U8: the database layer, on an in-memory database.
 *   U6  store / pending / ACK (DM and group) / MARK_SEEN idempotency,
 *       history order, delete-for-both counts
 *   U7  expiry: next_expiry, delete_expired, expired rows hidden
 *   U8  groups: unknown members are checked by the handler (atomic create is
 *       checked here via duplicates), watermark, creator leave cascades,
 *       fan-out excludes the sender, leave/rejoin pending rules
 */
#include <string.h>

#include "db.h"
#include "test.h"

static db_err_t err;

static int pending_count(const char *user, time_t now)
{
    int n = -1;
    db_count_pending(user, now, &n, &err);
    return n;
}

static void test_dm_delivery(void)
{
    time_t now = 1000;
    uint64_t id1, id2, id3;
    CHECK_EQ(db_store_dm("alice", "bob", "one", now, 0, &id1, &err), DB_OK);
    CHECK_EQ(db_store_dm("bob", "alice", "two", now, 0, &id2, &err), DB_OK);
    CHECK_EQ(db_store_dm("alice", "bob", "three", now, 0, &id3, &err), DB_OK);
    CHECK(id1 < id2 && id2 < id3);

    CHECK_EQ(pending_count("bob", now), 2);
    db_messages_t list = {0};
    CHECK_EQ(db_pending_for("bob", now, 0, 1000, &list, &err), DB_OK);
    CHECK_EQ(list.count, 2);
    CHECK(list.count == 2 && list.items[0].id == id1 && list.items[1].id == id3);
    db_messages_free(&list);
    /* paging: a limit, and a cursor (only ids after the last one sent) */
    db_pending_for("bob", now, 0, 1, &list, &err);
    CHECK(list.count == 1 && list.items[0].id == id1);
    db_messages_free(&list);
    db_pending_for("bob", now, id1, 1000, &list, &err);
    CHECK(list.count == 1 && list.items[0].id == id3);
    db_messages_free(&list);

    /* ACK -> delivered, twice is harmless; someone else's ACK changes nothing */
    CHECK_EQ(db_ack("carol", id1, now, &err), DB_OK);
    CHECK_EQ(pending_count("bob", now), 2);
    CHECK_EQ(db_ack("bob", id1, now, &err), DB_OK);
    CHECK_EQ(db_ack("bob", id1, now, &err), DB_OK);
    CHECK_EQ(pending_count("bob", now), 1);

    /* MARK_SEEN: changes once, then never again */
    bool changed = false;
    char sender[NAME_BUF], preview[PREVIEW_MAX + 1];
    CHECK_EQ(db_mark_seen("bob", id1, now, &changed, sender, preview, &err), DB_OK);
    CHECK(changed && strcmp(sender, "alice") == 0 && strcmp(preview, "one") == 0);
    CHECK_EQ(db_mark_seen("bob", id1, now, &changed, sender, preview, &err), DB_OK);
    CHECK(!changed);

    /* history: id order, statuses */
    CHECK_EQ(db_history_dm("alice", "bob", now, &list, &err), DB_OK);
    CHECK_EQ(list.count, 3);
    if (list.count == 3) {
        CHECK(list.items[0].id == id1 && list.items[1].id == id2 && list.items[2].id == id3);
        CHECK_EQ(list.items[0].status, STATUS_SEEN);
        CHECK_EQ(list.items[2].status, STATUS_PENDING);
    }
    db_messages_free(&list);

    /* delete for both: the count, and nothing left on either side */
    uint64_t other;
    db_store_dm("alice", "carol", "keep me", now, 0, &other, &err);
    int deleted = 0;
    CHECK_EQ(db_delete_dm("bob", "alice", now, &deleted, &err), DB_OK);
    CHECK_EQ(deleted, 3);
    CHECK_EQ(db_history_dm("alice", "bob", now, &list, &err), DB_OK);
    CHECK_EQ(list.count, 0);
    db_messages_free(&list);
    CHECK_EQ(db_history_dm("carol", "alice", now, &list, &err), DB_OK);
    CHECK_EQ(list.count, 1);
    db_messages_free(&list);
    db_ack("carol", other, now, &err);   /* so carol starts the group tests with 0 pending */
}

static void test_expiry(void)
{
    time_t now = 2000;
    time_t next;
    uint64_t a, b;
    CHECK_EQ(db_store_dm("alice", "dave", "soon", now, now + 10, &a, &err), DB_OK);
    CHECK_EQ(db_store_dm("alice", "dave", "later", now, now + 50, &b, &err), DB_OK);
    CHECK_EQ(db_next_expiry(&next, &err), DB_OK);
    CHECK_EQ(next, now + 10);

    /* After the deadline the message is hidden even before it is deleted. */
    CHECK_EQ(pending_count("dave", now + 10), 1);
    db_messages_t list = {0};
    CHECK_EQ(db_history_dm("dave", "alice", now + 10, &list, &err), DB_OK);
    CHECK_EQ(list.count, 1);
    db_messages_free(&list);

    CHECK_EQ(db_delete_expired(now + 10, &list, &err), DB_OK);
    CHECK_EQ(list.count, 1);
    CHECK(list.count == 1 && list.items[0].id == a && strcmp(list.items[0].recipient, "dave") == 0);
    db_messages_free(&list);
    CHECK_EQ(db_next_expiry(&next, &err), DB_OK);
    CHECK_EQ(next, now + 50);

    CHECK_EQ(db_delete_expired(now + 100, &list, &err), DB_OK);
    CHECK_EQ(list.count, 1);
    db_messages_free(&list);
    CHECK_EQ(db_next_expiry(&next, &err), DB_OK);
    CHECK_EQ(next, 0);
    CHECK_EQ(pending_count("dave", now + 100), 0);
}

static void test_groups(void)
{
    time_t now = 3000;
    char members[3][NAME_BUF] = { "bob", "carol", "bob" };   /* duplicate on purpose */
    CHECK_EQ(db_group_create("#os", "alice", members, 3, now, &err), DB_OK);
    CHECK_EQ(db_group_create("#os", "bob", members, 1, now, &err), DB_EXISTS);

    /* fan-out excludes the sender */
    uint64_t g1;
    db_names_t to = {0};
    CHECK_EQ(db_store_group_msg("#os", "alice", "hello", now, &g1, &to, &err), DB_OK);
    CHECK_EQ(to.count, 2);
    CHECK(to.count == 2 && strcmp(to.names[0], "bob") == 0 && strcmp(to.names[1], "carol") == 0);
    db_names_free(&to);
    CHECK_EQ(db_store_group_msg("#os", "dave", "x", now, &g1, &to, &err), DB_NOT_MEMBER);
    CHECK_EQ(db_store_group_msg("#nope", "alice", "x", now, &g1, &to, &err), DB_NO_GROUP);
    db_names_free(&to);

    /* group ACK: delivered, not pending at the next login (X11) */
    CHECK_EQ(pending_count("bob", now), 1);
    CHECK_EQ(db_ack("bob", g1, now, &err), DB_OK);
    CHECK_EQ(pending_count("bob", now), 0);
    CHECK_EQ(pending_count("carol", now), 1);

    /* watermark: a late joiner does not see older messages */
    CHECK_EQ(db_group_add("#os", "bob", "dave", &err), DB_NOT_CREATOR);
    CHECK_EQ(db_group_add("#os", "alice", "dave", &err), DB_OK);
    CHECK_EQ(db_group_add("#os", "alice", "dave", &err), DB_EXISTS);
    db_messages_t list = {0};
    CHECK_EQ(db_history_group("#os", "dave", &list, &err), DB_OK);
    CHECK_EQ(list.count, 0);
    db_messages_free(&list);
    uint64_t g2;
    db_store_group_msg("#os", "bob", "after dave joined", now, &g2, &to, &err);
    db_names_free(&to);
    CHECK_EQ(db_history_group("#os", "dave", &list, &err), DB_OK);
    CHECK_EQ(list.count, 1);
    CHECK(list.count == 1 && list.items[0].status == STATUS_NA);
    db_messages_free(&list);
    CHECK_EQ(db_history_group("#os", "alice", &list, &err), DB_OK);
    CHECK_EQ(list.count, 2);
    db_messages_free(&list);
    CHECK_EQ(db_history_group("#os", "eve", &list, &err), DB_NOT_MEMBER);

    /* leaving: pending group messages are no longer delivered (errata) */
    CHECK_EQ(pending_count("carol", now), 2);
    bool dissolved = true;
    db_names_t others = {0};
    CHECK_EQ(db_group_leave("#os", "carol", &dissolved, &others, &err), DB_OK);
    CHECK(!dissolved);
    db_names_free(&others);
    CHECK_EQ(pending_count("carol", now), 0);
    db_pending_for("carol", now, 0, 1000, &list, &err);
    CHECK_EQ(list.count, 0);
    db_messages_free(&list);
    CHECK_EQ(db_group_leave("#os", "carol", &dissolved, &others, &err), DB_NOT_MEMBER);

    /* rejoin: only messages after the rejoin are pending */
    CHECK_EQ(db_group_add("#os", "alice", "carol", &err), DB_OK);
    CHECK_EQ(pending_count("carol", now), 0);
    uint64_t g3;
    db_store_group_msg("#os", "alice", "welcome back", now, &g3, &to, &err);
    db_names_free(&to);
    CHECK_EQ(pending_count("carol", now), 1);

    /* user groups */
    db_names_t groups = {0};
    CHECK_EQ(db_user_groups("dave", &groups, &err), DB_OK);
    CHECK(groups.count == 1 && strcmp(groups.names[0], "#os") == 0);
    db_names_free(&groups);

    /* delete history: creator only, for everyone; the group stays */
    int count = 0;
    CHECK_EQ(db_delete_group_history("#os", "bob", &count, &err), DB_NOT_CREATOR);
    CHECK_EQ(db_delete_group_history("#os", "alice", &count, &err), DB_OK);
    CHECK_EQ(count, 3);
    CHECK_EQ(db_history_group("#os", "alice", &list, &err), DB_OK);
    CHECK_EQ(list.count, 0);
    db_messages_free(&list);
    CHECK_EQ(pending_count("carol", now), 0);

    /* creator leaves: dissolved, cascades, members listed for notification */
    uint64_t g4;
    db_store_group_msg("#os", "bob", "last words", now, &g4, &to, &err);
    db_names_free(&to);
    CHECK_EQ(db_group_leave("#os", "alice", &dissolved, &others, &err), DB_OK);
    CHECK(dissolved);
    CHECK_EQ(others.count, 3);   /* bob, carol, dave */
    db_names_free(&others);
    CHECK_EQ(db_history_group("#os", "bob", &list, &err), DB_NO_GROUP);
    CHECK_EQ(pending_count("carol", now), 0);
    CHECK_EQ(db_user_groups("bob", &groups, &err), DB_OK);
    CHECK_EQ(groups.count, 0);
    db_names_free(&groups);
    /* the name is free again */
    CHECK_EQ(db_group_create("#os", "bob", members, 1, now, &err), DB_OK);
}

int main(void)
{
    if (db_open(":memory:", &err) != DB_OK) {
        printf("cannot open db: %s\n", err.msg);
        return 1;
    }
    test_dm_delivery();
    test_expiry();
    test_groups();
    db_close();
    return TEST_SUMMARY();
}
