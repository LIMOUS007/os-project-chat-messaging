#!/bin/bash
# X10: a short sendtimed arrives just after the expiry thread queried MIN
# (it is sleeping until a far deadline): it must still expire on time.
source "$(dirname "$0")/lib.sh"
start_server
login alice
login bob
cmd alice "sendtimed bob 60 the long one"       # expiry thread now sleeps ~60 s
expect bob "the long one"
sleep 0.3
cmd alice "sendtimed bob 2 the short one"
expect bob "the short one"
expect bob "\[a message from alice has disappeared\]" 5
[ "$(count bob 'has disappeared')" -eq 1 ] || fail "expected exactly one disappearance"
cmd bob "getmessages alice"
expect bob "the long one"
finish
