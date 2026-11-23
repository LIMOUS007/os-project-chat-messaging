# SPEC Rev 3.1 — approved implementation clarifications

`SPEC.md` (Rev 3) is frozen. This file records every place where the implementation makes
the spec more precise or has to depart from it. The contract is **SPEC.md plus this file**.
Each entry names the spec section it affects and says why the change is needed.

> **Team approval:** these clarifications came out of the design review. Every member should
> read them and sign off before the design document is submitted.

---

## E1. Dispatch: reserve `busy` before publishing the job (§6.3 step 7, §6.4)

**Spec:** push the frame, then remove it from the inbox and set `busy = true`.

**Problem:** a worker can pop and finish the job, setting `busy = false`, *before* the event
loop sets `busy = true`. The session would then stay "busy" forever and never be dispatched
again.

**Implementation** (`eventloop.c: dispatch`, `session.c: session_reserve_job`):
1. Set `busy = true` under `sessions_mutex`. This is the reservation.
2. Release the lock, then call `jobq_try_push()`.
3. If the push succeeds, **ownership of the frame passes to the worker**. The event loop only
   removes its pointer from the inbox; it never touches or frees that frame again. The worker
   frees it.
4. If the push fails, set `busy = false` again (roll back). The frame stays in the inbox.

## E2. Session lifetime: the event loop is the only owner (§6.5, §6.6)

**Spec:** `session_send` marks an overflowing session `CLOSING`, and a failed login closes the
connection. The spec does not say which thread performs those transitions.

**Implementation:** workers never change a session's state. They only *request* a close:
- `close_req = CLOSE_AFTER_FLUSH` for a failed login: send the `ERROR` first, then close.
- `close_req = CLOSE_NOW` for `LOGOUT` and for output-buffer overflow.

`session_send` returns `SEND_OK`, `SEND_STALE` or `SEND_OVERFLOW`. Once a close has been
requested it drops every new frame. The event loop carries out each request in its
housekeeping step. Workers may still do two things directly: switch `LOGIN -> ACTIVE` (the
duplicate check, which must happen together with `LOGIN_OK`) and clear `busy`.

## E3. Shutdown order (§6.9)

**Spec:** broadcast `SHUTDOWN` first, then close the job queue.

**Problem:** the workers and the expiry thread could still append `MSG`, `RECEIPT` or
`EXPIRED` frames after the broadcast, so `SHUTDOWN` would not be the last frame a client
receives.

**Implementation** (`eventloop.c`) runs a state machine:
1. **RUNNING → DRAINING.** Close the listening socket. Stop reading from clients and stop
   dispatching; frames that were never dispatched are discarded. Stop the expiry thread and
   close the job queue.
2. **DRAINING.** The workers finish the jobs already queued. The event loop keeps writing
   output while it waits for `producers_alive` (4 workers + the expiry thread) to reach 0.
   There is **no forced exit**: if this takes more than 5 s, a warning is logged and the loop
   keeps waiting. Handlers never block on the network, so draining takes milliseconds.
3. **FLUSHING.** Now nothing else can produce frames. Append `SHUTDOWN` as the **last** frame
   of every client, then flush for at most 2 s.
4. **DONE.** Close every socket. `main` joins all threads, closes SQLite and exits 0.

The shutdown flag is an `atomic_int`.

## E4. A client timeout disconnects (§4.5, §7.2)

**Spec:** after 5 s with no reply, print `No response from server.` and continue.

**Problem:** frames carry no request ids. A late reply would be taken as the reply to the
*next* command.

**Implementation** (`client/main.c`): on timeout the client prints
`No response from server. Disconnecting.`, then cleans up in this order:
1. `shutdown(fd)`
2. join the receiver thread
3. `close(fd)`
4. clear the pending-command context and the duplicate filter
5. drain stale bytes from the notify pipe

The client then goes back to the not-connected state, and the user can `connect` again. The
pending-command context is protected by `ctx_mutex`; the pipe byte only wakes the input
thread up.

## E5. Pending group delivery checks current membership (§5 step 5, §8.3)

**Problem:** a member who left a group still had pending `group_delivery` rows and would get
those messages at their next login.

**Implementation:** the pending query joins `group_members` and only returns
`m.id > joined_after_id`. A member who has left gets nothing. A member who rejoins only gets
messages sent after the rejoin. `db_group_leave` also deletes the leaver's pending rows as a
tidy-up, but correctness does not depend on that.

## E6. Group messages use `messages.state = 255` (§8.2)

A group message row has `state = 255` ("not applicable"). Group delivery is tracked **only** in
`group_delivery.state`. The DM queries filter on `recipient`, so they never touch group rows.

## E7. Duplicate filter is a fixed-size ring (§5 step 2)

The client remembers the **most recent 1024** displayed message ids, in a circular buffer
searched linearly. Memory stays bounded. Duplicates (from the login/send race or a resend)
arrive close together, so a message older than the last 1024 can at worst be shown twice.
At-least-once delivery already allows that.

## E8. `n_unread` is a snapshot (§3.3)

`LOGIN_OK(n_unread)` is counted just before the session becomes `ACTIVE`. A message stored in
that short gap is still delivered, either live or in the replay, but it is not included in
`n`.

## E9. Login replay is paged; history is capped (§3.3 step 4, §2.1 R9, N3)

Found by test X2.

**Problem:** a user with more pending messages than the 256 KiB output buffer (e.g. 8000 ×
1 KB) overflowed the buffer during the login replay and was disconnected at **every** login.

**Implementation:**
- **Login replay is sent in pages** of up to 64 KiB. Where the page stopped is stored in the
  session. Each `ACK` from the client checks whether the output buffer has drained below 64 KiB;
  if so it sends the next page.
- **`getmessages` sends only the newest messages that fit in 128 KiB.** `HISTORY_END.count`
  carries the **total** number of matching messages, and the client prints
  `--- Conversation with bob (showing last 120 of 8000) ---`. When everything fits, the count
  equals the number of items, exactly as in the spec.

## E10. No logging while holding a lock (§6.10)

`log_event()` has its own small print mutex. To keep rule 1 ("never two locks at once"), the
code never logs while holding `sessions_mutex`, `db_mutex`, `jobq.mutex` or `expiry_mutex`.
Database functions return their error text in a `db_err_t`, and the caller logs it after the
call has released `db_mutex`.

## E11. Binaries are in `bin/` (§9, §10)

`server/` and `client/` are the source folders, so the programs cannot also be called
`./server` and `./client` in the same directory. `make` produces `bin/server` and `bin/client`.
In the Docker image they are `/app/server` and `/app/client`, as in the spec.

## E12. Per-connection read state lives in the event loop (§6.2)

The fd, the incremental frame parser and the 16-frame inbox are touched **only** by the
event-loop thread. They live in `eventloop.c` (`conn_t`), not in the shared `session_t`. That
makes it obvious which data needs `sessions_mutex` and which does not.

The parser reads **at most the rest of the current frame**. Once the inbox is full, the
remaining bytes stay in the kernel and TCP flow control slows the sender down (N3).

## E13. SQLite `synchronous = NORMAL` (§8.2)

Used together with WAL mode. It stays safe against application crashes and avoids an `fsync`
per message. Only a power loss can lose the last few committed messages.
