CC = gcc
CFLAGS = -lsqlite3 -pthread

SRC = Server.c

ARG = 8021

server: $(SRC)
	$(CC) $^ -o $@  $(CFLAGS)

.PHONY: run
run:
	make server
	./server $(ARG)

.PHONY: clean
clean:
	rm server
