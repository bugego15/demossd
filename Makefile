# SSD firmware simulator
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11
CPPFLAGS = -Iinclude
LDFLAGS  =
LDLIBS   = -lm

SRCS = src/main.c src/event.c src/nand.c src/ftl.c src/gc.c \
       src/journal.c src/host.c src/workload.c src/stats.c
OBJS = $(SRCS:.c=.o)

ssdsim: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c include/types.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) ssdsim ssdsim.exe

.PHONY: clean
