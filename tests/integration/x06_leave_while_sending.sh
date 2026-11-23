#!/bin/bash
# X6: a member leaves a group while another member sends in a loop: no crash,
# and messages after the leave are not delivered to them.
source "$(dirname "$0")/lib.sh"
start_server
login alice
login bob
login carol
cmd alice "creategroup #busy bob carol"
expect alice "Created #busy"
( for n in $(seq 100); do echo "sendgroup #busy tick $n"; done ) >&"${CLIENT_FD[alice]}" &
LOOP=$!
expect bob "Message from alice in #busy: tick 5"
cmd bob "leavegroup #busy"
expect bob "^Left #busy\.$"
wait "$LOOP"
expect carol "Message from alice in #busy: tick 100" 15
sleep 0.5
# Nothing in bob's log after the "Left #busy." line may be a #busy message.
after=$(sed -n '/^Left #busy\.$/,$p' "$WORK/bob.log" | grep -c "in #busy" || true)
[ "$after" -eq 0 ] || fail "bob got $after group messages after leaving"
cmd carol "getuserlist"
expect carol "Online \(3\)"
finish
