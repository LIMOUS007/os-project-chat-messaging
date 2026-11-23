# Demo runbook (SPEC §15)

The demo follows SPEC §15, with exact commands and the output to expect. **Rehearse it from
the ZIP downloaded from Moodle, starting clean.**

## 0. Before the demo (with internet)

```powershell
cd chat
docker compose down -v          # removes old containers AND the chatdata volume (old messages)
docker compose build            # pulls the base images and compiles
.\dev.ps1 make test             # keep this output as evidence (25 integration tests)
.\dev.ps1 make tsan             # keep this output too (no data races)
```

Open **6 terminals**: one server and five clients. The clients are alice, bob, carol, dave
and eve.

## 1. Explain the design (2 min)

- **Architecture:** the diagram in README §3, and who built what (README §9).
- **The four ideas to say out loud:**
  1. One event-loop thread owns every socket.
  2. 4 workers take jobs from a bounded producer-consumer queue.
  3. The no-nested-locks rule makes deadlock impossible.
  4. Delivery is at-least-once: store first, then the client ACKs.

## 2. Start

| Terminal | Command | Expect |
|---|---|---|
| 1 (B) | `docker compose up server` | `server listening on 0.0.0.0:5555 (32 slots ...)` |
| 2–5 | `docker compose run --rm client` then `connect server alice` / `bob` / `carol` / `dave` | `Connected as alice. 0 unread message(s).` |

Leave **eve** offline for now.

## 3. Core messaging

| Step | Who | Type | Expect |
|---|---|---|---|
| online list | alice | `getuserlist` | `Online (4): alice, bob, carol, dave` |
| live DM | alice | `sendmessage bob hi bob` | alice: `Sent to bob (#1).` · bob: `Message from alice: hi bob` |
| reply | bob | `sendmessage alice hey alice` | alice: `Message from bob: hey alice` |
| offline DM | carol | `sendmessage eve see you at 5` | `Stored for eve (offline) (#3).` |
| offline login | terminal 6 | `connect server eve` | `Connected as eve. 1 unread message(s):` then `Message from carol: see you at 5` |
| server log | terminal 1 | — | `alice -> bob #1 delivered`, `carol -> eve #3 stored (offline)` |

## 4. History and deletion

| Who | Type | Expect |
|---|---|---|
| alice | `getmessages bob` | `--- Conversation with bob (2) ---`, with `(seen)` on alice's own message |
| bob | `deletemessages alice` | `Deleted 2 message(s) with alice.` |
| alice | `getmessages bob` | `No messages with bob.` (deleted for both sides) |

## 5. Errors

| Where | Type | Expect |
|---|---|---|
| new client | `connect server alice` | `Error: alice is already logged in` (the client exits) |
| new client | `connect server mallory` | `Error: unknown user 'mallory'` (the client exits) |
| alice | `sendmessage nobody x` | `Error: unknown user 'nobody'` |
| alice | `sendmessage alice x` | `Error: cannot message yourself` |

## 6. F1: disappearing messages

| Who | Type | Expect |
|---|---|---|
| alice | `sendtimed bob 10 this vanishes` | bob: `Message from alice (disappears in 10s): this vanishes` |
| (wait 10 s) | — | bob: `[a message from alice has disappeared]` · server: `message #N expired` |
| bob | `getmessages alice` | the message is gone |

Say it plainly: the message is removed from the **server**. Text already printed stays in the
terminal (limitation F1.6).

## 7. F2: read receipts

| Who | Type | Expect |
|---|---|---|
| alice | `sendmessage carol did you see this` | alice: `[seen] carol saw your message #N: "did you see this"` |
| alice | `getmessages carol` | `(seen)` on the message |

## 8. F3: groups

| Who | Type | Expect |
|---|---|---|
| alice | `creategroup #os bob carol` | `Created #os with alice, bob, carol.` |
| alice | `sendgroup #os standup at 10` | bob and carol: `Message from alice in #os: standup at 10` |
| alice | `addmember #os dave` | dave: `[alice added you to #os]` |
| dave | `getmessages #os` | `No messages in #os.` (the join watermark hides older messages) |
| bob | `sendgroup #os welcome dave` | dave sees it |
| alice | `leavegroup #os` | everyone: `[#os was dissolved by alice]` |

## 9. Graceful shutdown

In terminal 1 press **Ctrl+C**. Every client prints `Server shutting down...` and exits. The
server logs `shutdown requested (SIGINT) — notifying 5 clients` and `server stopped`.

Be ready to explain the order: stop new work → let the workers finish → `SHUTDOWN` goes out
as the last frame → close (SPEC-ERRATA E3).

## 10. Evidence

Show the saved `make test` and `make tsan` output. Run `.\dev.ps1 make test` live if there is
time; it takes about 2 minutes.

## Likely questions, and where the answer is

| Question | Where |
|---|---|
| Why no deadlock? | README §4: the rules (one lock at a time, no lock across I/O) |
| What does each thread do? | README §3, thread model |
| What if a client is slow or floods? | README §4, backpressure; tests X2 and X9 |
| How is a message never lost? | README §5: pending → ACK; tests X1 and X4 |
| What happens when a slot is reused? | README §4, generation counters |
| Why can't a wake-up be missed (expiry)? | README §4, `expiry_gen`; test X10 |
| What changed from the spec? | SPEC-ERRATA.md |
