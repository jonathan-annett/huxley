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
	@mkdir -p build
	$(CC) $(CFLAGS) -c src/emulator/run.c -o build/run.o -Isrc/emulator
	$(CC) $(CFLAGS) -c src/emulator/snapshot.c -o build/snapshot.o -Isrc/emulator

.PHONY: lib
lib: build
	ar rcs build/libemu86.a build/run.o build/snapshot.o

# --- Linux CLI tool ---
emu86: src/emulator/run.c src/emulator/snapshot.c src/hosts/linux/main.c src/hosts/linux/platform_linux.c
	$(CC) $(CFLAGS) -o emu86 \
		src/hosts/linux/main.c \
		src/hosts/linux/platform_linux.c \
		src/emulator/run.c \
		src/emulator/snapshot.c \
		-Isrc/emulator -Isrc/hosts/linux

.PHONY: run-freedos
run-freedos: emu86
	./emu86 reference/bios test/images/freedos.img

.PHONY: run-elks
run-elks: emu86
	./emu86 reference/bios test/images/fd1440-minix.img

# --- WASM build ---
.PHONY: wasm
wasm:
	@echo "TODO: Not yet implemented. See EMU-15."

# --- Tests ---
.PHONY: test
test: test-unit test-integration

.PHONY: test-unit
test-unit: test-snapshot test-ringbuf test-decode test-arithmetic test-logic test-shift test-transfer test-string test-control test-flags-io test-run test-platform

.PHONY: test-snapshot
test-snapshot:
	$(CC) $(CFLAGS) -o test/unit/test_snapshot test/unit/test_snapshot.c
	./test/unit/test_snapshot

.PHONY: test-ringbuf
test-ringbuf:
	$(CC) $(CFLAGS) -o test/unit/test_ringbuf test/unit/test_ringbuf.c
	./test/unit/test_ringbuf

.PHONY: test-decode
test-decode:
	$(CC) $(CFLAGS) -o test/unit/test_decode test/unit/test_decode.c
	./test/unit/test_decode

.PHONY: test-arithmetic
test-arithmetic:
	$(CC) $(CFLAGS) -o test/unit/test_arithmetic test/unit/test_arithmetic.c
	./test/unit/test_arithmetic

.PHONY: test-logic
test-logic:
	$(CC) $(CFLAGS) -o test/unit/test_logic test/unit/test_logic.c
	./test/unit/test_logic

.PHONY: test-transfer
test-transfer:
	$(CC) $(CFLAGS) -o test/unit/test_transfer test/unit/test_transfer.c
	./test/unit/test_transfer

.PHONY: test-shift
test-shift:
	$(CC) $(CFLAGS) -o test/unit/test_shift test/unit/test_shift.c
	./test/unit/test_shift

.PHONY: test-string
test-string:
	$(CC) $(CFLAGS) -o test/unit/test_string test/unit/test_string.c
	./test/unit/test_string

.PHONY: test-control
test-control:
	$(CC) $(CFLAGS) -o test/unit/test_control test/unit/test_control.c
	./test/unit/test_control

.PHONY: test-flags-io
test-flags-io:
	$(CC) $(CFLAGS) -o test/unit/test_flags_io test/unit/test_flags_io.c
	./test/unit/test_flags_io

.PHONY: test-run
test-run:
	$(CC) $(CFLAGS) -o test/unit/test_run test/unit/test_run.c src/emulator/run.c -Isrc/emulator
	./test/unit/test_run

.PHONY: test-platform
test-platform:
	$(CC) $(CFLAGS) -o test/unit/test_platform_linux test/unit/test_platform_linux.c -Isrc/emulator -Isrc/hosts/linux
	./test/unit/test_platform_linux

.PHONY: test-integration
test-integration:
	@echo "TODO: Not yet implemented."

# --- Clean ---
.PHONY: clean
clean:
	rm -f reference/8086tiny emu86
	rm -f src/emulator/*.o
	rm -rf dist/
	rm -f test/unit/*.o test/unit/test_snapshot test/unit/test_ringbuf test/unit/test_decode test/unit/test_arithmetic test/unit/test_logic test/unit/test_shift test/unit/test_transfer test/unit/test_string test/unit/test_control test/unit/test_flags_io test/unit/test_run test/unit/test_platform_linux
