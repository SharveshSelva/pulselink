CC      := gcc
CFLAGS  := -Wall -Wextra -Werror -g -std=c11 -pthread -I../common -I../imaging
LDFLAGS := -pthread -lm

COMMON  := ../common/checksum.c ../common/wire.c ../common/netutil.c
IMG     := ../imaging/image.c ../imaging/frame.c

.PHONY: all clean
all: fake_client

fake_client: fake_client.c $(COMMON) $(IMG) ../common/protocol.h ../common/wire.h ../common/netutil.h
	$(CC) $(CFLAGS) fake_client.c $(COMMON) $(IMG) -o $@ $(LDFLAGS)

clean:
	rm -f fake_client
