#!/bin/bash
# I8: sendtimed 2 -> disappearance notice; missing from history after expiry.
source "$(dirname "$0")/lib.sh"
start_server
login alice
login bob
cmd alice "sendtimed bob 2 self destruct"
expect alice "Sent to bob \(#[0-9]+\), disappears in 2s\."
expect bob "Message from alice \(disappears in [12]s\): self destruct"
expect bob "\[a message from alice has disappeared\]" 5
expect_server "message #[0-9]+ expired"
cmd bob "getmessages alice"
expect bob "No messages with alice\."
cmd alice "sendtimed bob 0 nope"
expect alice "Error: seconds must be 1-3600"
cmd alice "sendtimed bob 3601 nope"
expect alice "Error: seconds must be 1-3600"
finish
