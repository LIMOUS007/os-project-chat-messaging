# Dockerfile — builds the server and client and runs the server (SPEC §10).
#
#   docker compose up --build server        # terminal 1
#   docker compose run --rm client          # one terminal per user
#
# Stage 1: compile with gcc (SQLite is vendored, so no internet is needed
# apart from pulling the base images).
FROM gcc:13 AS build
WORKDIR /src
COPY . .
RUN make clean all

# Stage 2: a small runtime image (same Debian release as gcc:13, so the
# binaries' glibc matches).
FROM debian:bookworm-slim
COPY --from=build /src/bin/server /src/bin/client /src/users.txt /app/
WORKDIR /app
VOLUME /data
EXPOSE 5555
# The server is PID 1 and handles SIGTERM itself, so `docker stop` triggers
# the normal "Server shutting down..." broadcast.
CMD ["./server", "--port", "5555", "--db", "/data/chat.db", "--users", "/app/users.txt"]
