# pinback Makefile.
#
# Style mirrors ~/ds4/Makefile: portable C99, -Wall -Wextra, no exotic
# deps, pthreads, BSD sockets, poll. macOS and Linux only. TLS is
# upstream (Caddy/Tailscale), so pinback-server speaks plain HTTP/1.1.

CC ?= cc
UNAME_S := $(shell uname -s)

DEBUG_FLAGS ?= -g
WARN_FLAGS  ?= -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
               -Wmissing-prototypes -Wno-unused-parameter
OPT_FLAGS   ?= -O2
STD_FLAGS   ?= -std=c99 -D_POSIX_C_SOURCE=200809L

CFLAGS  ?= $(OPT_FLAGS) $(DEBUG_FLAGS) $(WARN_FLAGS) $(STD_FLAGS) -Isrc

ifeq ($(UNAME_S),Darwin)
CFLAGS  += -D_DARWIN_C_SOURCE
endif

LDLIBS ?= -lpthread

SRC_DIR  := src
TEST_DIR := tests
TOOLS    := tools

CORE_SRCS := \
	$(SRC_DIR)/util.c \
	$(SRC_DIR)/log.c \
	$(SRC_DIR)/http.c \
	$(SRC_DIR)/event_log.c \
	$(SRC_DIR)/workspace.c \
	$(SRC_DIR)/snapshot.c \
	$(SRC_DIR)/agent.c \
	$(SRC_DIR)/handlers.c \
	$(SRC_DIR)/static_assets.c

CORE_OBJS := $(CORE_SRCS:.c=.o)

MAIN_BIN  := pinback-server
MAIN_SRC  := $(SRC_DIR)/pinback.c
MAIN_OBJ  := $(MAIN_SRC:.c=.o)

FAKE_BIN  := $(TOOLS)/fake-ds4-agent
FAKE_SRC  := $(TOOLS)/fake-ds4-agent.c
FAKE_OBJ  := $(FAKE_SRC:.c=.o)

TEST_SRCS := \
	$(TEST_DIR)/test_util.c \
	$(TEST_DIR)/test_event_log.c \
	$(TEST_DIR)/test_http.c \
	$(TEST_DIR)/test_workspace.c \
	$(TEST_DIR)/test_agent.c \
	$(TEST_DIR)/test_main.c
TEST_OBJS := $(TEST_SRCS:.c=.o)
TEST_BIN  := $(TEST_DIR)/run_tests

.PHONY: all clean test smoke help embed

all: $(MAIN_BIN) $(FAKE_BIN)

# Regenerate the embedded UI from web/ on demand. The output is checked
# in (so a fresh `make` works without xxd on PATH); run `make embed` to
# refresh after editing web/.
embed:
	bash $(TOOLS)/gen-static-assets.sh > $(SRC_DIR)/static_assets.c

help:
	@echo "pinback build targets:"
	@echo "  make              Build ./pinback-server and tools/fake-ds4-agent"
	@echo "  make test         Build and run unit + integration tests"
	@echo "  make smoke URL=…  Run live smoke against URL"
	@echo "  make embed        Regenerate src/static_assets.c from web/"
	@echo "  make clean        Remove build outputs"

$(MAIN_BIN): $(CORE_OBJS) $(MAIN_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(FAKE_BIN): $(FAKE_OBJ) $(SRC_DIR)/util.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_BIN): $(TEST_OBJS) $(CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test: $(TEST_BIN) $(FAKE_BIN)
	$(TEST_BIN)

smoke:
	@if [ -z "$(URL)" ]; then echo "usage: make smoke URL=http://127.0.0.1:8088"; exit 2; fi
	$(TOOLS)/pinback-smoke "$(URL)"

clean:
	rm -f $(MAIN_BIN) $(FAKE_BIN) $(CORE_OBJS) $(MAIN_OBJ) $(FAKE_OBJ) \
	      $(TEST_OBJS) $(TEST_BIN)

# Rebuild on header change.
$(CORE_OBJS) $(MAIN_OBJ) $(TEST_OBJS) $(FAKE_OBJ): $(wildcard $(SRC_DIR)/*.h)
