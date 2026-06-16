# Stage 4 imaging — pure host C (fully buildable and testable here).
CC     := gcc
CFLAGS := -Wall -Wextra -Werror -g -std=c11 -I. -I../common
LDLIBS := -lm

COMMON := ../common/checksum.c ../common/wire.c
IMG    := image.c bmp.c convolve.c frame.c

.PHONY: test demo clean
test: img_test
	./img_test

img_test: img_test.c $(IMG) $(COMMON)
	$(CC) $(CFLAGS) img_test.c $(IMG) $(COMMON) -o img_test $(LDLIBS)

demo: pl_imaging
	./pl_imaging .

pl_imaging: pl_imaging.c $(IMG) $(COMMON)
	$(CC) $(CFLAGS) pl_imaging.c $(IMG) $(COMMON) -o pl_imaging $(LDLIBS)

clean:
	rm -f img_test pl_imaging *.o
