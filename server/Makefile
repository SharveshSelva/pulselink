CC      := gcc
CFLAGS  := -Wall -Wextra -Werror -g -std=c11 -pthread -I../common -I../imaging
LDFLAGS := -pthread -lm

COMMON  := ../common/checksum.c ../common/wire.c ../common/netutil.c
SRVSRC  := main.c tcp_listener.c udp_listener.c client_handler.c \
           device_table.c ring_log.c sink_chardev.c reaper.c \
           ../imaging/image.c ../imaging/bmp.c ../imaging/convolve.c ../imaging/frame.c
SRCS    := $(SRVSRC) $(COMMON)
HDRS    := server.h log.h device_table.h sink.h \
           ../common/protocol.h ../common/wire.h ../common/netutil.h

.PHONY: all clean tsan dt_test
all: telemetryd

telemetryd: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)

# ThreadSanitizer build (Stage 2 DoD: race-check under load)
tsan: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -fsanitize=thread $(SRCS) -o telemetryd_tsan $(LDFLAGS)

# device-table unit test
dt_test: dt_test.c device_table.c
	$(CC) $(CFLAGS) dt_test.c device_table.c -o dt_test $(LDFLAGS)

clean:
	rm -f telemetryd telemetryd_tsan dt_test integ_test integ_test_tsan

# in-process end-to-end pipeline test (no networking/backgrounding)
FRAME_E2E := frame_e2e_test.c client_handler.c device_table.c ring_log.c \
             ../imaging/image.c ../imaging/bmp.c ../imaging/convolve.c ../imaging/frame.c \
             ../common/checksum.c ../common/wire.c ../common/netutil.c

frame_e2e: $(FRAME_E2E) $(HDRS)
	$(CC) $(CFLAGS) $(FRAME_E2E) -o frame_e2e_test $(LDFLAGS)

INTEG_SRC := integ_test.c client_handler.c device_table.c ring_log.c \
             ../imaging/image.c ../imaging/bmp.c ../imaging/convolve.c ../imaging/frame.c \
             ../common/checksum.c ../common/wire.c ../common/netutil.c
integ: $(INTEG_SRC) $(HDRS)
	$(CC) $(CFLAGS) $(INTEG_SRC) -o integ_test $(LDFLAGS)

integ_tsan: $(INTEG_SRC) $(HDRS)
	$(CC) $(CFLAGS) -fsanitize=thread $(INTEG_SRC) -o integ_test_tsan $(LDFLAGS)
