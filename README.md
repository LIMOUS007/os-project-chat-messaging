# Multi-User Client-Server Messaging

Operating Systems group project: a chat server and a command-line client in **C11** on
Linux, using POSIX sockets, pthreads and SQLite. On Windows it runs through Docker.

- **`bin/server`:** one process. It accepts many clients, tracks who is online, routes every
  message and stores everything in SQLite.
- **`bin/client`:** the CLI. It sends and receives messages in real time, reads and deletes
  history, and shows unread messages at login. It writes nothing to disk.
- **Extra features:** **F1** disappearing messages, **F2** read receipts, **F3** group chats.

The contract is [SPEC.md](SPEC.md) together with [SPEC-ERRATA.md](SPEC-ERRATA.md), which lists
the clarifications made during implementation.

---

## 1. Quick start

### Linux (or WSL / a lab machine)
```sh
make                                   # builds bin/server and bin/client (needs only gcc + make)
./bin/server --port 5555 --db chat.db --users users.txt
./bin/client                           # in other terminals
> connect 127.0.0.1 alice
```

### Windows, with Docker Desktop
```powershell
docker compose up --build server       # terminal 1
docker compose run --rm client         # terminals 2..6, one per user
> connect server alice                 # "server" is the compose service name
```
- **Client on another laptop:** connect to the Windows machine's LAN IP. Windows Firewall must
  allow inbound TCP 5555.
- **Client container, server running directly on the host:** `connect host.docker.internal alice`.
- **Build and test inside a Linux container** without installing anything:
  `.\dev.ps1 make test` (PowerShell) or `./dev.sh make test` (bash).

### Usernames
Only the names in `users.txt` can log in: alice, bob, carol, dave, eve, frank and grace. There
are no passwords.

---

## 2. Commands

| Command | What it does | Example output |
|---|---|---|
| `connect <ip>[:port] <user>` | log in (default port 5555) | `Connected as alice. 2 unread message(s):` |
| `sendmessage <user> <message>` | direct message (DM) | `Sent to bob (#42).` / `Stored for bob (offline) (#42).` |
| `sendtimed <user> <sec> <message>` | disappearing DM, 1–3600 s | `Sent to bob (#43), disappears in 30s.` |
| `getmessages <user\|#group>` | conversation history | see below |
| `deletemessages <user\|#group>` | delete a conversation **for both sides** | `Deleted 7 message(s) with bob.` |
| `getuserlist` | who is online | `Online (3): alice, bob, carol` |
| `creategroup #g <u1> [u2 ...]` | create a group (you are the creator) | `Created #g with alice, bob, carol.` |
| `addmember #g <user>` | add a member (creator only) | `Added dave to #g.` |
| `sendgroup #g <message>` | message the group | `Sent to #g (#50).` |
| `leavegroup #g` | leave; if the creator leaves, the group is dissolved | `Left #g.` |
| `mygroups` | your groups | `Your groups: #project, #os` |
| `help` | list the commands | |
| `shutdown` | log out and exit | `Disconnected.` |

**Messages pushed by the server** can appear at any time:
```
Message from alice: hi
Message from alice (disappears in 30s): secret
Message from alice in #project: hello team
[seen] bob saw your message #42: "hi"
[a message from alice has disappeared]
[alice added you to #project]
[#project was dissolved by alice]
Server shutting down...
```

**History** shows `sent`, `delivered` or `seen` on your own DMs:
```
--- Conversation with bob (3) ---
[#40 11:58] alice -> bob: hi                 (seen)
[#41 11:59] bob -> alice: hey
[#42 12:00] alice -> bob: meet at 5?         (delivered, disappears in 25s)
--- end ---
```

---

## 3. Architecture

```
 ┌──────────── client process (×N) ─────────────┐
 │  input thread ── poll(stdin, notify pipe)    │
 │  receiver thread ── recv frames, print, ACK  │
 └───────────────────┬──────────────────────────┘
                     │ TCP (one connection per client)
 ┌───────────────────▼──────────── server process ───────────────────────────┐
 │   EVENT-LOOP THREAD (main thread after start-up)                          │
 │   poll() over: listen fd, wake pipe, every client fd                      │
 │     • accept new connections         • read bytes → frames → inbox        │
 │     • write output buffers (POLLOUT) • close / free sessions              │
 │          │ jobq_try_push (never blocks)          ▲ wake pipe              │
 │          ▼                                       │                        │
 │   ┌──────────────┐   pop   ┌──────────────────┐  │ session_send()         │
 │   │  JOB QUEUE   │ ──────► │ WORKER POOL (×4) │──┘ append frames to       │
 │   │ bounded (64) │         │ run handlers     │    per-session out bufs   │
 │   └──────────────┘         └────────┬─────────┘                           │
 │                                     │ db_mutex                            │
 │   EXPIRY THREAD ── timed wait ──►   ▼                                     │
 │   SIGNAL THREAD ── sigwait ──►  ┌────────┐                                │
 │     (SIGINT/SIGTERM)            │ SQLite │ chat.db                        │
 │                                 └────────┘                                │
 │   SESSION TABLE [32]  (sessions_mutex)        users.txt (read-only)       │
 └───────────────────────────────────────────────────────────────────────────┘
```

### Thread model

| Process | Thread | Job |
|---|---|---|
| server | **event loop** (the main thread) | The **only** thread that touches sockets. Each round it builds a `pollfd` list (listening socket, wake pipe, clients) and then: accepts connections, reads frames into each client's inbox, writes output buffers, hands frames to workers, closes and frees sessions, and runs the shutdown phases. |
| server | **4 workers** | Pop a job, run its handler (database work, then append replies to output buffers), clear `busy`, wake the event loop. They never touch sockets. |
| server | **expiry** | Sleeps until the earliest `expires_at`, deletes expired messages and notifies recipients who are online (F1). |
| server | **signal** | Blocks in `sigwait(SIGINT, SIGTERM)`. When a signal arrives it sets the shutdown flag and wakes the event loop. |
| client | **input** (main) | Reads a line, sends the command, then waits up to 5 s on the notify pipe for the reply. |
| client | **receiver** | Receives frames and prints pushes and replies. For every `MSG` it skips duplicates and expired messages, displays it, sends `ACK`, and for a DM also `MARK_SEEN`. |

The server always runs **7 threads**, however many clients are connected.

### Source files

| Area | Files |
|---|---|
| Networking | `common/protocol.c` (frames), `common/net.c` (send_all, recv_all, frame parser), `server/eventloop.c` |
| Threading | `server/workers.c`, `server/jobqueue.c`, `server/expiry.c`, `server/signals.c`, `client/receiver.c` |
| Shared state | `server/session.c` (the session table) |
| Database | `server/db.c` (all SQL), `server/users.c` (users.txt) |
| Command handling | `server/handlers.c` (server side), `client/commands.c` (parsing typed lines) |
| Output | `common/log.c` (server log), `client/ui.c` (terminal) |

Every function in a header has a comment saying what it does, what its parameters are and
which locks it takes.

---

## 4. Synchronization

| Lock | Protects | Why it is needed |
|---|---|---|
| `sessions_mutex` (session.c) | the session table: state, `busy`, close requests, output buffers | The event loop, the 4 workers and the expiry thread all read and change sessions. |
| `db_mutex` (db.c) | the single SQLite connection | Five threads use the database, and each `db_*` call must run alone. |
| `jobq.mutex` + `not_empty` condvar | the job ring buffer | The textbook bounded buffer: one producer (the event loop) and 4 consumers. The producer only uses `try_push`, so it never waits. |
| `expiry_mutex` + `expiry_cond` | `expiry_gen`, the stop flag | Lets `SEND_TIMED` and shutdown wake the expiry thread. The **generation counter** stops a wake-up from being missed between "what is the next deadline?" and going to sleep. |
| `send_mutex` (client) | the client socket | Both client threads send (commands, and `ACK`/`MARK_SEEN`). |
| `ctx_mutex` (client) | the command waiting for a reply | The input thread sets it; the receiver reads and clears it. |
| `stdout_mutex` (client) | the terminal | Both client threads print. |
| print mutex (log.c) | one log line | Keeps lines from different threads separate. |

**Rules that make deadlock impossible:**
1. **Never hold two of these locks at once.** Each lock is always taken alone, so there can be
   no circular wait. A handler does its database work under `db_mutex`, releases it, and only
   then appends replies under `sessions_mutex`.
2. Never hold a lock across `send`, `recv`, `poll` or a wait on another lock. Even output is
   written by copying under the lock, unlocking, calling `send()`, then re-locking to remove
   what was sent.
3. Only the event loop touches sockets.
4. Never log while holding a lock.

**Other mechanisms:**
- **Session generation counters.** A handle is `(slot, gen)`. `gen` is bumped every time a slot
  is freed, so a late reply aimed at a disconnected client cannot reach the next client who
  gets the same slot.
- **`busy` flag.** Each client has at most one job in flight, so its commands run in the
  order they were sent. It is set *before* the job is queued (errata E1).
- **Wake pipe.** Other threads cannot interrupt `poll()`, so they write one byte to a pipe
  that `poll()` is watching.
- **Backpressure (N3).**
  - When a client's inbox holds 16 frames, the server stops reading that socket. The bytes
    stay in the kernel and TCP slows the sender down.
  - A client that stops reading is disconnected once its 256 KiB output buffer is full.
- **Signals.** SIGINT and SIGTERM are blocked in every thread and received by `sigwait` in an
  ordinary thread, so the usual restrictions on signal handlers don't apply. SIGPIPE is
  ignored and every send uses `MSG_NOSIGNAL`.

---

## 5. Database

`chat.db` is created automatically from the schema in SPEC §8.2. The connection uses
`foreign_keys=ON`, WAL journaling and `busy_timeout(2000)`.

| Table | Contents |
|---|---|
| `messages` | Every message. A DM has `recipient` set; a group message has `group_name` set. Also `state`, `expires_at`. |
| `groups` | Group name (`#...`) and creator. |
| `group_members` | Membership, with `joined_after_id`, the **join watermark**. |
| `group_delivery` | One row per group message per member: pending or delivered. |

**Delivery states (at-least-once):**
```
pending (0) ──ACK──► delivered (1) ──MARK_SEEN──► seen (2)      (seen: DMs only)
```
- **Store first.** Every message is stored as *pending* first, and only then sent if the
  recipient is online.
- **Delivered.** It becomes *delivered* only when the client's `ACK` arrives. A successful
  `send()` does not prove the message arrived.
- **Resent at login.** Anything still pending is sent again at the next login.
- **Duplicates.** The client hides duplicates within one session by message id.
- **Not exactly-once.** A message can be shown twice if the client displayed it but its `ACK`
  never reached the server.

---

## 6. Message flow, traced through the code

**`sendmessage bob hi` while bob is online:**
1. **alice's client:** `client/commands.c: parse_line` → `cmd_encode` builds a `SEND` frame.
   `client/main.c: do_remote_command` sends it under `send_mutex` and waits on the notify pipe.
2. **Server event loop:** `eventloop.c: read_from_client` → `parser_read` builds the frame
   into alice's inbox. Then `dispatch`: `session_reserve_job` (busy = true) → `jobq_try_push`.
3. **Worker:** `workers.c` → `handlers.c: handle_send` checks the input, then
   `db.c: db_store_dm` inserts the row as pending (id 42).
4. **Same worker:** `session_send_to_user("bob", MSG(42, ...))` appends to bob's output buffer,
   and `reply_ok` appends `OK(42, "sent")` to alice's. It then writes the wake pipe.
5. **Event loop:** `write_to_client` writes both buffers.
   - alice's receiver prints `Sent to bob (#42).`
   - bob's receiver (`receiver.c: on_msg`) prints `Message from alice: hi`, then sends
     `ACK(42)` and `MARK_SEEN(42)`.
6. **Worker:** `handle_ack` → `db_ack` sets the message to delivered. `handle_mark_seen` →
   `db_mark_seen` sets it to seen, then sends `RECEIPT` to alice, who prints
   `[seen] bob saw your message #42: "hi"`.

**Bob offline:** step 3 is the same. In step 4 bob has no session, so the row stays pending
and alice gets `Stored for bob (offline)`. At bob's next login, `handle_login` →
`send_pending_page` → `db_pending_for` sends it.

**Login** (`handlers.c: handle_login`):
1. Check `users.txt`.
2. Count pending messages.
3. `session_try_activate`: in one critical section, check for a duplicate login, mark the
   session ACTIVE and queue `LOGIN_OK(n)`.
4. Send pending messages in pages of up to 64 KiB. The client's ACKs pull the next page
   (errata E9).

**Disappearing message (F1):** `handle_send` stores the message with an `expires_at`, then
calls `expiry_notify()`. The expiry thread (`expiry.c`) wakes, re-reads the earliest deadline,
sleeps until it, runs `db_delete_expired`, and sends `EXPIRED` to the recipient if they are
online.

**Group message (F3):** `handle_group_send` → `db_store_group_msg`. One transaction inserts
the message, one `group_delivery` row per member except the sender, and returns that same
member list. The message is then pushed to each member who is online.

**Server shutdown (Ctrl+C or `docker stop`)** — errata E3:
1. The signal thread sets the shutdown flag.
2. The event loop closes the listening socket, stops reading and dispatching, stops the
   expiry thread and closes the job queue.
3. The workers finish the jobs already queued.
4. When every producer thread has exited, the event loop appends `SHUTDOWN` as the last frame
   of every client.
5. It flushes for at most 2 s, then closes every socket.
6. `main` joins the threads, closes the database and exits 0.

---

## 7. Building and testing

| Target | What it does |
|---|---|
| `make` | builds `bin/server` and `bin/client` |
| `make test` | unit tests (U1–U10), then integration and failure tests (I1–I12, X1–X13) |
| `make stress` | 20 bots × 100 DMs at once: each message stored once, shown once, and ends up *seen* |
| `make tsan` | rebuilds with ThreadSanitizer, then runs the queue and session tests and the stress test. It must report no data races (N2). |
| `make clean` | removes `build/` and `bin/` |
| `make docker` | builds the Docker image |

On Windows run any target inside the gcc container: `.\dev.ps1 make test`.

- **Integration tests** (`tests/integration/`) start a fresh server on a random port with a
  temporary database. Clients read commands from FIFOs and write to logs, and the scripts
  check the logs with `grep`.
- **`rawbot`** is a raw-protocol helper for the failure tests: clients that never ACK, never
  read, flood the server, or send broken frames.
- **TSan on WSL2/Docker Desktop:** the TSan runs use `setarch -R`, which turns off address
  randomisation, because newer kernels otherwise make TSan abort with
  `unexpected memory mapping`. `dev.sh`/`dev.ps1` also pass `--security-opt seccomp=unconfined`
  so that `setarch` is allowed.

---

## 8. Known limitations

- **No security.** Traffic is plain text with no encryption, and login is by username only.
- **One server, one database connection.** Database access is serialised through a single
  connection.
- **At-least-once delivery.** A message can be shown twice after a disconnect (SPEC §5).
- **Disappearing text can stay on screen.** It is removed from the server, but text already
  printed stays in the terminal's scrollback.
- **Expiry needs synchronised clocks.** Hiding expired messages on the client assumes the
  server and client clocks roughly agree.
- **Slow readers are disconnected.** A client that stops reading is cut off once its 256 KiB
  output buffer is full. Its messages stay pending.
- **Long histories are trimmed.** `getmessages` shows only the newest messages that fit in
  128 KiB, as `showing last N of M`.
- **Commands queue behind ACKs.** Each client's frames are handled in order, so a command sent
  right after a very large login replay waits behind that replay's ACKs.

---

## 9. Team

| Member | Primary ownership | Secondary |
|---|---|---|
| **A** | Protocol and networking: `common/protocol`, `common/net` | U1–U3 |
| **B** | Server core: `eventloop`, `session`, `jobqueue`, `workers`, `signals`, shutdown | U4, U5, X1–X4, X9, stress, tsan |
| **C** | Persistence: `db`, `users`, schema, history/delete | U6–U8, U10, X5, X8 |
| **D** | Client: `client/*` | U9, I1–I12, Docker |

The features are owned jointly: F1 by B + C, F2 by A + C + D, F3 by A + C + D. Every member
should be able to explain the whole design (SPEC §13).
