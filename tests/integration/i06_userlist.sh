#!/bin/bash
# I6: getuserlist before and after a client shutdown.
source "$(dirname "$0")/lib.sh"
start_server
login alice
login bob
login carol
cmd alice "getuserlist"
expect alice "Online \(3\): alice, bob, carol"
cmd bob "shutdown"
expect bob "^Disconnected\.$"
wait_exit bob 0
expect_server "bob disconnected"
cmd alice "getuserlist"
expect alice "Online \(2\): alice, carol"
finish
