#!/bin/bash
# X13 (extra, SPEC §4.1 and §4.5): framing errors close the connection; a
# well-framed bad payload gets ERROR(BAD_REQUEST) and the connection stays
# open; commands before LOGIN get NOT_LOGGED_IN; an idle connection that never
# logs in is closed after 10 s.
source "$(dirname "$0")/lib.sh"
start_server
"$RAWBOT" "$PORT" badframe > "$WORK/bad.log" 2>&1
expect bad "zero-length: closed"
expect bad "too-long: closed"
expect bad "bad payload: ERROR 5"
expect bad "before login: ERROR 4"
expect bad "then login: ok"
"$RAWBOT" "$PORT" idle > "$WORK/idle.log" 2>&1
expect idle "closed after (10|11|12)s"
expect_server "login timeout"
finish
