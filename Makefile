CC=gcc
PREFIX=/usr/local

all: http-stress

http.o: http.c
	$(CC) -I$(PREFIX)/include -c http.c -o http.o

humanize_number.o: humanize_number.c
	$(CC) -I$(PREFIX)/include -c humanize_number.c -o humanize_number.o

http-stress: http.o humanize_number.o
	$(CC) -L$(PREFIX)/lib -levent -o http-stress humanize_number.o http.o

clean:
	rm -f http-stress http.o
