#include <sys/types.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>

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
    struct timespec *begin;
	struct timeval tv;
	struct event ev;
	int state;
    char *url;
    struct url_item *item;
};

struct url_item {
    char *url;
    uint64_t average;
    int count;
    int succeed;
    int total;
    TAILQ_ENTRY (url_item) entry;
};

TAILQ_HEAD(urlhead, url_item) *urls;

int humanize_number(char *buf, size_t len, int64_t bytes, const char *suffix, int scale, int flags);

static void
http_callback (int fd, short what, void *arg)
{
	struct ctx *ctx = (struct ctx *)arg;
	int r, s_error;
	socklen_t optlen;
	char buf[1024];
	struct timeval ntv;
    struct timespec ts;
    uint64_t ms;
    double alpha;
	
	if (what == EV_TIMEOUT) {
        if (ctx->item) {
            ctx->item->count ++;
        }
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
                    if (ctx->item) {
                        ctx->item->count ++;
                    }
					event_del (&ctx->ev);
					close (fd);
					free (ctx);
					goto check_remain;
	    		}
				if (!silent) {
					fprintf (stderr, "Connection successful\n");
				}
				r = snprintf (buf, sizeof (buf), "GET /%s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", ctx->url, host);
				if (write (fd, buf, r) == -1) {
					if (!silent) {
						fprintf (stderr, "Write error: %s, %d\n", strerror (errno), errno);
					}
					event_del (&ctx->ev);
                    if (ctx->item) {
                        ctx->item->count ++;
                    }
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
                if (ctx->item) {
                    ctx->item->count ++;
                }
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
                        if (ctx->item) {
                            /* Calculate time for this request */
                            clock_gettime (CLOCK_REALTIME, &ts);
                    		ms = (ts.tv_sec - ctx->begin->tv_sec) * 1000000L + (ts.tv_nsec - ctx->begin->tv_nsec) / 1000;
                            /* Calculate growing average */
                            alpha = 2. / (++ctx->item->count + 1);
                            ctx->item->average = (double)ctx->item->average * (1. - alpha) + (double)ms * alpha;
                            ctx->item->succeed ++;
                        }
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
                if (ctx->item) {
                    ctx->item->count ++;
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
connect_socket (char *url, struct url_item *item, struct timespec *begin)
{
	int s, r, flags;
	struct ctx *curctx;

	s = socket (AF_INET, SOCK_STREAM, 0);

	if (s == -1) {
		fprintf (stderr, "socket() failed: %s, %d\n", strerror (errno), errno);
		return;
	}
	
	flags = fcntl(s, F_GETFL, 0);
	fcntl(s, F_SETFL, flags | O_NONBLOCK);
	
	r = connect(s, (struct sockaddr *)&addr, sizeof (addr));
	if (r == -1 && errno != EINPROGRESS) {
		if (!silent) {
			fprintf (stderr, "connect() failed: %s, %d\n", strerror (errno), errno);
		}
		return;
	}
	curctx = malloc (sizeof (struct ctx));
	
	curctx->state = STATE_CONNECT;
    curctx->url = url;
    curctx->begin = begin;
    curctx->item = item;
	event_set (&curctx->ev, s, EV_WRITE | EV_TIMEOUT, http_callback, curctx);
	memcpy (&curctx->tv, &http_timeout, sizeof (struct timeval));
	event_add (&curctx->ev, &curctx->tv);
}

char*
strchug (char *string)
{
    char *start;


    for (start = (char*) string; *start && isspace (*start); start++)
    ;

    memmove (string, start, strlen (start) + 1);

    return string;
}

char*
strchomp (char *string)
{
    int len;

    len = strlen (string);
    while (len--) {
        if (isspace (string[len])) {
            string[len] = '\0';
        }
        else {
            break;
        }
    }

    return string;
}

static int
read_urls_file (const char *file)
{
    FILE *f;
    char buf[BUFSIZ];
    struct url_item *new_item;

    f = fopen (file, "r");

    if (f == NULL) {
        return 0;
    }

    while (!feof (f)) {
        if (!fgets (buf, sizeof(buf), f)) {
            break;
        }
        strchomp (strchug (buf));
        if (buf[0] == '#') {
            /* skip comments */
            continue;
        }
        new_item = malloc (sizeof (struct url_item));
        if (!new_item) {
            fclose (f);
            return 0;
        }
        bzero (new_item, sizeof (struct url_item));
        new_item->url = strdup (buf);
        TAILQ_INSERT_TAIL (urls, new_item, entry); 
    }
    
    fclose (f);

    return 1;
}

int
main (int argc, char **argv)
{
	int ch, i, j, iterations = 1;
	struct hostent *he;
	char intbuf[32], intbuf2[32];
	struct timespec ts1, ts2;
    struct url_item *np;
	struct rlimit rlim;

	event_init ();
	
	bzero (&addr, sizeof (struct sockaddr_in));
	addr.sin_port = htons (80);
	addr.sin_family = AF_INET;
	inet_aton ("127.0.0.1", &addr.sin_addr);
	http_timeout.tv_sec = 1;
	http_timeout.tv_usec = 0;

	while ((ch = getopt(argc, argv, "f:c:p:n:t:u:i:sh")) != -1) {
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
            case 'f':
                if (optarg) {
                    urls = malloc (sizeof (struct urlhead));
                    TAILQ_INIT(urls);
                    if (!read_urls_file (optarg)) {
                        urls = NULL;
                    }
                }
                break;
            case 'h':
            default:
                /* Show help message and exit */
                printf (
                        "Usage: http-stress [-c host] [-p port] [-n connections_count] [-t timeout] [-f file]\n"
                        "-h:        This help message\n"
                        "-f:        Specify file with urls\n"
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
	/* Try to set rlimits for openfiles */
	getrlimit (RLIMIT_NOFILE, &rlim);
	fprintf (stderr, "current files limit: %ld, targeted: %ld, hard limit: %ld\n", (long int)rlim.rlim_cur, (long int)nconns + 10, (long int)rlim.rlim_max);	
	rlim.rlim_cur = nconns + 10;
	if (setrlimit (RLIMIT_NOFILE, &rlim) == -1) {
		fprintf (stderr, "setting limits failed: %s, %d, targeted limit: %ld\n", strerror (errno), errno, (long int)rlim.rlim_cur);		
	}

	for (j = 0; j < iterations; j ++) {
		clock_gettime (CLOCK_REALTIME, &ts1);
        if (urls == NULL) {
		    for (i = 0; i < nconns; i ++) {
			    connect_socket (url, NULL, &ts1);
		    }
        }
        else {
            /* Get all urls */
            nconns = 0;
            for (np = urls->tqh_first; np != NULL; np = np->entry.tqe_next) {
                connect_socket (np->url, np, &ts1);
                nconns ++;
            }
        }
		remain = nconns;

		event_loop (0);
		clock_gettime (CLOCK_REALTIME, &ts2);
		microseconds += (ts2.tv_sec - ts1.tv_sec) * 1000000L + (ts2.tv_nsec - ts1.tv_nsec) / 1000;
	}

    if (urls != NULL) {
        printf ("*** URLS STATISTICS ***\n");
        printf ("--------------------------------------------------------------------------\n"
                "| URL                                      | TIME       | SUCCESS | FAIL |\n"
                "--------------------------------------------------------------------------\n");
        for (np = urls->tqh_first; np != NULL; np = np->entry.tqe_next) {
            printf ("| %40.39s | %10lu | %7d | %4d |\n", np->url, (long int)np->average / 1000, np->succeed, (np->count - np->succeed));
        }
        printf ("--------------------------------------------------------------------------\n");
    }
    humanize_number (intbuf, sizeof (intbuf), bytes, "B", 1, HN_B);
	humanize_number (intbuf2, sizeof (intbuf2), (double)bytes / ((double)microseconds / 1000000.) * 8, "b/sec", 1, HN_B);

	printf ("Number of connections: %d\n"
	        "Number of successfull connections: %d\n"
			"Total amount of time (microseconds): %lld = %.3f msec\n"
			"Average per connection: %lld = %.3f msec\n"
			"Average connections per second: %.3f\n"
			"Bytes read: %s, %s\n",
			nconns * iterations, succeed, microseconds, microseconds / 1000.,
			succeed ? microseconds / succeed : 0,
			succeed ? (microseconds / 1. / succeed / 1000.) : 0.,
			(double)succeed / ((double)microseconds / 1000000.),
			intbuf, intbuf2);
	
	exit (EXIT_SUCCESS);
}
