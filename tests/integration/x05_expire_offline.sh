#!/bin/bash
# X5: a timed message expires while the recipient is offline -> never shown.
source "$(dirname "$0")/lib.sh"
start_server
login alice
cmd alice "sendtimed bob 1 gone before you come"
expect alice "Stored for bob \(offline\)"
expect_server "message #[0-9]+ expired" 5
login bob
expect bob "Connected as bob\. 0 unread"
sleep 0.5
expect_not bob "gone before you come"
cmd bob "getmessages alice"
expect bob "No messages with alice\."
finish
