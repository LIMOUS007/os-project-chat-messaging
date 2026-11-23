# Makefile — Multi-User Client-Server Messaging (SPEC §9)
#
#   make            build bin/server and bin/client
#   make test       unit tests, then integration tests
#   make stress     20-bot stress test
#   make tsan       rebuild with ThreadSanitizer and run the stress test + unit tests
#   make clean      remove build output
#   make docker     build the Docker image
#
# Each build variant has its own directory, so they never overwrite each other:
#   build/rel   normal build (copied to bin/server and bin/client)
#   build/test  MAX_CLIENTS=6 + test hooks, used by integration tests I7 and X8
#   build/tsan  -fsanitize=thread

CC      ?= gcc
BUILD   ?= build/rel
EXTRA   ?=
CFLAGS   = -std=c11 -Wall -Wextra -pthread -g -D_GNU_SOURCE \
           -Icommon -Iserver -Iclient -Ithird_party $(EXTRA)
SQLITE_CFLAGS = -O2 -g -pthread -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -w $(EXTRA)
LDLIBS   = -pthread -lm

COMMON_SRC = common/protocol.c common/net.c common/log.c
SERVER_LIB = server/session.c server/jobqueue.c server/workers.c server/handlers.c \
             server/db.c server/users.c server/expiry.c server/signals.c server/eventloop.c
CLIENT_LIB = client/commands.c client/receiver.c client/ui.c

OBJ = $(BUILD)/obj
obj = $(patsubst %.c,$(OBJ)/%.o,$(1))
COMMON_OBJ = $(call obj,$(COMMON_SRC))
SERVER_OBJ = $(call obj,$(SERVER_LIB))
CLIENT_OBJ = $(call obj,$(CLIENT_LIB))
SQLITE_OBJ = $(OBJ)/third_party/sqlite3.o

UNIT_TESTS = test_protocol test_net test_jobqueue test_session test_db test_commands test_users
UNIT_BINS  = $(addprefix $(BUILD)/tests/,$(UNIT_TESTS))

.PHONY: all bins unit test stress tsan clean docker

.DEFAULT_GOAL := all

# The programs go in bin/ (the names server/ and client/ are the source folders).
all: bin/server bin/client

bin/server: $(BUILD)/server
	@mkdir -p bin
	cp $< $@
bin/client: $(BUILD)/client
	@mkdir -p bin
	cp $< $@

bins: $(BUILD)/server $(BUILD)/client $(BUILD)/rawbot $(BUILD)/stress

# ---------- compiling ----------

$(OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(SQLITE_OBJ): third_party/sqlite3.c
	@mkdir -p $(dir $@)
	$(CC) $(SQLITE_CFLAGS) -c $< -o $@

# ---------- programs ----------

$(BUILD)/server: $(OBJ)/server/main.o $(SERVER_OBJ) $(COMMON_OBJ) $(SQLITE_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/client: $(OBJ)/client/main.o $(CLIENT_OBJ) $(COMMON_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/rawbot: $(OBJ)/tests/integration/rawbot.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/stress: $(OBJ)/tests/stress/stress.o $(COMMON_OBJ) $(SQLITE_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

# ---------- unit tests ----------

$(UNIT_BINS): | $(BUILD)/tests
$(BUILD)/tests:
	mkdir -p $@

$(BUILD)/tests/test_protocol: $(OBJ)/tests/unit/test_protocol.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)
$(BUILD)/tests/test_net: $(OBJ)/tests/unit/test_net.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)
$(BUILD)/tests/test_jobqueue: $(OBJ)/tests/unit/test_jobqueue.o $(OBJ)/server/jobqueue.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)
$(BUILD)/tests/test_session: $(OBJ)/tests/unit/test_session.o $(OBJ)/server/session.o \
                             $(OBJ)/server/jobqueue.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)
$(BUILD)/tests/test_db: $(OBJ)/tests/unit/test_db.o $(OBJ)/server/db.o $(COMMON_OBJ) $(SQLITE_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)
$(BUILD)/tests/test_commands: $(OBJ)/tests/unit/test_commands.o $(CLIENT_OBJ) $(COMMON_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)
$(BUILD)/tests/test_users: $(OBJ)/tests/unit/test_users.o $(OBJ)/server/users.o $(COMMON_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

unit: $(UNIT_BINS)
	@set -e; for t in $(UNIT_BINS); do echo "== $$t"; $$t; done

# ---------- top-level targets ----------

test: all
	$(MAKE) BUILD=build/rel bins unit
	$(MAKE) BUILD=build/test EXTRA="-DMAX_CLIENTS=6 -DCHAT_TEST_HOOKS" build/test/server
	bash tests/integration/run_all.sh

stress: all
	$(MAKE) BUILD=build/rel bins
	bash tests/stress/run.sh build/rel

tsan:
	$(MAKE) BUILD=build/tsan EXTRA="-fsanitize=thread -O1" bins build/tsan/tests/test_jobqueue \
	        build/tsan/tests/test_session
	TSAN_OPTIONS="halt_on_error=1" build/tsan/tests/test_jobqueue
	TSAN_OPTIONS="halt_on_error=1" build/tsan/tests/test_session
	bash tests/stress/run.sh build/tsan

clean:
	rm -rf build bin

docker:
	docker build -t chat .

-include $(shell find build -name '*.d' 2>/dev/null)
