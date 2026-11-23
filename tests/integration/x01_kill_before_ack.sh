#!/bin/bash
# X1: the recipient's connection is killed while a message is in flight
# (received but not ACKed) -> it stays pending and is shown at the next login.
source "$(dirname "$0")/lib.sh"
start_server
"$RAWBOT" "$PORT" noack bob > "$WORK/rawbob.log" 2>&1 &
RAW=$!
expect rawbob "logged in"
login alice
cmd alice "sendmessage bob in flight"
expect rawbob "MSG [0-9]+ in flight"
kill -9 "$RAW"
wait "$RAW" 2>/dev/null
expect_server "bob disconnected"
login bob
expect bob "Connected as bob\. 1 unread message\(s\):"
expect bob "Message from alice: in flight"
finish
