#!/bin/bash
# I11: SIGINT -> every client prints "Server shutting down..." and exits 0;
# the server exits 0. (I12 runs this same script with SIGNAL=TERM.)
source "$(dirname "$0")/lib.sh"
SIGNAL=${SIGNAL:-INT}
start_server
for u in alice bob carol dave; do login "$u"; done
kill -"$SIGNAL" "$SERVER_PID"
for u in alice bob carol dave; do
    expect "$u" "^Server shutting down\.\.\.$"
    wait_exit "$u" 0
done
wait_server_exit 0
expect_server "shutdown requested \(SIG$SIGNAL\) — notifying 4 clients"
expect_server "server stopped"
finish
