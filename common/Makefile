CC      := gcc
CFLAGS  := -Wall -Wextra -Werror -g -std=c11 -I.

.PHONY: all test clean

all: checksum_test

checksum_test: checksum.c checksum_test.c protocol.h
	$(CC) $(CFLAGS) checksum.c checksum_test.c -o $@

test: checksum_test
	./checksum_test

clean:
	rm -f checksum_test
