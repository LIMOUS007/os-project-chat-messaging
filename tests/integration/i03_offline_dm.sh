#!/bin/bash
# I3: a DM to an offline user is shown at that user's next login.
source "$(dirname "$0")/lib.sh"
start_server
login alice
cmd alice "sendmessage bob are you there?"
expect alice "Stored for bob \(offline\) \(#[0-9]+\)\."
expect_server "alice -> bob #[0-9]+ stored \(offline\)"
login bob
expect bob "Connected as bob\. 1 unread message\(s\):"
expect bob "^Message from alice: are you there\?$"
end_input bob
wait_exit bob 0
login bob2 bob
expect bob2 "Connected as bob\. 0 unread"     # it was ACKed: not shown again
finish
