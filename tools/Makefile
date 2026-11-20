CFLAGS ?= -O2 -Wall -Wextra

PROGS := blkcheck memhold latread

all: $(PROGS)

blkcheck: blkcheck.c
	$(CC) $(CFLAGS) -pthread -o $@ blkcheck.c

clean:
	rm -f $(PROGS)

.PHONY: all clean
