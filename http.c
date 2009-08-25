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
#include <time.h>

#include "event.h"

#define FIXED_CONNS 10
#define STATE_CONNECT 0
#define STATE_READ 1

#define HN_DECIMAL              0x01
#define HN_NOSPACE              0x02
#define HN_B                    0x04
#define HN_DIVISOR_1000         0x08

#define HN_GETSCALE             0x10
#define HN_AUTOSCALE            0x20


struct sockaddr_in addr;
struct timeval http_timeout;
int nconns = FIXED_CONNS;
char *host = "localhost";
char *url = "";
int silent = 0;
int remain;

int succeed = 0;
long long int microseconds = 0;
unsigned long long int bytes;

struct ctx {
	struct timeval tv;
	struct event ev;
	int state;
};

int humanize_number(char *buf, size_t len, int64_t bytes, const char *suffix, int scale, int flags);

static void
http_callback (int fd, short what, void *arg)
{
	struct ctx *ctx = (struct ctx *)arg;
	int r, s_error;
	socklen_t optlen;
	char buf[1024];
	struct timeval ntv;
	
	if (what == EV_TIMEOUT) {
		fprintf (stderr, "Connection timed out\n");
		event_del (&ctx->ev);
		close (fd);
		free (ctx);
		goto check_remain;	
	}
	switch (ctx->state) {
		case STATE_CONNECT:
			if (what == EV_WRITE) {
				optlen = sizeof (s_error);
				getsockopt (fd, SOL_SOCKET, SO_ERROR, (void *)&s_error, &optlen);
	    		if (s_error && !silent) {
					fprintf (stderr, "Connection failed: %s, %d\n", strerror (s_error), s_error);
					event_del (&ctx->ev);
					close (fd);
					free (ctx);
					goto check_remain;
	    		}
				if (!silent) {
					fprintf (stderr, "Connection successful\n");
				}
				r = snprintf (buf, sizeof (buf), "GET /%s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", url, host);
				if (write (fd, buf, r) == -1) {
					if (!silent) {
						fprintf (stderr, "Write error: %s, %d\n", strerror (errno), errno);
					}
					event_del (&ctx->ev);
					close (fd);
					free (ctx);
					goto check_remain;
				}
				ctx->state = STATE_READ;
				event_del (&ctx->ev);
				event_set (&ctx->ev, fd, EV_READ | EV_TIMEOUT | EV_PERSIST, http_callback, ctx);
				memcpy (&ctx->tv, &http_timeout, sizeof (struct timeval));
				event_add (&ctx->ev, &ctx->tv);
			}
			else {
				if (!silent) {
					fprintf (stderr, "Connection error: %d\n", what);
				}
				event_del (&ctx->ev);
				close (fd);
				free (ctx);
				goto check_remain;
			}
			break;
		case STATE_READ:
			if (what == EV_READ) {
				if ((r = read (fd, buf, sizeof (buf))) <= 0) {
					if (r == -1) {
						fprintf (stderr, "Error while reading: %s\n", strerror (errno));	
					}
					else {
						succeed ++;
					}
					if (!silent) {
						fprintf (stderr, "Read eof, closing connection\n");
					}
					event_del (&ctx->ev);
					close (fd);
					free (ctx);
					goto check_remain;
				}
				if (!silent) {
					fprintf (stderr, "Read %d bytes\n", r);
				}
				bytes += r;
			}
			else {
				if (!silent) {
					fprintf (stderr, "Read error, connection closed\n");
				}
				event_del (&ctx->ev);
				close (fd);
				free (ctx);
				goto check_remain;
			}
			break;
	}

	return;

	check_remain:
	remain --;
	if (remain == 0) {
		ntv.tv_sec = 0;
		ntv.tv_usec = 0;
		event_loopexit (&ntv);	
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
		if (!silent) {
			fprintf (stderr, "connect() failed: %m, %d\n", errno);
		}
		return;
	}
	curctx = malloc (sizeof (struct ctx));
	
	curctx->state = STATE_CONNECT;
	event_set (&curctx->ev, s, EV_WRITE | EV_TIMEOUT, http_callback, curctx);
	memcpy (&curctx->tv, &http_timeout, sizeof (struct timeval));
	event_add (&curctx->ev, &curctx->tv);
}

int
main (int argc, char **argv)
{
	int ch, i, iterations = 1;
	struct hostent *he;
	char intbuf[32], intbuf2[32];
	struct timespec ts1, ts2;
	event_init ();
	
	bzero (&addr, sizeof (struct sockaddr_in));
	addr.sin_port = htons (80);
	addr.sin_family = AF_INET;
	inet_aton ("127.0.0.1", &addr.sin_addr);
	http_timeout.tv_sec = 1;
	http_timeout.tv_usec = 0;

	while ((ch = getopt(argc, argv, "c:p:n:t:u:i:sh")) != -1) {
        switch (ch) {
            case 'c':
				if (optarg) {
					host = strdup (optarg);
					if (inet_aton (optarg, &addr.sin_addr) == 0) {
						if ((he = gethostbyname (optarg)) == NULL) {
							fprintf (stderr, "Host %s not found\n", optarg);
							return -1;
						}
						memcpy((char *)&addr.sin_addr, he->h_addr, sizeof(struct in_addr));
					}
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
			case 'u':
				if (optarg) {
					url = strdup (optarg);
				}
				break;
			case 's':
				silent = 1;
				break;
			case 'i':
				if (optarg) {
					iterations = atoi (optarg);
				}
				break;
            case 'h':
            default:
                /* Show help message and exit */
                printf (
                        "Usage: http-stress [-c host] [-p port] [-n connections_count] [-t timeout]\n"
                        "-h:        This help message\n"
                        "-c:        Connect to specified host or ip (default 127.0.0.1)\n"
                        "-p:        Specify port to connect (default 80)\n"
						"-i:        Number of iterations (default 1)\n"
                        "-t:        Number of seconds to timeout (default 1)\n"
						"-u:        Url to get (relative to /)\n"
						"-s:        Silent mode\n"
						"-n:        Number of connections to make (default %d)\n", FIXED_CONNS);
                exit (EXIT_SUCCESS);
                break;
        }
    }
	
	signal (SIGPIPE, SIG_IGN);

	while (iterations) {
		clock_gettime (CLOCK_REALTIME, &ts1);
		for (i = 0; i < nconns; i ++) {
			connect_socket ();
		}
		remain = nconns;

		event_loop (0);
		clock_gettime (CLOCK_REALTIME, &ts2);
		microseconds += (ts2.tv_sec - ts1.tv_sec) * 1000000L + (ts2.tv_nsec - ts1.tv_nsec) / 1000;
		iterations --;
	}

	humanize_number (intbuf, sizeof (intbuf), bytes, "B", 1, HN_B);
	humanize_number (intbuf2, sizeof (intbuf2), (double)bytes / ((double)microseconds / 1000000.) * 8, "b/sec", 1, HN_B);

	printf ("Number of connections: %d\n"
	        "Number of successfull connections: %d\n"
			"Total amount of time (microseconds): %lld = %.3f msec\n"
			"Average per connection: %lld = %.3f msec\n"
			"Average connections per second: %.3f\n"
			"Bytes read: %s, %s\n",
			nconns, succeed, microseconds, microseconds / 1000.,
			succeed ? microseconds / succeed : 0,
			succeed ? (microseconds / 1. / succeed / 1000.) : 0.,
			(double)succeed / ((double)microseconds / 1000000.),
			intbuf, intbuf2);
	
	exit (EXIT_SUCCESS);
}
