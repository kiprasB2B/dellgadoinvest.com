# Dellgado.invest static server - pure C17
CC       = gcc
WARN     = -Wall -Wextra -Wpedantic -Werror
CFLAGS  ?= -O2
CFLAGS  += -std=c17 $(WARN)
LDFLAGS += -pthread
DBGFLAGS = -std=c17 $(WARN) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined

SRC      = src/server.c
BIN      = build/server
DBG      = build/server-debug
PORT    ?= 8080

.PHONY: all debug run test test-debug clean

all: $(BIN)

$(BIN): $(SRC) | build
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

debug: $(DBG)

$(DBG): $(SRC) | build
	$(CC) $(DBGFLAGS) -o $@ $(SRC) -pthread -fsanitize=address,undefined

build:
	mkdir -p build

run: $(BIN)
	PORT=$(PORT) ./$(BIN)

test: $(BIN)
	./tests/test.sh ./$(BIN)

test-debug: $(DBG)
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		./tests/test.sh ./$(DBG)

clean:
	rm -rf build
