CC = gcc
CFLAGS = -pthread

SRC = Client.c

ARG = 127.0.0.1 8021

client: $(SRC)
	$(CC) $^ -o $@ $(CFLAGS)

.PHONY: run
run:
	make client
	./client $(ARG)

.PHONY: clean
clean:
	rm client