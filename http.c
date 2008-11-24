#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <sys/poll.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

#include "event.h"

#define FIXED_CONNS 10
#define STATE_CONNECT 0
#define STATE_READ 1

struct sockaddr_in addr;
struct timeval http_timeout;
int nconns = FIXED_CONNS;

struct ctx {
	struct event ev;
	int state;
};

static void
http_callback (int fd, short what, void *arg)
{
	struct ctx *ctx = (struct ctx *)arg;
	int r;
	char buf[1024];

	switch (ctx->state) {
		case STATE_CONNECT:
			if (what == EV_WRITE) {
				fprintf (stderr, "Connection successful\n");
				if (write (fd, "GET / HTTP/1.0\r\n\r\n", 18) == -1) {
					fprintf (stderr, "Write error: %m, %d\n", errno);
					return;
				}
				ctx->state = STATE_READ;
				event_del (&ctx->ev);
				event_set (&ctx->ev, fd, EV_READ | EV_TIMEOUT | EV_PERSIST, http_callback, ctx);
				event_add (&ctx->ev, &http_timeout);
			}
			else {
				fprintf (stderr, "Connection error: %d\n", what);
			}
			break;
		case STATE_READ:
			if (what == EV_READ) {
				if ((r = read (fd, buf, sizeof (buf))) <= 0) {
					fprintf (stderr, "Read eof, closing connection\n");
					event_del (&ctx->ev);
					return;
				}
				fprintf (stderr, "Read %d bytes\n", r);
			}
			else {
				fprintf (stderr, "Read error, connection closed %m, %d\n", errno);
				event_del (&ctx->ev);
				return;
			}
			break;
	}
}

static void
connect_socket ()
{
	int s, r, flags;
	struct ctx *curctx;

	s = socket (AF_INET, SOCK_STREAM, 0);

	if (s == -1) {
		fprintf (stderr, "socket() failed: %m, %d\n", errno);
		return;
	}
	
	flags = fcntl(s, F_GETFL, 0);
	fcntl(s, F_SETFL, flags | O_NONBLOCK);
	
	r = connect(s, (struct sockaddr *)&addr, sizeof (addr));
	if (r == -1 && errno != EINPROGRESS) {
		fprintf (stderr, "connect() failed: %m, %d\n", errno);
		return;
	}
	curctx = malloc (sizeof (struct ctx));
	
	curctx->state = STATE_CONNECT;
	event_set (&curctx->ev, s, EV_WRITE | EV_TIMEOUT, http_callback, curctx);
	event_add (&curctx->ev, &http_timeout);
}

int
main (int argc, char **argv)
{
	int ch, i;
	struct hostent *he;
	event_init ();
	
	bzero (&addr, sizeof (struct sockaddr_in));
	addr.sin_port = htons (80);
	addr.sin_family = AF_INET;
	inet_aton ("127.0.0.1", &addr.sin_addr);
	http_timeout.tv_sec = 1;
	http_timeout.tv_usec = 0;

	while ((ch = getopt(argc, argv, "c:h:p:n:t:?")) != -1) {
        switch (ch) {
            case 'c':
			case 'h':
                if (inet_aton (optarg, &addr.sin_addr) == 0) {
					if ((he = gethostbyname (optarg)) == NULL) {
						fprintf (stderr, "Host %s not found\n", optarg);
						return -1;
					}
					memcpy((char *)&addr.sin_addr, he->h_addr, sizeof(struct in_addr));
				}
                break;
            case 'p':
                if (optarg) {
                    addr.sin_port = htons (atoi (optarg));
                }
                break;
            case 'n':
				if (optarg) {
					nconns = atoi (optarg);
				}
				break;
			case 't':
				if (optarg) {
					http_timeout.tv_sec = atoi (optarg);
				}
				break;
            case '?':
            default:
                /* Show help message and exit */
                printf (
                        "Usage: http-stress [-h host] [-p port] [-n connections_count] [-t timeout]\n"
                        "-?:        This help message\n"
                        "-h:        Connect to specified host or ip (default 127.0.0.1)\n"
                        "-p:        Specify port to connect (default 80)\n"
                        "-t:        Number of seconds to timeout (default 1)\n"
						"-n:        Number of connections to make (default %d)\n", FIXED_CONNS);
                exit (EXIT_SUCCESS);
                break;
        }
    }
	
	signal (SIGPIPE, SIG_IGN);
	for (i = 0; i < nconns; i ++) {
		connect_socket ();
	}

	event_loop (0);
}
