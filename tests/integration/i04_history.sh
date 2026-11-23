#!/bin/bash
# I4: getmessages order and statuses; unknown user -> error.
source "$(dirname "$0")/lib.sh"
start_server
login alice
login bob
cmd alice "sendmessage bob first"
expect bob "Message from alice: first"
cmd bob "sendmessage alice second"
expect alice "Message from bob: second"
end_input bob
wait_exit bob 0
cmd alice "sendmessage bob third"
expect alice "Stored for bob"
cmd alice "getmessages bob"
expect alice "^--- Conversation with bob \(3\) ---$"
expect alice "alice -> bob: first +\(seen\)$"
expect alice "bob -> alice: second$"
expect alice "alice -> bob: third +\(sent\)$"
expect alice "^--- end ---$"
order=$(grep -E "^\[#" "$WORK/alice.log" | grep -oE "(first|second|third)" | tr '\n' ' ')
[ "$order" = "first second third " ] || fail "wrong order: $order"
cmd alice "getmessages nobody"
expect alice "Error: unknown user 'nobody'"
cmd alice "getmessages alice"
expect alice "Error: cannot message yourself"
cmd alice "getmessages carol"
expect alice "No messages with carol\."
finish
