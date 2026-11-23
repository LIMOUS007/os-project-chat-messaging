#!/bin/bash
# I7: 6 clients connected and messaging at once; in the test build
# (MAX_CLIENTS=6) a 7th connection gets SERVER_FULL.
source "$(dirname "$0")/lib.sh"
start_server "$TEST_SERVER_BIN"
users=(alice bob carol dave eve frank)
for u in "${users[@]}"; do login "$u"; done
for i in "${!users[@]}"; do
    from=${users[$i]}
    to=${users[$(((i + 1) % 6))]}
    cmd "$from" "sendmessage $to hello from $from"
done
for i in "${!users[@]}"; do
    to=${users[$(((i + 1) % 6))]}
    expect "$to" "Message from ${users[$i]}: hello from ${users[$i]}"
done
cmd alice "getuserlist"
expect alice "Online \(6\): alice, bob, carol, dave, eve, frank"
start_client grace
cmd grace "connect 127.0.0.1:$PORT grace"
expect grace "Error: server is full"
wait_exit grace 1
expect_server "refused: server full"
finish
