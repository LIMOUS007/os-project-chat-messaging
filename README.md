# Multi-User Client-Server Messaging

Operating Systems group project: a C11 / POSIX chat server and command-line client.

- **Server:** `poll()` event loop, 4 worker threads, an expiry thread and a signal thread, with SQLite storage.
- **Client:** two-thread CLI (input + receiver).
- **Extra features:** disappearing messages (F1), read receipts (F2), group chats (F3).

The full design is in [SPEC.md](SPEC.md). This README will cover architecture, threads,
synchronization, the database, message flow, build steps and CLI commands as the
implementation lands.

## Status

Work in progress.

- [x] Environment check (Docker `gcc:13`, ThreadSanitizer), SQLite 3.54.1 vendored in `third_party/`
- [x] `common/`: protocol, networking, logging
- [x] `server/`: session table, job queue, users whitelist
- [ ] `server/`: db, expiry, signals, workers, handlers, event loop, main
- [ ] `client/`
- [ ] Makefile, unit, integration and stress tests
- [ ] Docker / compose, full README, SPEC-ERRATA.md

## Building (once the Makefile lands)

On Linux: `make`. On Windows, use Docker:

```
docker run --rm -v "${PWD}:/src" -w /src gcc:13 make
```
