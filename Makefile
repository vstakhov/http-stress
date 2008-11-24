CC=gcc
PREFIX=/usr/local

all: http-stress

http.o: http.c
	$(CC) -I$(PREFIX)/include -c http.c -o http.o

http-stress: http.o
	$(CC) -L$(PREFIX)/lib -levent -o http-stress http.o

clean:
	rm -f http-stress http.o
