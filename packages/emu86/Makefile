CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11
EMCC = emcc
EMFLAGS = -O2 -s MODULARIZE=1 -s EXPORT_ES6=1 -s ALLOW_MEMORY_GROWTH=1 \
          -s ENVIRONMENT=worker -s NO_EXIT_RUNTIME=1

# --- Reference build (original 8086tiny, for comparison) ---
.PHONY: reference-build
reference-build:
	@if [ ! -f reference/8086tiny.c ]; then \
		echo "Error: reference/8086tiny.c not found. Run EMU-01 first."; \
		exit 1; \
	fi
	$(CC) -O2 -DNO_GRAPHICS -o reference/8086tiny reference/8086tiny.c

# --- Main emulator build ---
.PHONY: build
build:
	@echo "TODO: Not yet implemented. See EMU-02 onwards."

# --- WASM build ---
.PHONY: wasm
wasm:
	@echo "TODO: Not yet implemented. See EMU-15."

# --- Tests ---
.PHONY: test
test: test-unit test-integration

.PHONY: test-unit
test-unit: test-snapshot test-ringbuf

.PHONY: test-snapshot
test-snapshot:
	$(CC) $(CFLAGS) -o test/unit/test_snapshot test/unit/test_snapshot.c
	./test/unit/test_snapshot

.PHONY: test-ringbuf
test-ringbuf:
	$(CC) $(CFLAGS) -o test/unit/test_ringbuf test/unit/test_ringbuf.c
	./test/unit/test_ringbuf

.PHONY: test-integration
test-integration:
	@echo "TODO: Not yet implemented."

# --- Clean ---
.PHONY: clean
clean:
	rm -f reference/8086tiny
	rm -f src/emulator/*.o
	rm -rf dist/
	rm -f test/unit/*.o test/unit/test_snapshot test/unit/test_ringbuf
