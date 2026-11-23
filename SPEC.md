# Multi-User Client-Server Messaging — Specification

| | |
|---|---|
| **Course** | Operating Systems — Group Project |
| **Language / platform** | C (C11), Linux (POSIX sockets, pthreads); Windows via Docker |
| **Storage** | SQLite (vendored amalgamation) |
| **Status** | Rev 3 — frozen. Changes after this point need agreement from the whole team. |
| **Team** | Member A, Member B, Member C, Member D *(fill in names)* |

This document is the team's contract. Everyone builds against the same protocol, schema,
command syntax and ownership rules described here. It is also the starting point for the
required design document.

---

## 1. Overview and scope

A chat system with two programs:

- **`server`**: one process that accepts many clients, keeps track of who is online, routes
  every message, and stores all messages in SQLite.
- **`client`**: a command-line program. A user connects with a username, sends and receives
  messages in real time, reads and deletes history, and sees unread messages at login. The client
  stores nothing on disk.

**In scope:** everything the brief asks for, plus three extra features:

- **F1 Disappearing messages:** a message is deleted on the server after N seconds.
- **F2 Read receipts:** the sender is told when a direct message has been seen.
- **F3 Group chats:** messages sent to a named group of users.

**Out of scope:** passwords and authentication beyond the username whitelist, encryption, an
admin console, a GUI, client-side storage, multiple servers, Bluetooth.

### Glossary

| Term | Meaning |
|---|---|
| **Session** | One connected client: a slot in the server's session table. |
| **Frame** | One protocol message on the wire (§4). |
| **Job** | One received frame waiting to be handled, or being handled, by a worker thread. |
| **DM** | A direct message between two users. |
| **Pending / delivered / seen** | Delivery states of a message (§5). |
| **Expiry** | The time a disappearing message (F1) stops existing on the server. |
| **Join watermark** | The highest message ID at the moment a user joined a group. They only see group messages with a larger ID. |

### Interpretations of the brief

1. "If the user name is valid, it displays such message and client exists" is read as: valid
   name → success message; **invalid name → error message and the client exits**.
2. "Authenticates" means checking the username against `users.txt`. There are no passwords.
3. An **unknown** user (not in `users.txt`) is an error. A **known but offline** user is not an
   error: the message is stored and delivered at their next login.
4. `deletemessages <user>` deletes the conversation **for both users**, as written in the brief.
5. "Unread" means a message whose delivery state is still *pending* (§5).
6. The server is shut down with **Ctrl+C (SIGINT)** or **SIGTERM** (`docker stop`).

---

## 2. Requirements

Each requirement has an ID that the test plan (§11) refers to.

### 2.1 Core (from the brief)

| ID | Requirement |
|---|---|
| R1 | The server handles at least 5 concurrent clients. Hard cap `MAX_CLIENTS = 32`. |
| R2 | A connection beyond the cap is refused with `SERVER_FULL`. |
| R3 | Only usernames listed in `users.txt` can log in. Unknown name → error, client exits. |
| R4 | A second login for a user who is already online is rejected with `ALREADY_LOGGED_IN`. |
| R5 | The server keeps the list of online users. `getuserlist` shows it. |
| R6 | `sendmessage <user> <message>`: if the recipient is online, it shows immediately as `Message from <sender>: <text>`. |
| R7 | If the recipient is offline, the message is stored and shown at their next login (unread messages at login). |
| R8 | Sending to an unknown user, or to yourself, gives a clear error. |
| R9 | `getmessages <user>` shows every stored message exchanged with that user. Unknown user → error. |
| R10 | `deletemessages <user>` deletes every message to or from that user, for both users. Unknown user → error. |
| R11 | Client `shutdown` disconnects and exits. The server removes the user from the online list. |
| R12 | On SIGINT or SIGTERM the server sends `Server shutting down...` to every client. Clients print it and exit cleanly. The server releases all resources. |
| R13 | The server console prints live events (`[12:01:03] alice connected`, etc.). |
| R14 | The client writes nothing to disk. All client state is lost when it exits. |
| R15 | Clients never connect to each other. All traffic goes through the server. |

### 2.2 F1: Disappearing messages

| ID | Requirement |
|---|---|
| F1.1 | `sendtimed <user> <seconds> <message>`, where `seconds` is 1–3600. DMs only. |
| F1.2 | Expiry is a **server-side availability cutoff**. At `expires_at` the row is deleted, whether or not it was delivered. If the recipient was offline the whole time, they never see it. |
| F1.3 | The server never newly sends a message that has expired. Expired messages are left out of `getmessages` and of unread delivery. |
| F1.4 | If the recipient is online at expiry, they see `[a message from alice has disappeared]`. |
| F1.5 | Every `MSG` frame carries `expires_at`. If a message is still buffered when it expires, the client discards it instead of displaying it. Clocks on the same LAN are assumed roughly in sync. |
| F1.6 | **Known limitation:** text already printed in a terminal cannot be removed from its scrollback. The feature promises removal from the server, not from the screen. |

### 2.3 F2: Read receipts

| ID | Requirement |
|---|---|
| F2.1 | When the recipient's client displays a DM, the sender sees `[seen] bob saw your message #42: "first 30 chars..."` if they are online. |
| F2.2 | The state is saved. In `getmessages`, the sender's own messages show `sent`, `delivered` or `seen`. |
| F2.3 | DMs only. Groups track delivery per member but have no seen receipts. |

### 2.4 F3: Group chats

Group names start with `#` (e.g. `#project`), so they can never clash with usernames.

| ID | Requirement |
|---|---|
| F3.1 | `creategroup #g u1 u2 ...` creates a group. The creator is automatically a member. Every listed username is checked against `users.txt`; if any is unknown, the whole command is rejected with `UNKNOWN_USER`. A name that already exists gives `GROUP_EXISTS` (two clients creating the same name at once is settled by the table's primary key). |
| F3.2 | `sendgroup #g <message>` sends to all members except the sender. Members see `Message from alice in #g: <text>`. Offline members get it at their next login. The message is stored in the group history for everyone, the sender included. |
| F3.3 | `addmember #g <user>`: creator only. The user must exist. The new member only sees messages sent **after** they joined (join watermark). If online, they see `[alice added you to #g]`. |
| F3.4 | `leavegroup #g`: the member stops receiving messages, and `getmessages #g` then gives `NOT_MEMBER`. **If the creator leaves, the group is dissolved:** the group and its history are deleted, and online members see `[#g was dissolved by alice]`. |
| F3.5 | `getmessages #g`: members only. Shows the messages after the caller's join watermark. |
| F3.6 | `deletemessages #g`: creator only. Deletes the group's history **for all members**, matching the DM rule. The group itself stays. |
| F3.7 | `mygroups` lists the groups the user belongs to. |
| F3.8 | If membership changes while a message is being sent, the message goes to the member list as it was at the moment the message was stored (inside the same transaction). |

### 2.5 Non-functional

| ID | Requirement |
|---|---|
| N1 | Delivery is **at-least-once** (§5). Stored, unexpired messages are never lost because of a disconnect. |
| N2 | No data races or deadlocks. The stress test runs cleanly under ThreadSanitizer. |
| N3 | Memory use per client is bounded. A slow or flooding client cannot slow down other clients. |
| N4 | Builds with `make` on a clean Ubuntu install that has only `gcc` and `make`. |

---

## 3. Architecture

### 3.1 Process and thread overview

```
 ┌──────────── client process (×N) ─────────────┐
 │  input thread ── poll(stdin, wake pipe)      │
 │  receiver thread ── recv frames, print, ACK  │
 └───────────────────┬──────────────────────────┘
                     │ TCP (one connection per client)
 ┌───────────────────▼──────────── server process ───────────────────────────┐
 │                                                                           │
 │   EVENT-LOOP THREAD (main thread after init)                              │
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

The server always runs **7 threads**: the main thread (which sets everything up and then
becomes the event loop), 4 workers, 1 expiry thread and 1 signal thread. The number does not
grow with the number of clients.

### 3.2 Key design decisions

| Decision | Why |
|---|---|
| `poll()` event loop plus a worker pool, instead of one thread per client | Shows both server-side I/O multiplexing and multithreading, as the brief asks. A fixed thread count, one owner for every socket (no blocked `recv` at shutdown, no thread joining itself), and a textbook producer-consumer queue. |
| Length-prefixed binary frames | TCP is a byte stream. One `recv` can return half a message or two messages. A length prefix makes message boundaries unambiguous. |
| Pending → delivered → seen states, plus ACKs | The server cannot know a message arrived just because `send()` succeeded. ACKs make offline delivery and read receipts reliable. |
| One SQLite connection behind a mutex | Simple and correct for this workload. The lock that protects the database is easy to point to. |
| Vendored `sqlite3.c` | The submission builds anywhere with just `gcc`, which matters because the demo is built from the Moodle download. |
| Only the event loop touches sockets | No lock is ever held during network I/O. Session cleanup has a single owner. |

### 3.3 Data flows

**Login**
1. The client opens a TCP connection. The event loop accepts it and takes a FREE slot (state `LOGIN`).
2. The client sends `LOGIN(alice)`. A worker checks `users.txt`.
3. Holding `sessions_mutex` once, the worker checks that no ACTIVE session is named `alice`,
   marks this one ACTIVE, and appends `LOGIN_OK(n_unread)` to its output buffer. Doing these in
   one step guarantees that `LOGIN_OK` comes before any live message.
4. The worker fetches pending DMs and pending group messages (DB), and appends one `MSG` frame each.
5. The client prints `Connected as alice. 3 unread message(s):` followed by the messages, and ACKs each one.

**DM to an online user**
1. alice: `sendmessage bob hi` → `SEND(bob, "hi")`.
2. The worker inserts the row as `pending` and commits → gets id 42.
3. The worker looks up bob's session. He is ACTIVE, so it appends `MSG(42, alice, "", "hi", 0)`
   to his output buffer and writes the wake pipe. alice gets `OK(42, "sent")`.
4. The event loop writes the frame to bob's socket. bob's client prints
   `Message from alice: hi` and sends `ACK(42)` then `MARK_SEEN(42)`.
5. The worker sets state = delivered, then seen, and appends `RECEIPT(42, bob, "hi")` to alice's output buffer.

**DM to an offline user:** steps 1–2 as above. bob has no session, so the row stays `pending` and
alice gets `OK(42, "stored, bob is offline")`. At bob's next login the row is sent (Login, step 4).

**Group send:** one transaction inserts one `messages` row plus one `group_delivery` row per
member except the sender. After the commit, a `MSG` is appended for every online member.
Members ACK; each ACK updates their own `group_delivery` row.

**Expiry:** the expiry thread wakes at the earliest `expires_at`, deletes the expired rows in one
transaction, then sends `EXPIRED(id, sender)` to each recipient who is online.

**Server shutdown:** see §6.9.

---

## 4. Wire protocol

### 4.1 Frame format

```
+----------------+-------------+----------------------------+
| len : uint32   | type : uint8| payload : (len - 1) bytes  |
+----------------+-------------+----------------------------+
```

- `len` counts the **type byte plus the payload**. It does not count itself.
- Every multi-byte integer is in **network byte order** (big-endian): `u16`, `u32`, and `u64`
  (`htobe64`/`be64toh`, used for message IDs and timestamps).
- **String** = `u16 length` + that many bytes. No NUL terminator. UTF-8 is passed through untouched.
- **Limits:**
  - `len` is between 1 and 8192 and is checked **before** any memory is allocated.
  - Username: 1–32 characters from `[A-Za-z0-9_]`.
  - Group name: 2–32 characters, `#` followed by `[A-Za-z0-9_]`.
  - Message text: 1–1000 bytes.
- **Errors:**
  - A framing error (`len` out of range, or the connection closes in the middle of a frame)
    closes the connection.
  - A frame that is correctly framed but has a bad payload (truncated field, invalid name, text
    too long) gets `ERROR(BAD_REQUEST)` or `ERROR(TOO_LONG)`, and the connection stays open.
- `send_all()` and `recv_all()` loop until every byte is sent or received, so partial
  transfers are handled.

**Example:** `SEND(to="bob", text="hi")`

```
00 00 00 0A   len = 10  (1 type + 2+3 + 2+2)
02            type = SEND
00 03 62 6F 62   "bob"
00 02 68 69      "hi"
```

### 4.2 Client → server frames

| Code | Name | Payload | Server reply |
|---|---|---|---|
| 0x01 | `LOGIN` | str user | `LOGIN_OK` + pending `MSG`s, or `ERROR` (then closes the connection) |
| 0x02 | `SEND` | str to, str text | `OK(id)` / `ERROR` |
| 0x03 | `SEND_TIMED` | str to, u32 seconds, str text | `OK(id)` / `ERROR` |
| 0x04 | `GET_HISTORY` | str peer *(user or #group)* | `HISTORY_ITEM`×n, `HISTORY_END` / `ERROR` |
| 0x05 | `DELETE_HISTORY` | str peer | `OK(count)` / `ERROR` |
| 0x06 | `GET_USERS` | — | `USER_LIST` |
| 0x07 | `GROUP_CREATE` | str name, u16 n, str user×n | `OK` / `ERROR` |
| 0x08 | `GROUP_ADD` | str name, str user | `OK` / `ERROR` |
| 0x09 | `GROUP_SEND` | str name, str text | `OK(id)` / `ERROR` |
| 0x0A | `GROUP_LEAVE` | str name | `OK` / `ERROR` |
| 0x0B | `GROUP_LIST` | — | `GROUP_LIST_RESULT` |
| 0x0C | `ACK` | u64 msg_id | *none* |
| 0x0D | `MARK_SEEN` | u64 msg_id | *none* (may cause a `RECEIPT` to the sender) |
| 0x0E | `LOGOUT` | — | *none*; the server closes the session |

### 4.3 Server → client frames

| Code | Name | Payload |
|---|---|---|
| 0x81 | `LOGIN_OK` | u32 n_unread |
| 0x82 | `OK` | u64 ref *(message id, count, or 0)*, str text |
| 0x83 | `ERROR` | u8 code, str text |
| 0x84 | `MSG` | u64 msg_id, str from, str group *(empty for a DM)*, str text, u64 expires_at *(Unix seconds, 0 = never)* |
| 0x85 | `HISTORY_ITEM` | u64 msg_id, str from, str to *(user or #group)*, str text, u8 status, u64 created_at, u64 expires_at |
| 0x86 | `HISTORY_END` | u32 count |
| 0x87 | `USER_LIST` | u16 n, str user×n |
| 0x88 | `RECEIPT` | u64 msg_id, str reader, str preview *(≤30 bytes)* |
| 0x89 | `EXPIRED` | u64 msg_id, str from |
| 0x8A | `GROUP_EVENT` | str text |
| 0x8B | `SHUTDOWN` | str text |
| 0x8C | `GROUP_LIST_RESULT` | u16 n, str group×n |

`HISTORY_ITEM.status`: 0 = pending ("sent"), 1 = delivered, 2 = seen, 255 = not applicable (group messages).

### 4.4 Error codes

| Code | Name | When |
|---|---|---|
| 1 | `UNKNOWN_USER` | The username is not in `users.txt` |
| 2 | `ALREADY_LOGGED_IN` | Duplicate session |
| 3 | `SERVER_FULL` | All 32 slots are in use |
| 4 | `NOT_LOGGED_IN` | Any command other than `LOGIN` before logging in |
| 5 | `BAD_REQUEST` | Malformed payload, invalid name, or a second `LOGIN` |
| 6 | `NO_SUCH_GROUP` | The group does not exist |
| 7 | `NOT_MEMBER` | The caller is not in the group |
| 8 | `NOT_CREATOR` | A creator-only group command run by someone else |
| 9 | `GROUP_EXISTS` | `creategroup` with a name that is already taken |
| 10 | `SELF_MESSAGE` | Sending to, or reading history with, yourself |
| 11 | `TOO_LONG` | Text over 1000 bytes, or more than 3600 seconds |
| 12 | `INTERNAL` | A database error (also logged on the server) |

### 4.5 Request / response model

- The client sends **one command at a time** and waits up to 5 s for its reply (`OK`, `ERROR`,
  `USER_LIST`, `GROUP_LIST_RESULT`, `HISTORY_END`, or `LOGIN_OK`) before showing the next prompt.
  If no reply arrives, it prints `No response from server.`
- `MSG`, `RECEIPT`, `EXPIRED`, `GROUP_EVENT` and `SHUTDOWN` are **pushed** by the server and can
  arrive at any time.
- `ACK` and `MARK_SEEN` are sent by the receiver thread without waiting for a reply.
- A connection that has not sent `LOGIN` within 10 s is closed, so idle connections cannot hold slots.

---

## 5. Delivery model (at-least-once)

**States:** a DM's state is in `messages.state`. A group message has a separate state per member,
in `group_delivery.state`.

```
pending (0) ──ACK──► delivered (1) ──MARK_SEEN──► seen (2)      [seen: DMs only]
```

1. **Store, then deliver.** The worker stores the message as `pending` in one transaction (for a
   group: the message row plus a `group_delivery` row per member except the sender), commits,
   then finds online recipients and appends a `MSG` to each one's output buffer. It skips any
   message that has already expired.
2. **The client always processes a `MSG` in this order:**
   1. Receive the frame and check it is valid.
   2. If this `msg_id` was already displayed during this session (an in-memory set, never saved
      to disk), or `expires_at` has passed, **skip displaying it**.
   3. Otherwise display it.
   4. Send `ACK(msg_id)`. Duplicates are ACKed too.
   5. For a DM, also send `MARK_SEEN(msg_id)`.
3. **ACK, DM path:**
   `UPDATE messages SET state=1, delivered_at=now WHERE id=? AND recipient=? AND state=0`
   **ACK, group path:**
   `UPDATE group_delivery SET state=1 WHERE message_id=? AND username=? AND state=0`
   The handler chooses the path by whether the row has a `group_name`. Both updates are
   **idempotent**: repeating them changes nothing.
4. **MARK_SEEN (DMs only):** `UPDATE messages SET state=2, seen_at=now WHERE id=? AND
   recipient=? AND state<2`. If a row changed and the sender is online, a `RECEIPT` is
   appended to the sender's output buffer.
5. **At login** the server sends, in ID order:
   - every pending, unexpired DM addressed to the user;
   - every pending group message for the user (`group_delivery WHERE username=? AND state=0`,
     joined to `messages`).

**What is guaranteed (this exact wording goes in the design doc):**

- Stored, unexpired messages that are still pending are sent again after the user reconnects.
  This covers a failed send, output-buffer overflow, a disconnect, or a server restart.
- Message IDs let a client detect duplicates **within one session**.
- A message **can** be displayed twice if the client displayed it but its ACK never reached the
  server (for example, it disconnected immediately). A client stores nothing, so it cannot
  remember across sessions what it has already shown.
- The system is **at-least-once, not exactly-once**.

A login racing with a send can put the same message in the output buffer twice (once from the
live path, once from the pending fetch). The in-session duplicate check covers this.

In this CLI, `ACK` and `MARK_SEEN` are sent one after the other. The protocol keeps them separate
so that "delivered" and "seen" stay distinct states, which a GUI client could show differently.

---

## 6. Server design

### 6.1 Command line

```
./server [--port 5555] [--db chat.db] [--users users.txt]
```

The server binds `0.0.0.0:<port>` and logs to stdout with timestamps.

### 6.2 Data structures

```c
typedef struct { uint32_t slot, gen; } session_handle_t;

typedef enum { S_FREE, S_LOGIN, S_ACTIVE, S_CLOSING } session_state_t;

typedef struct {
    uint32_t        gen;          // bumped every time the slot is freed
    session_state_t state;
    char            username[33];
    int             fd;
    time_t          accepted_at;  // for the 10 s login timeout
    bytebuf_t       inbuf;        // partial incoming frame (≤ 8 KiB + header)
    frame_t        *inbox[16];    // decoded frames waiting to be dispatched (ring)
    int             inbox_head, inbox_len;
    bool            busy;         // a job for this session is queued or running
    bytebuf_t       outbuf;       // encoded frames waiting to be written (≤ 256 KiB)
} session_t;

session_t        sessions[MAX_CLIENTS];   // guarded by sessions_mutex
pthread_mutex_t  sessions_mutex;

typedef struct {
    session_handle_t h;
    char             username[33];  // copied: never a pointer into sessions[]
    frame_t         *req;           // copied request frame
} job_t;

typedef struct {                    // bounded ring buffer, capacity 64
    job_t           items[64];
    int             head, len;
    bool            closed;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
} jobqueue_t;
```

The user whitelist is a sorted array loaded from `users.txt` at startup. It is never modified,
so reading it needs no lock.

### 6.3 Event-loop thread (sole owner of every socket)

Each iteration:

1. Build the `pollfd` array:
   - the listen fd (`POLLIN`, not during shutdown);
   - the wake pipe's read end (`POLLIN`);
   - every non-FREE session fd: `POLLIN` if its inbox is not full, plus `POLLOUT` if its outbuf is not empty.
2. `poll()` with a timeout of 1 s, used for the login timeout and the shutdown deadline.
3. **Wake pipe readable:** drain it.
4. **Listen fd readable:** `accept()` and set `O_NONBLOCK`.
   - If there is a FREE slot: set state `LOGIN`.
   - Otherwise: try one direct non-blocking write of `ERROR(SERVER_FULL)`, then close.
5. **Client readable:** `recv` into `inbuf`, and move every complete frame into the inbox.
   - EOF, an error, or a framing error → **close the session** (§6.6).
6. **Client writable:** flush its outbuf (§6.7).
7. **Dispatch:** for every session with `!busy` and a non-empty inbox, call
   `jobq_try_push(head frame)`. On success, remove the frame from the inbox and set `busy = true`.
8. **Housekeeping:**
   - close `LOGIN` sessions older than 10 s;
   - free `CLOSING` sessions that are no longer busy (§6.6);
   - check the shutdown deadline.

Because only one job per session is in flight at a time (`busy`), **each client's commands run
in the order they were sent**. `ACK` and `MARK_SEEN` take the same path.

### 6.4 Job queue and backpressure

- **Producer:** the event loop, using `jobq_try_push()` only. **It never waits.** If the queue is
  full, the frame stays at the head of that session's inbox and is retried on the next iteration.
  Workers write the wake pipe after every pop, so the event loop retries soon.
- **Consumers:** workers call `jobq_pop()`, which waits on `not_empty`. It returns NULL once the
  queue is `closed` and empty, which is how workers exit.
- **Per-client backpressure:** when a session's inbox holds 16 frames, the event loop stops
  polling its fd for `POLLIN`. Unread bytes stay in the kernel's socket buffer, and TCP flow
  control slows the sender down. Memory per client is limited to the input buffer + 16 frames +
  the 256 KiB output buffer (requirement N3).

### 6.5 Worker threads (4)

```
loop:
    job = jobq_pop()                  // blocks; NULL → exit
    handle(job)                       // DB work under db_mutex, then session_send(...)
    lock sessions_mutex
        if sessions[job.h.slot].gen == job.h.gen: sessions[slot].busy = false
    unlock
    write(wake_pipe, 1 byte)
```

`session_send(handle, frame)` locks `sessions_mutex` and appends the encoded frame to the output
buffer **only if** the slot's `gen` matches and its state is `LOGIN` or `ACTIVE`. Otherwise it
silently drops the frame. If the output buffer would go over 256 KiB, the session is marked
`CLOSING` instead. Workers send to *other* users through `session_send_to_user(name, frame)`,
which finds the ACTIVE session by username under the same lock.

### 6.6 Session lifetime (single owner: the event loop)

1. **Accept:** the event loop takes a FREE slot → `LOGIN`.
2. **Login:** a worker checks and switches the slot to `ACTIVE` under `sessions_mutex`, which
   is what prevents duplicate logins.
3. **Close** (EOF, socket error, framing error, outbuf overflow, `LOGOUT`, login timeout,
   shutdown): the event loop marks the session `CLOSING`, closes the fd, stops reading,
   **discards inbox frames that were never dispatched**, and logs `alice disconnected`.
   `session_send` to a `CLOSING` slot is a no-op.
4. A job that is already queued or running for that session finishes normally. Its replies are
   dropped by the state/`gen` check, and the worker clears `busy`.
5. **Free:** the event loop frees the slot only when it is `CLOSING` and not `busy`, so no queued
   or running job can still refer to it. It **increments `gen`** and sets `FREE`. Any handle still
   holding the old `gen` now fails harmlessly.

There are no per-client threads, so no thread ever joins itself or stays blocked in `recv()`.

### 6.7 Output buffers and writing

All reads and changes to a session's `outbuf` (its bytes and offsets) happen while holding
`sessions_mutex`. When a socket is writable, the event loop:

1. Locks, copies up to 16 KiB of pending bytes into a local buffer, and unlocks.
2. Calls `send(fd, local, n, MSG_NOSIGNAL)` **without holding any lock**.
3. Locks again, advances the outbuf by the number of bytes actually sent, and unlocks.

A partial write, or `EAGAIN`/`EWOULDBLOCK`, keeps the unsent bytes for the next `POLLOUT`. Any
other error closes the session.

**SIGPIPE:** set `signal(SIGPIPE, SIG_IGN)` at startup and pass `MSG_NOSIGNAL` on every send.
Writing to a socket the client has closed then returns an error instead of killing the server.

### 6.8 Expiry thread (F1), protected against missed wake-ups

Shared state: `expiry_mutex`, `expiry_cond`, and an `expiry_gen` counter.

```
loop:
    lock expiry_mutex;  g = expiry_gen;  unlock
    next = db_next_expiry()              // SELECT MIN(expires_at); db_mutex only
    lock expiry_mutex
    while expiry_gen == g && !shutting_down && now() < next:
        pthread_cond_timedwait(&expiry_cond, &expiry_mutex, next or now+3600)
    unlock
    if shutting_down: exit
    expired = db_delete_expired(now())   // one transaction; returns (id, sender, recipient)
    for each: session_send_to_user(recipient, EXPIRED(id, sender)); log it
```

The `SEND_TIMED` handler, **after its transaction commits**, locks `expiry_mutex`, increments
`expiry_gen`, signals `expiry_cond`, and unlocks. If that happens between the thread's query and
its wait, `expiry_gen` no longer equals `g`, so the thread does not go to sleep and picks up the
new, earlier expiry.

### 6.9 Signals and shutdown

- At startup, before creating any thread, the main thread blocks SIGINT and SIGTERM with
  `pthread_sigmask`. Every thread inherits that mask.
- The **signal thread** calls `sigwait({SIGINT, SIGTERM})`. Because the signal is handled as
  ordinary code in an ordinary thread, the async-signal-safety restrictions of a signal handler
  do not apply.

**Shutdown sequence:**

1. The signal thread logs `shutdown requested`, sets `shutting_down = 1`, and writes the wake pipe.
2. The event loop closes the listen fd and appends `SHUTDOWN("Server shutting down...")` to every
   `LOGIN`/`ACTIVE` session. It stops dispatching new jobs.
3. The event loop keeps polling **only for `POLLOUT`** until every outbuf is empty or a
   **2-second deadline** passes, so a slow client cannot block shutdown. Then it closes every fd.
4. Close the job queue. Workers finish the job they are running and exit.
5. Signal `expiry_cond`; the expiry thread exits.
6. Join all threads, close the database, log `server stopped`, exit 0.

### 6.10 Locks

| Operation | Lock(s) |
|---|---|
| Login / logout / free slot | `sessions_mutex` |
| `getuserlist` | `sessions_mutex` |
| `session_send`, any output-buffer access | `sessions_mutex` |
| Job push / pop | `jobq.mutex` |
| Any database call | `db_mutex` |
| Expiry wait / notify | `expiry_mutex` |
| Expiry sweep | `db_mutex`, released, **then** `sessions_mutex` |
| Message delivery | DB insert under `db_mutex`, released, **then** `sessions_mutex` |

**Rules (they make deadlock impossible):**

1. **No two locks are ever held at the same time.** `db_mutex`, `sessions_mutex`, `jobq.mutex`
   and `expiry_mutex` are each taken alone.
2. No lock is ever held across `send()`, `recv()`, `poll()`, or a condition wait on another lock.
3. Only the event loop touches sockets.

### 6.11 Server console log

```
[12:00:00] server listening on 0.0.0.0:5555 (32 slots, db=chat.db)
[12:00:04] connection from 172.17.0.3:51234 (slot 0)
[12:00:04] alice logged in (2 unread)
[12:00:09] login rejected for 'mallory': unknown user
[12:00:15] alice -> bob #42 delivered
[12:00:20] alice -> carol #43 stored (offline)
[12:01:00] message #44 expired
[12:02:00] alice disconnected
[12:05:00] shutdown requested (SIGINT) — notifying 4 clients
[12:05:00] server stopped
```

---

## 7. Client design

### 7.1 Starting and connecting

```
$ ./client
> connect 192.168.1.20 alice          # also: connect server:5555 alice / host.docker.internal
Connected as alice. 2 unread message(s):
Message from bob: are you there?
Message from carol in #project: standup at 10
>
```

- `connect <server IP or hostname>[:port] <user>`. The default port is 5555; the address is
  resolved with `getaddrinfo`.
- Any other command before connecting prints `Not connected. Use: connect <server IP> <user>`.
- **Login failure** (unknown user, already logged in, server full, connection refused): print the
  error and **exit with status 1**.
- The `> ` prompt is printed only when stdin is a terminal, so integration tests can pipe commands in.

### 7.2 Threads

- **Input thread** (main): `poll()` on stdin and a self-pipe.
  - It reads a line, parses it, sends the frame, then waits on the self-pipe (up to 5 s) for
    the receiver to report that the reply has arrived.
  - A "quit" byte on the self-pipe (server shutdown or Ctrl+C) makes it clean up and exit.
- **Receiver thread:** loops on `recv_frame()`.
  - It prints pushes and replies.
  - For a `MSG`, it follows the order in §5 step 2.
  - Replies wake the input thread.
  - `SHUTDOWN`, or EOF from the server, prints the message and writes the "quit" byte.
- `send_mutex` protects the socket, because both threads send (commands, and `ACK`/`MARK_SEEN`).
- `stdout_mutex` serialises printing. A push that arrives while the user is typing does
  `\r` + clear line → print → reprint `> `.
- **Ctrl+C** sets a flag in a `sigaction` handler and writes the self-pipe (both async-signal-safe).
  It then behaves exactly like `shutdown`.

### 7.3 Commands and output

| Command | Frame | Success output | Error examples |
|---|---|---|---|
| `connect <ip>[:port] <user>` | `LOGIN` | `Connected as alice. N unread message(s):` then the messages | `Error: unknown user 'x'` → exit 1 |
| `sendmessage <user> <message>` | `SEND` | `Sent to bob (#42).` / `Stored for bob (offline) (#42).` | `Error: unknown user 'x'`, `Error: cannot message yourself` |
| `sendtimed <user> <sec> <message>` | `SEND_TIMED` | `Sent to bob (#43), disappears in 30s.` | `Error: seconds must be 1-3600` |
| `getmessages <user\|#group>` | `GET_HISTORY` | see below | `Error: unknown user 'x'`, `Error: you are not a member of #g` |
| `deletemessages <user\|#group>` | `DELETE_HISTORY` | `Deleted 7 message(s) with bob.` | `Error: only the creator can delete #g history` |
| `getuserlist` | `GET_USERS` | `Online (3): alice, bob, carol` | — |
| `creategroup #g <u1> [u2 ...]` | `GROUP_CREATE` | `Created #g with alice, bob, carol.` | `Error: group #g already exists` |
| `addmember #g <user>` | `GROUP_ADD` | `Added dave to #g.` | `Error: only the creator can add members` |
| `sendgroup #g <message>` | `GROUP_SEND` | `Sent to #g (#50).` | `Error: no such group #g` |
| `leavegroup #g` | `GROUP_LEAVE` | `Left #g.` / `Left #g (group dissolved).` | `Error: you are not a member of #g` |
| `mygroups` | `GROUP_LIST` | `Your groups: #project, #os` | — |
| `help` | — | command list | — |
| `shutdown` | `LOGOUT` | `Disconnected.` then exit 0 | — |

Everything after the recipient (or after the seconds) up to the end of the line is the message.

**Pushes:**

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

**`getmessages bob`:**

```
--- Conversation with bob (3) ---
[#40 11:58] alice -> bob: hi                (seen)
[#41 11:59] bob -> alice: hey
[#42 12:00] alice -> bob: meet at 5?        (delivered, disappears in 25s)
--- end ---
```

The status (`sent` / `delivered` / `seen`) is shown only on the caller's own DMs. With no
messages, it prints `No messages with bob.`

---

## 8. Storage (SQLite)

### 8.1 Files

- `users.txt`: one username per line. Blank lines and lines starting with `#` are ignored.
  Invalid names are rejected at startup with an error. The file is loaded once.
- `chat.db`: created automatically if missing. The schema below is applied with `CREATE TABLE IF NOT EXISTS`.

### 8.2 Schema

```sql
PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;          -- plus sqlite3_busy_timeout(db, 2000)

CREATE TABLE IF NOT EXISTS groups (
    name        TEXT PRIMARY KEY,                 -- '#project'
    creator     TEXT NOT NULL,
    created_at  INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS messages (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    sender       TEXT    NOT NULL,
    recipient    TEXT,                            -- set for DMs
    group_name   TEXT REFERENCES groups(name) ON DELETE CASCADE,  -- set for group msgs
    body         TEXT    NOT NULL,
    created_at   INTEGER NOT NULL,
    state        INTEGER NOT NULL DEFAULT 0,      -- DMs: 0 pending, 1 delivered, 2 seen
    delivered_at INTEGER,
    seen_at      INTEGER,
    expires_at   INTEGER,                         -- NULL = never (F1, DMs only)
    CHECK ((recipient IS NULL) <> (group_name IS NULL))
);

CREATE TABLE IF NOT EXISTS group_members (
    group_name      TEXT NOT NULL REFERENCES groups(name) ON DELETE CASCADE,
    username        TEXT NOT NULL,
    joined_after_id INTEGER NOT NULL,             -- join watermark
    PRIMARY KEY (group_name, username)
);

CREATE TABLE IF NOT EXISTS group_delivery (
    message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
    username   TEXT    NOT NULL,
    state      INTEGER NOT NULL DEFAULT 0,        -- 0 pending, 1 delivered
    PRIMARY KEY (message_id, username)
);

CREATE INDEX IF NOT EXISTS idx_msg_recipient ON messages(recipient, state);
CREATE INDEX IF NOT EXISTS idx_msg_expiry    ON messages(expires_at) WHERE expires_at IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_msg_group     ON messages(group_name, id);
CREATE INDEX IF NOT EXISTS idx_gd_user       ON group_delivery(username, state);
```

A group message is stored **once** in `messages`. Each member's delivery state is a row in
`group_delivery`.

### 8.3 Key operations (`db.h`)

| Function | SQL outline | Transaction? |
|---|---|---|
| `db_store_dm` | `INSERT INTO messages(...)` → returns id | single statement |
| `db_store_group_msg` | check membership, `INSERT` message, `INSERT` a `group_delivery` row for each member except the sender | **yes** |
| `db_pending_for` | pending unexpired DMs `UNION ALL` pending group rows, `ORDER BY id` | read |
| `db_ack` / `db_mark_seen` | see §5 | single statement |
| `db_history_dm` | `WHERE ((sender=me AND recipient=p) OR (sender=p AND recipient=me)) AND (expires_at IS NULL OR expires_at > now) ORDER BY id` | read |
| `db_history_group` | `WHERE group_name=? AND id > my joined_after_id ORDER BY id` | read |
| `db_delete_dm` | `DELETE` using the same condition as the history query → returns count | single statement |
| `db_delete_group_history` | check creator, `DELETE FROM messages WHERE group_name=?` (cascades to `group_delivery`) | **yes** |
| `db_group_create` | `INSERT groups`, `INSERT` members with `joined_after_id = COALESCE(MAX(messages.id),0)` | **yes** |
| `db_group_add` / `db_group_leave` | insert / delete a membership. If the creator leaves, `DELETE FROM groups` (cascades) | **yes** |
| `db_next_expiry` / `db_delete_expired` | `SELECT MIN(expires_at)` / select-then-delete `WHERE expires_at <= now` | **yes** (delete) |

**Rules:**

- Every call takes `db_mutex` for its whole duration and releases it before returning. Callers
  never hold `db_mutex` while doing anything else.
- Every SQLite return code is checked. On failure, the transaction is rolled back, the error is
  logged, and the client gets `ERROR(INTERNAL)`.
- WAL mode is used for crash safety. With a single connection behind a mutex it does not add
  parallelism; it doesn't need to at this scale.

---

## 9. Source layout and build

```
chat/
├── common/
│   ├── protocol.c/.h     frame encode/decode, type & error codes, field readers/writers
│   ├── net.c/.h          send_all/recv_all, recv_frame, nonblocking helpers, connect/listen
│   └── log.c/.h          timestamped logging
├── server/
│   ├── main.c            arg parsing, init, start threads, become event loop, teardown
│   ├── eventloop.c/.h    poll loop, accept, read/write, dispatch, housekeeping
│   ├── session.c/.h      session table, handles, session_send, lifecycle
│   ├── jobqueue.c/.h     bounded producer-consumer queue
│   ├── workers.c/.h      worker pool
│   ├── handlers.c/.h     one function per request type (command handling)
│   ├── db.c/.h           all SQLite access (persistence)
│   ├── users.c/.h        users.txt loading and lookup
│   ├── expiry.c/.h       F1 expiry thread
│   └── signals.c/.h      signal mask + signal thread + shutdown flag
├── client/
│   ├── main.c            startup, input thread loop
│   ├── commands.c/.h     line parsing → frames (pure, unit-testable)
│   ├── receiver.c/.h     receiver thread, dedupe set, ACK/MARK_SEEN
│   └── ui.c/.h           prompt, stdout_mutex, output formatting
├── third_party/sqlite3.c/.h
├── tests/
│   ├── unit/             test.h + test_*.c
│   ├── integration/      lib.sh, run_all.sh, one script per scenario
│   └── stress/           stress.c (bot clients)
├── Makefile  Dockerfile  docker-compose.yml  users.txt  README.md
```

This follows the brief's "separate files for networking (`net`, `eventloop`), threading
(`workers`, `jobqueue`, `expiry`, `signals`), database (`db`) and command handling (`handlers`,
`commands`)".

**Makefile targets:**

| Target | What it does |
|---|---|
| `all` (default) | builds `server` and `client` |
| `test` | builds and runs unit tests, then integration tests |
| `stress` | runs the stress test |
| `tsan` | rebuilds with `-fsanitize=thread` and runs the stress test |
| `clean` | removes build output |
| `docker` | builds the Docker image |

Compiler flags: `-std=c11 -Wall -Wextra -pthread -g -D_GNU_SOURCE`. `sqlite3.c` is compiled
with `-DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION`. Every function in a header gets a
comment describing what it does, its parameters, and which locks it takes.

---

## 10. Running on Windows with Docker

**Dockerfile (outline):**

```dockerfile
FROM gcc:13 AS build
WORKDIR /src
COPY . .
RUN make clean all

FROM debian:bookworm-slim
COPY --from=build /src/server /src/client /src/users.txt /app/
WORKDIR /app
VOLUME /data
EXPOSE 5555
CMD ["./server", "--port", "5555", "--db", "/data/chat.db", "--users", "/app/users.txt"]
```

**docker-compose.yml (outline):**

```yaml
services:
  server:
    build: .
    ports: ["5555:5555"]
    volumes: ["chatdata:/data"]
  client:
    build: .
    command: ["./client"]
    stdin_open: true
    tty: true
    profiles: ["client"]
volumes:
  chatdata:
```

**Usage:**

```
docker compose up --build server                 # terminal 1
docker compose run --rm client                   # terminals 2..6, one per user
> connect server alice                           # same compose network
```

- **Client on another laptop:** connect to the Windows host's LAN IP. Windows Firewall must allow
  inbound TCP on port 5555.
- **Client container talking to a server run directly on the host:** use `host.docker.internal`.
- The server runs as PID 1 in its container and handles SIGTERM itself, so `docker stop` triggers
  the normal shutdown broadcast.
- `chat.db` is on the `chatdata` volume, so messages survive container restarts.
- **Before the demo:** download the submitted ZIP from Moodle, run `docker compose build` with an
  internet connection (it pulls the base images), and test it. On a Linux lab machine, plain
  `make` also works.

---

## 11. Testing

### 11.1 Unit tests (`make test`)

A small harness, `tests/unit/test.h`, provides `CHECK(cond)`, `CHECK_EQ(a,b)` and a summary line.
Each `test_*.c` builds into its own executable.

| ID | File | Covers |
|---|---|---|
| U1 | `test_protocol.c` | encode/decode round trip for every frame type; `u64` byte order; string bounds |
| U2 | `test_protocol.c` | `len` = 0, `len` > 8192, truncated payload → errors, no allocation |
| U3 | `test_net.c` | `recv_frame` over a `socketpair` with data written 1 byte at a time, and two frames in one write |
| U4 | `test_jobqueue.c` | 4 producer + 4 consumer threads, 100k items: none lost or duplicated; `try_push` on a full queue returns false without blocking; close wakes consumers |
| U5 | `test_session.c` | duplicate login rejected; `session_send` with a stale `gen` is dropped; output-buffer overflow → CLOSING |
| U6 | `test_db.c` (`:memory:`) | store / pending / ACK (DM and group paths) / MARK_SEEN idempotency; history order; delete-for-both counts |
| U7 | `test_db.c` | expiry: `next_expiry`, `delete_expired`; expired rows excluded from history and pending |
| U8 | `test_db.c` | groups: create with an unknown member fails atomically; watermark hides old messages; creator leave cascades; fan-out excludes sender |
| U9 | `test_commands.c` | client parser: every command, missing arguments, message with spaces, bad seconds, `#` names |
| U10 | `test_users.c` | `users.txt` parsing: comments, blank lines, invalid names |

### 11.2 Integration tests (`tests/integration/run_all.sh`)

Each scenario starts a fresh server on a random port with a temporary database. Clients run in
the background, take commands from FIFOs (via helpers in `lib.sh`), and write output to log
files; the script then checks those logs with `grep`.

| ID | Scenario |
|---|---|
| I1 | Login OK; unknown user → error and exit code 1; duplicate login rejected |
| I2 | Live DM shows `Message from alice: hi` |
| I3 | DM to an offline user → shown at that user's next login |
| I4 | `getmessages` order and statuses; unknown user → error |
| I5 | `deletemessages` removes the conversation for both sides |
| I6 | `getuserlist` before and after a client `shutdown` |
| I7 | 6 clients connected and messaging at once; in a test build with `MAX_CLIENTS=6`, a 7th connection gets `SERVER_FULL` |
| I8 | `sendtimed 2`: shows the disappearance notice; missing from history after expiry |
| I9 | Read receipt arrives live; offline sender sees `seen` in history later |
| I10 | Group create / send / fan-out / offline member at login / late joiner can't see old messages / creator delete / creator leave dissolves the group |
| I11 | SIGINT to the server → every client prints `Server shutting down...` and exits 0; server exits 0 |
| I12 | SIGTERM gives the same result as I11 |

### 11.3 Failure and concurrency tests

| ID | Scenario | Expected |
|---|---|---|
| X1 | Recipient's connection is killed while a message is in flight (before ACK) | Message stays pending and is shown at the next login |
| X2 | A client that never reads its socket while others send to it | Its outbuf fills → it is disconnected; other clients unaffected; its messages still pending |
| X3 | Server shut down while clients are connected and sending | Broadcast reaches everyone; server exits within ~2 s |
| X4 | Login at the same moment as messages to that user (repeated 200×) | Every message shown exactly once (dedupe) and none lost |
| X5 | Timed message expires while the recipient is offline | Never shown at login |
| X6 | A member leaves a group while another member sends in a loop | No crash; messages after the leave not delivered to them |
| X7 | Two clients run `creategroup #g` at the same moment | Exactly one succeeds; the other gets `GROUP_EXISTS` |
| X8 | A simulated SQLite error (test hook makes `db_store_dm` fail) | Client gets `ERROR(INTERNAL)`; server keeps running |
| X9 | One client floods 10,000 commands without reading replies | Job queue never blocks the event loop; other clients stay responsive; the flooder is slowed by backpressure |
| X10 | `sendtimed` with a short expiry arrives just after the expiry thread queried MIN | Expires on time, not at the older deadline |
| X11 | A group message is ACKed | The `group_delivery` row becomes delivered; it is not resent at the next login |
| X12 | `creategroup #g alice nobody` | Rejected; no group or members created |

### 11.4 Stress test (`make stress`, `make tsan`)

`stress.c` starts 20 bot clients, each sending 100 DMs to random other bots at the same time.
With **no forced disconnects**, it checks that:

- every message is stored exactly once;
- every recipient displays each ID exactly once;
- every DM ends in state `seen`.

The "no duplicate display" claim applies to normal operation only (see §5). `make tsan` runs the
same test under ThreadSanitizer and must report no data races (requirement N2).

### 11.5 Requirement coverage

| Requirement | Tests |
|---|---|
| R1, R2 | I7, stress |
| R3, R4 | I1, U5, U10 |
| R5, R11 | I6 |
| R6 | I2 |
| R7 | I3, X1 |
| R8 | I1, I4 |
| R9 | I4, U6 |
| R10 | I5, U6 |
| R12 | I11, I12, X3 |
| R13 | manual check during the demo; log lines checked in I1–I3 |
| R14, R15 | code review: the client has no file I/O and only connects to the server |
| F1.x | I8, U7, X5, X10 |
| F2.x | I9, U6 |
| F3.x | I10, U8, X6, X7, X11, X12 |
| N1 | X1, X4, stress |
| N2 | `make tsan`, U4 |
| N3 | X2, X9 |
| N4 | clean Docker build and fresh-VM build |

---

## 12. OS concepts used

| Brief concept | Where in this project |
|---|---|
| Process management | Server and each client are separate processes; server exits cleanly and releases its resources; the server runs as PID 1 in a container |
| Concurrency and threading | Server: event loop, 4 workers, expiry and signal threads (`pthread_create/join`). Client: input + receiver threads |
| Synchronization | `sessions_mutex`, `db_mutex`, job-queue mutex + condvar (bounded buffer, producer-consumer), expiry condvar with a generation counter (no missed wake-ups), session generation counters (preventing use of a reused slot), the no-nesting lock rule |
| IPC | TCP sockets with a custom framed protocol; wake pipe (worker/signal → event loop); self-pipe in the client |
| I/O management | Server: `poll()` over all sockets, non-blocking sockets, partial reads/writes, `EAGAIN`, backpressure. Client: `poll()` on stdin and a pipe |
| File system and persistence | SQLite file with transactions, foreign keys and WAL; `users.txt` |
| Signal handling | SIGINT/SIGTERM via a `sigwait` thread → broadcast shutdown; SIGPIPE ignored plus `MSG_NOSIGNAL`; client SIGINT handler using the self-pipe |

---

## 13. Team responsibilities

| Member | Primary ownership | Secondary |
|---|---|---|
| **A** | Protocol and networking: `common/protocol`, `common/net` | Unit tests U1–U3; client↔server communication tests |
| **B** | Server core: `eventloop`, `session`, `jobqueue`, `workers`, `signals`, shutdown | U4, U5; failure tests X1–X4, X9; stress test and `make tsan` |
| **C** | Persistence: `db`, `users`, schema, history/delete | U6–U8, U10; X5, X8 |
| **D** | Client: `client/*` (commands, receiver, UI) | U9; integration tests I1–I12; Docker |

**Features are owned jointly:**

- **F1 Expiry:** B + C (expiry thread + DB queries)
- **F2 Receipts:** A + C + D (protocol frames + DB states + client order)
- **F3 Groups:** A + C + D (frames + schema/transactions + commands); B reviews the fan-out locking

**Ground rules:**

- Every change is reviewed by at least one other member.
- Ownership says who is responsible for a file, not who is allowed to touch it.
- **Every member must be able to explain the whole design**, because the demo (15%) includes
  questions to any member about threads, synchronization and data structures.

---

## 14. Build order (3-week timeline)

| Phase | When | Deliverable |
|---|---|---|
| **1. Vertical slice** | Week 1 | Client A → server → client B: framing, `LOGIN` with `users.txt`, duplicate check, live `SEND`/`MSG`, 5 clients at once. Event loop + workers in their simplest form. **Send the group and work split to the instructor.** |
| **2. Core brief** | Week 2 | SQLite, pending/ACK delivery, offline delivery, `getmessages`, `deletemessages`, `getuserlist`, client `shutdown`, server shutdown broadcast, event log. Unit tests for these. **Submit the high-level design (by day 14).** |
| **3. Extra features** | Week 3, first half | F1 → F2 → F3, in that order. Each one is merged only once the core tests still pass. |
| **4. Stabilize and submit** | Week 3, by day 19 | Integration, failure and stress tests, TSan clean, Docker, final design document, presentation. Rehearse the demo **from the ZIP downloaded from Moodle**. Submit the code one day before the presentation. |

If time runs short, cut from the end: F3, then F2. The core brief is never at risk.

---

## 15. Demo script

1. Briefly explain the design, using the architecture diagram, and who built what.
2. Start the server (Member B). Four members plus one extra terminal connect as 5 clients.
3. Show `getuserlist`, live DMs both ways, and a DM to an offline user followed by that user's
   login showing it as unread.
4. Show `getmessages`, then `deletemessages`, with history now empty on both sides.
5. Try a duplicate login and an unknown user to show the errors.
6. F1: `sendtimed bob 10 ...` → the disappearance notice → gone from history.
7. F2: the `[seen]` receipt, and statuses in history.
8. F3: create a group, send to it, add a late joiner, show the history watermark.
9. Ctrl+C the server → every client prints `Server shutting down...` and exits.
10. Run `make test` (and `make stress` if time allows).

---

## 16. Known limitations

- Traffic is plain text with no encryption. Authentication is by username only.
- There is a single server process, and database access is serialised through one connection.
- Delivery is at-least-once with in-session duplicate detection; a message can be displayed
  twice after a disconnect (§5).
- Disappearing messages cannot be removed from a terminal's scrollback once printed.
- Expiry suppression on the client assumes server and client clocks are roughly in sync.
- A client that stops reading is disconnected once its 256 KiB output buffer is full.
