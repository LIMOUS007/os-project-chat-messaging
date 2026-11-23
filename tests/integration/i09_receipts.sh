#!/bin/bash
# I9: a read receipt arrives live; an offline sender sees "seen" in history later.
source "$(dirname "$0")/lib.sh"
start_server
login alice
login bob
cmd alice "sendmessage bob did you read this"
expect alice "\[seen\] bob saw your message #[0-9]+: \"did you read this\""

end_input bob
wait_exit bob 0
cmd alice "sendmessage bob read it later"
expect alice "Stored for bob"
end_input alice
wait_exit alice 0

login bob2 bob
expect bob2 "Message from alice: read it later"
sleep 0.5
login alice2 alice
cmd alice2 "getmessages bob"
expect alice2 "alice -> bob: read it later +\(seen\)"
finish
