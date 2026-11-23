#!/bin/bash
# X8: a simulated SQLite error (test hook makes db_store_dm fail): the client
# gets ERROR(INTERNAL) and the server keeps running.
source "$(dirname "$0")/lib.sh"
CHAT_FAIL_STORE_DM=1 start_server "$TEST_SERVER_BIN"
login alice
login bob
cmd alice "sendmessage bob this will fail"
expect alice "Error: internal server error"
expect_server "database error: simulated failure"
expect_not bob "this will fail"
cmd alice "getuserlist"                       # still working
expect alice "Online \(2\): alice, bob"
cmd alice "creategroup #ok bob"               # other database calls still work
expect alice "Created #ok"
finish
