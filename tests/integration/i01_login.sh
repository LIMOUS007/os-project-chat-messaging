#!/bin/bash
# I1: login OK; unknown user -> error, exit 1; duplicate login rejected, exit 1.
source "$(dirname "$0")/lib.sh"
start_server
login alice
expect_server "alice logged in"

start_client mallory
cmd mallory "connect 127.0.0.1:$PORT mallory"
expect mallory "Error: unknown user 'mallory'"
wait_exit mallory 1
expect_server "login rejected for 'mallory': unknown user"

start_client alice2
cmd alice2 "connect 127.0.0.1:$PORT alice"
expect alice2 "Error: alice is already logged in"
wait_exit alice2 1

start_client early
cmd early "getuserlist"
expect early "Not connected. Use: connect <server IP> <user>"
finish
