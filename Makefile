CC?=gcc
PREFIX?=/usr/local
LIBS?=-levent

all: http-stress

http.o: http.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -g -O2 -I$(PREFIX)/include -c http.c -o http.o

humanize_number.o: humanize_number.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -g -O2 -I$(PREFIX)/include -c humanize_number.c -o humanize_number.o

http-stress: http.o humanize_number.o
	$(CC) $(CFLAGS) $(LDFLAGS) -L$(PREFIX)/lib -o http-stress humanize_number.o http.o $(LIBS) $(EXTRA_LIBS)

clean:
	rm -f http-stress http.o humanize_number.o
