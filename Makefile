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
ifdef WERROR
CFLAGS  += -Werror
endif

ifeq ($(UNAME_S),Darwin)
CFLAGS  += -D_DARWIN_C_SOURCE
endif
ifeq ($(UNAME_S),Linux)
CFLAGS  += -D_DEFAULT_SOURCE
endif

LDLIBS ?= -lpthread

BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj
GEN_DIR   := $(BUILD_DIR)/generated
BINDIR    := $(BUILD_DIR)

SRC_DIR   := src
TEST_DIR  := tests
SUPPORT   := tests/support
UI_DIR    := ui/app
EMBED_SH  := scripts/embed/gen-static-assets.sh

CORE_SRCS := \
	$(SRC_DIR)/util.c \
	$(SRC_DIR)/log.c \
	$(SRC_DIR)/http.c \
	$(SRC_DIR)/event_log.c \
	$(SRC_DIR)/workspace.c \
	$(SRC_DIR)/snapshot.c \
	$(SRC_DIR)/vterm.c \
	$(SRC_DIR)/tracestream.c \
	$(SRC_DIR)/agent.c \
	$(SRC_DIR)/handlers.c

CORE_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(CORE_SRCS))

GEN_STATIC    := $(GEN_DIR)/static_assets.c
GEN_STATIC_OBJ := $(OBJ_DIR)/static_assets.o

MAIN_BIN  := $(BINDIR)/pinback-server
MAIN_SRC  := $(SRC_DIR)/pinback.c
MAIN_OBJ  := $(OBJ_DIR)/pinback.o

FAKE_BIN  := $(BINDIR)/fake-ds4-agent
FAKE_SRC  := $(SUPPORT)/fake-ds4-agent.c
FAKE_OBJ  := $(OBJ_DIR)/fake-ds4-agent.o

FAKE_KV_BIN  := $(BINDIR)/fake-kv-ds4-agent
FAKE_KV_SRC  := $(SUPPORT)/fake-kv-ds4-agent.c
FAKE_KV_OBJ  := $(OBJ_DIR)/fake-kv-ds4-agent.o

TEST_SRCS := \
	$(TEST_DIR)/test_util.c \
	$(TEST_DIR)/test_event_log.c \
	$(TEST_DIR)/test_http.c \
	$(TEST_DIR)/test_workspace.c \
	$(TEST_DIR)/test_agent.c \
	$(TEST_DIR)/test_tracestream.c \
	$(TEST_DIR)/test_main.c
TEST_OBJS := $(patsubst $(TEST_DIR)/test_%.c,$(OBJ_DIR)/test_%.o,$(TEST_SRCS))
TEST_BIN  := $(BUILD_DIR)/run_tests

.PHONY: all clean test smoke help embed embed-check pinback-server fake-ds4-agent debug ci

# Avoid parallel link races on fresh build/ trees (common on CI -j).
.NOTPARALLEL: all test

all: $(MAIN_BIN) $(FAKE_BIN) $(FAKE_KV_BIN)

pinback-server: $(MAIN_BIN)
fake-ds4-agent: $(FAKE_BIN)
fake-kv-ds4-agent: $(FAKE_KV_BIN)

# Regenerate embedded UI from ui/app/. Output lives under build/generated/.
embed:
	@mkdir -p $(GEN_DIR)
	bash $(EMBED_SH) > $(GEN_STATIC)

embed-check: embed
	@test -s $(GEN_STATIC)

$(GEN_STATIC): $(EMBED_SH) $(shell find $(UI_DIR) -type f 2>/dev/null)
	@mkdir -p $(GEN_DIR)
	bash $(EMBED_SH) > $(GEN_STATIC)

debug:
	@$(MAKE) clean
	@$(MAKE) OPT_FLAGS=-O0 DEBUG_FLAGS=-g3 embed all

ci: embed all test

help:
	@echo "pinback build targets:"
	@echo "  make              Build build/pinback-server and build/fake-ds4-agent"
	@echo "  make debug        Unoptimized build with debug symbols (-O0 -g3)"
	@echo "  make ci           embed + all + test (CI compile/test gate)"
	@echo "  make test         Build and run unit + integration tests"
	@echo "  make smoke URL=…  Run live smoke against URL"
	@echo "  make embed        Regenerate build/generated/static_assets.c from ui/app/"
	@echo "  make clean        Remove build/ outputs"

$(MAIN_BIN): $(CORE_OBJS) $(GEN_STATIC_OBJ) $(MAIN_OBJ) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(FAKE_BIN): $(FAKE_OBJ) $(OBJ_DIR)/util.o | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(FAKE_KV_BIN): $(FAKE_KV_OBJ) $(OBJ_DIR)/util.o | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_BIN): $(TEST_OBJS) $(CORE_OBJS) $(GEN_STATIC_OBJ) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJS) $(CORE_OBJS) $(GEN_STATIC_OBJ) $(LDLIBS)

$(BINDIR):
	@mkdir -p $(BINDIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/test_%.o: $(TEST_DIR)/test_%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/fake-ds4-agent.o: $(FAKE_SRC) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/fake-kv-ds4-agent.o: $(FAKE_KV_SRC) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(GEN_STATIC_OBJ): $(GEN_STATIC) src/static_assets.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $(GEN_STATIC)

$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

test: $(TEST_BIN) $(FAKE_BIN) $(FAKE_KV_BIN)
	$(TEST_BIN)

smoke:
	@if [ -z "$(URL)" ]; then echo "usage: make smoke URL=http://127.0.0.1:8088"; exit 2; fi
	scripts/qa/pinback-smoke "$(URL)"

kv-smoke:
	@if [ -z "$(URL)" ] && [ -z "$(DS4_MODEL)" ]; then \
		echo "usage: make kv-smoke URL=http://127.0.0.1:18098"; \
		echo "   or: make kv-smoke DS4_MODEL=/path/to/model.gguf  (starts server)"; \
		exit 2; \
	fi
	@if [ -n "$(DS4_MODEL)" ]; then scripts/qa/kv-resume-smoke --start-server --model "$(DS4_MODEL)"; \
	else scripts/qa/kv-resume-smoke "$(URL)"; fi

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(SRC_DIR)/*.o $(TEST_DIR)/*.o $(SUPPORT)/*.o

# Rebuild on header change.
$(CORE_OBJS) $(MAIN_OBJ) $(TEST_OBJS) $(FAKE_OBJ): $(wildcard $(SRC_DIR)/*.h)
