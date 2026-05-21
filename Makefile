CC = gcc
AR = ar rcs
CFLAGS = -Wall -Wextra -pthread -I.
LDFLAGS = -pthread

all: libanimate.a animate_server animate_client

animate.o: animate.c animate.h
	$(CC) $(CFLAGS) -c animate.c -o animate.o

libanimate.a: animate.o
	$(AR) $@ $^

rpc_handler.o: rpc_handler.c rpc_handler.h animate.h
	$(CC) $(CFLAGS) -c rpc_handler.c -o rpc_handler.o

threadpool.o: threadpool.c threadpool.h
	$(CC) $(CFLAGS) -c threadpool.c -o threadpool.o

animate_server.o: animate_server.c rpc_handler.h threadpool.h animate.h
	$(CC) $(CFLAGS) -c animate_server.c -o animate_server.o

animate_server: animate_server.o rpc_handler.o threadpool.o libanimate.a
	$(CC) $(CFLAGS) -o $@ animate_server.o rpc_handler.o threadpool.o libanimate.a $(LDFLAGS)

animate_client: animate_client.o
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f animate_server animate_client *.o libanimate.a
	rm -f *.dat *.mp4 *.log

.PHONY: all clean
