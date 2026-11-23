#!/bin/bash
# X11: a group message that was ACKed is marked delivered and is NOT resent at
# the next login; one that was not received (offline) is.
source "$(dirname "$0")/lib.sh"
start_server
login alice
login bob
cmd alice "creategroup #ack bob carol"
expect alice "Created #ack"
cmd alice "sendgroup #ack once only"
expect bob "Message from alice in #ack: once only"
sleep 0.3
end_input bob
wait_exit bob 0
login bob2 bob
expect bob2 "Connected as bob\. 0 unread"
login carol
expect carol "Connected as carol\. 1 unread message\(s\):"
expect carol "Message from alice in #ack: once only"
finish
