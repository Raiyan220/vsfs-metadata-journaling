CC = gcc
CFLAGS = -Wall -Wextra -O2

TARGETS = mkfs validator journal

all: $(TARGETS)

mkfs: mkfs.c
	$(CC) $(CFLAGS) -o mkfs mkfs.c

validator: validator.c
	$(CC) $(CFLAGS) -o validator validator.c

journal: journal.c
	$(CC) $(CFLAGS) -o journal journal.c

clean:
	rm -f $(TARGETS) vsfs.img