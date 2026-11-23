#!/bin/bash
# I5: deletemessages removes the conversation for both sides.
source "$(dirname "$0")/lib.sh"
start_server
login alice
login bob
cmd alice "sendmessage bob one"
cmd alice "sendmessage bob two"
cmd bob "sendmessage alice three"
expect alice "Message from bob: three"
cmd alice "sendmessage carol unrelated"
expect alice "Stored for carol"
cmd bob "deletemessages alice"
expect bob "Deleted 3 message\(s\) with alice\."
cmd alice "getmessages bob"
expect alice "No messages with bob\."
cmd bob "getmessages alice"
expect bob "No messages with alice\."
cmd alice "getmessages carol"
expect alice "alice -> carol: unrelated"
cmd alice "deletemessages nobody"
expect alice "Error: unknown user 'nobody'"
finish
