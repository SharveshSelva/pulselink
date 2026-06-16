# The firmware itself builds in Wokwi / arduino-cli (see README.md). This
# Makefile only builds the HOST cross-check that proves the firmware's framing
# produces packets the server accepts.
CC     := gcc
CFLAGS := -Wall -Wextra -Werror -g -std=c11 -I.

.PHONY: test clean
test: fw_frame_test
	./fw_frame_test

fw_frame_test: fw_frame_test.c telemetry.c checksum.c ../common/wire.c
	$(CC) $(CFLAGS) fw_frame_test.c telemetry.c checksum.c ../common/wire.c -o fw_frame_test

clean:
	rm -f fw_frame_test
