#!/bin/bash
# X2: a client that never reads its socket while others send to it: its
# output buffer fills -> it is disconnected; other clients are unaffected;
# its messages are still pending.
source "$(dirname "$0")/lib.sh"
start_server
"$RAWBOT" "$PORT" noread bob > "$WORK/rawbob.log" 2>&1 &
RAW=$!
expect rawbob "logged in"
login carol
"$RAWBOT" "$PORT" sendmany alice bob 8000 1000 > "$WORK/sender.log" 2>&1 &
SENDER=$!
expect_server "bob disconnected" 60
cmd carol "getuserlist"                       # still responsive
expect carol "Online \(2\): alice, carol"
wait "$SENDER"
expect sender "sent 8000 ok 8000"
kill "$RAW" 2>/dev/null
wait "$RAW" 2>/dev/null

login bob
expect bob "Connected as bob\. [0-9]+ unread message\(s\):"
unread=$(grep -oE "[0-9]+ unread" "$WORK/bob.log" | grep -oE "[0-9]+")
[ "${unread:-0}" -gt 0 ] || fail "expected pending messages for bob, got ${unread:-none}"

# Far more than one output buffer is pending: the login replay is paged, so
# bob stays connected and eventually sees every one of them.
for _ in $(seq 300); do
    [ "$(count bob '^Message from alice: ')" -ge "${unread:-0}" ] && break
    sleep 0.1
done
shown=$(count bob '^Message from alice: ')
[ "$shown" -eq "${unread:-0}" ] || fail "bob saw $shown of $unread pending messages"
expect_not bob "Connection to server lost"

# History is capped to what fits in the output buffer. (Wait first: bob's
# client is still sending 16,000 ACK/MARK_SEEN frames, and the server runs each
# client's frames in order, so a command now would queue behind them.)
sleep 12
cmd bob "getmessages alice"
expect bob "\-\-\- Conversation with alice \(showing last [0-9]+ of 8000\) \-\-\-" 10
finish
