#!/bin/bash
# I2: a live DM shows "Message from alice: hi".
source "$(dirname "$0")/lib.sh"
start_server
login alice
login bob
cmd alice "sendmessage bob hi"
expect alice "Sent to bob \(#[0-9]+\)\."
expect bob "^Message from alice: hi$"
cmd bob "sendmessage alice hello back with spaces"
expect alice "^Message from bob: hello back with spaces$"
expect_server "alice -> bob #[0-9]+ delivered"
finish
