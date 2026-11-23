#!/bin/bash
# X4: a login at the same moment as messages to that user, repeated 200
# times: every message is shown exactly once (dedupe) and none is lost.
source "$(dirname "$0")/lib.sh"
start_server
"$RAWBOT" "$PORT" loginrace 200 > "$WORK/race.log" 2>&1
expect race "loginrace ok 200" 1
finish
