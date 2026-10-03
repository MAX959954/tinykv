// tinykv server: N event-loop threads (epoll) + the store's commit thread.
//
// Each worker thread owns its own listening socket (SO_REUSEPORT: the kernel
// spreads new connections across them), its own epoll instance and its own
// connections. GETs are answered inline. SET/DEL are handed to the store's
// commit thread; when the write is durable, the commit thread queues the
// connection on the worker's completion list and pokes the worker's eventfd,
// and the worker sends the reply. A worker never blocks on the disk, so one
// slow fsync doesn't stall the other connections it serves.
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "command.h"
#include "linebuf.h"
#include "store.h"

#define DEFAULT_PORT "9999"
#define DEFAULT_WAL "kv.log"
#define DEFAULT_MAX_CONNECTIONS 10000
#define DEFAULT_COMPACT_BYTES (64ull << 20)    // 64 MiB
#define BACKLOG 1024
#define MAX_EVENTS 128
#define OUT_HIGH_WATER ((size_t)64 * 1024)   // stop reading from a client whose replies pile up
#define SHUTDOWN_GRACE_MS 5000

typedef struct Worker Worker;

typedef struct Conn {
    int fd;                      // -1 once closed
    Worker *w;
    LineBuf in;
    char *out;                   // pending reply bytes [out_off, out_len)
    size_t out_len, out_off, out_cap;
    unsigned events;             // current epoll interest
    int write_pending;           // a SET/DEL is with the commit thread
    int peer_eof;                // client shut down its sending side
    int dead;                    // closed and unlinked; freed at the end of the loop iteration
    StoreResult result;          // filled in by the commit thread
    struct Conn *done_next;      // link in the worker's completion list
    struct Conn *prev, *next;    // link in the worker's list of connections
} Conn;

struct Worker {
    int id;
    int epfd;
    int listen_fd;               // -1 once we stop accepting
    int efd;                     // eventfd: completions and shutdown wakeups
    pthread_t thread;
    Conn *conns;                 // every Conn this worker owns (open or awaiting a completion)
    int nconns;
    Conn *dead;                  // closed Conns, freed after the current batch of events

    pthread_mutex_t done_mu;     // guards done_head (pushed by the commit thread)
    Conn *done_head;
};

static Store *store;
static const char *port = DEFAULT_PORT;
static int max_connections = DEFAULT_MAX_CONNECTIONS;
static atomic_int open_connections;
static atomic_int stopping;

// epoll user data: a pointer to one of these markers means "listener" or
// "eventfd"; anything else is a Conn *.
static char LISTENER_TAG, EVENTFD_TAG;

// ---------------------------------------------------------------------------
// connection helpers

static void set_interest(Conn *c, unsigned events) {
    if (c->fd < 0 || events == c->events) return;
    struct epoll_event ev = { .events = events, .data.ptr = c };
    epoll_ctl(c->w->epfd, EPOLL_CTL_MOD, c->fd, &ev);
    c->events = events;
}

// Closes the socket. The Conn itself is not freed here:
//  - while a write is in flight the commit thread still holds a pointer to
//    it; the completion handler calls close_conn again afterwards;
//  - otherwise it goes on the worker's dead list and is freed at the end of
//    the current loop iteration, because the same epoll_wait batch may still
//    contain an event pointing at it.
static void close_conn(Conn *c) {
    if (c->fd >= 0) {
        epoll_ctl(c->w->epfd, EPOLL_CTL_DEL, c->fd, NULL);
        close(c->fd);
        c->fd = -1;
        atomic_fetch_sub(&open_connections, 1);
    }
    if (c->write_pending || c->dead) return;
    Worker *w = c->w;
    if (c->prev) c->prev->next = c->next; else w->conns = c->next;
    if (c->next) c->next->prev = c->prev;
    w->nconns--;
    c->dead = 1;
    c->next = w->dead;
    w->dead = c;
}

static void reap_dead(Worker *w) {
    while (w->dead) {
        Conn *c = w->dead;
        w->dead = c->next;
        free(c->out);
        free(c);
    }
}

static int append_out(Conn *c, const char *data, size_t len) {
    if (c->out_len + len > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 1024;
        while (cap < c->out_len + len) cap *= 2;
        char *p = realloc(c->out, cap);
        if (!p) return -1;
        c->out = p;
        c->out_cap = cap;
    }
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
    return 0;
}

static size_t out_pending(const Conn *c) { return c->out_len - c->out_off; }

// Sends as much buffered output as the socket takes right now.
// Returns -1 if the connection is broken.
static int flush_out(Conn *c) {
    while (out_pending(c) > 0) {
        ssize_t n = send(c->fd, c->out + c->out_off, out_pending(c), MSG_NOSIGNAL);
        if (n > 0) {
            c->out_off += (size_t)n;
        } else if (n == -1 && errno == EINTR) {
            continue;
        } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;                               // socket buffer full: wait for EPOLLOUT
        } else {
            return -1;                           // EPIPE, ECONNRESET, ...
        }
    }
    if (out_pending(c) == 0) c->out_len = c->out_off = 0;
    return 0;
}

static void on_write_done(void *ctx, StoreResult r);

static const char *result_text(StoreResult r) {
    return r == STORE_OK ? "OK\n" : r == STORE_NOT_FOUND ? "NOT_FOUND\n" : "ERROR\n";
}

// Executes buffered commands until the input runs out, a write has to wait
// for the commit thread, or replies pile up. Then flushes, decides what to
// wait for next, and closes the connection if it is finished.
//
// Only one write per connection is in flight at a time, and nothing after it
// is processed until it completes: replies come back in request order, and a
// pipelined "SET a 1 / GET a" always sees its own write.
static void process(Conn *c) {
    char line[MAX_LINE], value[MAX_LINE + 1];
    while (!c->write_pending && !atomic_load(&stopping) && out_pending(c) < OUT_HIGH_WATER &&
           linebuf_extract(&c->in, line, sizeof(line)) == 1) {
        Command cmd;
        const char *reply = "ERROR\n";
        if (parse_command(line, &cmd) == 0) {
            if (cmd.type == CMD_GET) {
                if (store_get(store, cmd.key, value, sizeof(value) - 1) == STORE_OK) {
                    size_t n = strlen(value);
                    value[n] = '\n';
                    if (append_out(c, value, n + 1) != 0) { close_conn(c); return; }
                    continue;
                }
                reply = "NOT_FOUND\n";
            } else {
                StoreResult r = (cmd.type == CMD_SET)
                    ? store_set_async(store, cmd.key, cmd.value, on_write_done, c)
                    : store_del_async(store, cmd.key, on_write_done, c);
                if (r == STORE_PENDING) {
                    c->write_pending = 1;
                    break;
                }
                reply = result_text(r);
            }
        }
        if (append_out(c, reply, strlen(reply)) != 0) { close_conn(c); return; }
    }

    // Line buffer full and still no '\n': a single line longer than
    // MAX_LINE -- protocol violation, drop the client.
    if (!c->write_pending && linebuf_space(&c->in) == 0) {
        close_conn(c);
        return;
    }
    if (flush_out(c) != 0) {
        close_conn(c);
        return;
    }
    // Finished: the client stopped sending (or we are shutting down), nothing
    // is in flight, and every reply has been delivered.
    if ((c->peer_eof || atomic_load(&stopping)) && !c->write_pending && out_pending(c) == 0) {
        char probe[MAX_LINE];
        LineBuf copy = c->in;                    // any complete command left?
        if (atomic_load(&stopping) || linebuf_extract(&copy, probe, sizeof(probe)) == 0) {
            close_conn(c);
            return;
        }
    }

    unsigned want = 0;
    if (!c->write_pending && !c->peer_eof && !atomic_load(&stopping) &&
        out_pending(c) < OUT_HIGH_WATER) want |= EPOLLIN;
    if (out_pending(c) > 0) want |= EPOLLOUT;
    set_interest(c, want);
}

static void on_readable(Conn *c) {
    size_t space = linebuf_space(&c->in);
    if (space > 0) {
        char chunk[MAX_LINE];
        ssize_t n = recv(c->fd, chunk, space, 0);
        if (n > 0) {
            linebuf_append(&c->in, chunk, (size_t)n);
        } else if (n == 0) {
            c->peer_eof = 1;                     // finish what was sent, then close
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close_conn(c);
            return;
        }
    }
    process(c);
}

// Called on the commit thread. Must not touch the socket or the epoll set:
// just queue the connection for its worker and wake the worker up.
static void on_write_done(void *ctx, StoreResult r) {
    Conn *c = ctx;
    Worker *w = c->w;
    c->result = r;
    pthread_mutex_lock(&w->done_mu);
    c->done_next = w->done_head;
    w->done_head = c;
    pthread_mutex_unlock(&w->done_mu);
    uint64_t one = 1;
    ssize_t rc = write(w->efd, &one, sizeof(one));
    (void)rc;                                    // eventfd write only fails on overflow
}

static void handle_completions(Worker *w) {
    uint64_t n;
    ssize_t rc = read(w->efd, &n, sizeof(n));
    (void)rc;
    pthread_mutex_lock(&w->done_mu);
    Conn *list = w->done_head;
    w->done_head = NULL;
    pthread_mutex_unlock(&w->done_mu);

    while (list) {
        Conn *c = list;
        list = c->done_next;
        c->write_pending = 0;
        if (c->fd < 0) {                         // client went away meanwhile
            close_conn(c);
            continue;
        }
        const char *reply = result_text(c->result);
        if (append_out(c, reply, strlen(reply)) != 0) {
            close_conn(c);
            continue;
        }
        process(c);                              // continue with pipelined commands
    }
}

static void accept_clients(Worker *w) {
    for (;;) {
        int fd = accept4(w->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd == -1) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) perror("accept");
            return;
        }
        if (atomic_fetch_add(&open_connections, 1) >= max_connections) {
            static const char msg[] = "ERROR too many connections\n";
            ssize_t rc = send(fd, msg, sizeof(msg) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
            (void)rc;
            close(fd);
            atomic_fetch_sub(&open_connections, 1);
            continue;
        }
        int one = 1;                             // small replies: don't let Nagle hold them
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        Conn *c = calloc(1, sizeof(*c));
        if (!c) {
            close(fd);
            atomic_fetch_sub(&open_connections, 1);
            continue;
        }
        c->fd = fd;
        c->w = w;
        linebuf_init(&c->in);
        c->events = EPOLLIN;
        struct epoll_event ev = { .events = EPOLLIN, .data.ptr = c };
        if (epoll_ctl(w->epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
            close(fd);
            free(c);
            atomic_fetch_sub(&open_connections, 1);
            continue;
        }
        c->next = w->conns;
        if (w->conns) w->conns->prev = c;
        w->conns = c;
        w->nconns++;
    }
}

// ---------------------------------------------------------------------------
// worker thread

static int make_listener(void) {
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    int status = getaddrinfo(NULL, port, &hints, &res);
    if (status != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        return -1;
    }
    int fd = -1;
    for (p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, p->ai_protocol);
        if (fd == -1) continue;
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        // every worker binds the same port; the kernel load-balances
        // incoming connections across the listening sockets
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
        if (bind(fd, p->ai_addr, p->ai_addrlen) == 0 && listen(fd, BACKLOG) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd == -1) fprintf(stderr, "failed to listen on port %s: %s\n", port, strerror(errno));
    return fd;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// Graceful stop for one worker: stop accepting, stop reading new commands,
// let writes already handed to the commit thread finish and their replies
// go out, then close everything.
static void begin_shutdown(Worker *w) {
    if (w->listen_fd >= 0) {
        epoll_ctl(w->epfd, EPOLL_CTL_DEL, w->listen_fd, NULL);
        close(w->listen_fd);
        w->listen_fd = -1;
    }
    for (Conn *c = w->conns, *next; c; c = next) {
        next = c->next;
        if (c->fd >= 0 && !c->write_pending) process(c);   // flushes and closes it
    }
}

static void *worker_main(void *arg) {
    Worker *w = arg;
    struct epoll_event events[MAX_EVENTS];
    long deadline = 0;

    for (;;) {
        if (atomic_load(&stopping)) {
            if (!deadline) {
                begin_shutdown(w);
                deadline = now_ms() + SHUTDOWN_GRACE_MS;
            }
            if (w->nconns == 0 || now_ms() > deadline) break;
        }
        int n = epoll_wait(w->epfd, events, MAX_EVENTS, deadline ? 100 : -1);
        if (n == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            void *tag = events[i].data.ptr;
            if (tag == &LISTENER_TAG) {
                accept_clients(w);
            } else if (tag == &EVENTFD_TAG) {
                handle_completions(w);
            } else {
                Conn *c = tag;
                if (c->fd < 0) continue;                    // closed earlier in this batch
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    // Reset or fully closed: nobody will read our replies. (These
                    // are reported even without EPOLLIN interest, so handling them
                    // lazily would spin while a write is in flight.)
                    close_conn(c);
                } else {
                    if (events[i].events & EPOLLIN) on_readable(c);
                    if (c->fd >= 0 && (events[i].events & EPOLLOUT)) process(c);
                }
            }
        }
        reap_dead(w);
    }
    // Past the grace period: drop whatever is left. Connections still waiting
    // for the commit thread are freed by handle_completions -- which no longer
    // runs -- so they're intentionally leaked; the process is exiting.
    for (Conn *c = w->conns, *next; c; c = next) {
        next = c->next;
        if (c->fd >= 0) close_conn(c);
    }
    reap_dead(w);
    return NULL;
}

static int worker_init(Worker *w, int id) {
    memset(w, 0, sizeof(*w));
    w->id = id;
    pthread_mutex_init(&w->done_mu, NULL);
    w->listen_fd = make_listener();
    w->epfd = epoll_create1(EPOLL_CLOEXEC);
    w->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (w->listen_fd < 0 || w->epfd < 0 || w->efd < 0) return -1;
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = &LISTENER_TAG };
    epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->listen_fd, &ev);
    ev.data.ptr = &EVENTFD_TAG;
    epoll_ctl(w->epfd, EPOLL_CTL_ADD, w->efd, &ev);
    return 0;
}

static void wake(Worker *w) {
    uint64_t one = 1;
    ssize_t rc = write(w->efd, &one, sizeof(one));
    (void)rc;
}

// ---------------------------------------------------------------------------
// main

static void usage(FILE *out, const char *prog) {
    fprintf(out,
        "usage: %s [options]\n"
        "  -p, --port PORT            TCP port (default " DEFAULT_PORT ")\n"
        "  -w, --wal PATH             write-ahead log (default " DEFAULT_WAL "); the snapshot\n"
        "                             is kept next to it as PATH.snap\n"
        "  -t, --threads N            event-loop threads (default: number of CPUs)\n"
        "  -c, --max-connections N    refuse clients beyond this (default %d)\n"
        "  -s, --compact-bytes N      snapshot + truncate the log when it grows past N\n"
        "                             bytes; 0 = never (default %llu)\n"
        "  -m, --mode MODE            group   batch writes, one fsync per batch (default)\n"
        "                             serial  one fsync per write, reads wait for it (baseline)\n"
        "                             nosync  no fsync at all -- NOT durable, benchmarks only\n"
        "      --repair               if the log is damaged mid-file, truncate it there\n"
        "                             (losing what follows) instead of refusing to start\n"
        "  -h, --help\n",
        prog, DEFAULT_MAX_CONNECTIONS, (unsigned long long)DEFAULT_COMPACT_BYTES);
}

static int parse_long(const char *s, long min, long *out) {
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *end || end == s || v < min) return -1;
    *out = v;
    return 0;
}

static void raise_fd_limit(void) {
    // each connection is a file descriptor; the default soft limit (often
    // 1024) would cap us far below --max-connections
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

int main(int argc, char **argv) {
    const char *wal_path = DEFAULT_WAL;
    long threads = sysconf(_SC_NPROCESSORS_ONLN);
    if (threads < 1) threads = 1;
    StoreOptions opts = { STORE_MODE_GROUP, DEFAULT_COMPACT_BYTES, 0 };

    static const struct option longopts[] = {
        { "port",            required_argument, NULL, 'p' },
        { "wal",             required_argument, NULL, 'w' },
        { "threads",         required_argument, NULL, 't' },
        { "max-connections", required_argument, NULL, 'c' },
        { "compact-bytes",   required_argument, NULL, 's' },
        { "mode",            required_argument, NULL, 'm' },
        { "repair",          no_argument,       NULL, 'R' },
        { "help",            no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    int opt;
    long v;
    while ((opt = getopt_long(argc, argv, "p:w:t:c:s:m:h", longopts, NULL)) != -1) {
        switch (opt) {
        case 'p': port = optarg; break;
        case 'w': wal_path = optarg; break;
        case 't': if (parse_long(optarg, 1, &v)) goto bad; threads = v; break;
        case 'c': if (parse_long(optarg, 1, &v)) goto bad; max_connections = (int)v; break;
        case 's': if (parse_long(optarg, 0, &v)) goto bad; opts.compact_bytes = (uint64_t)v; break;
        case 'R': opts.repair = 1; break;
        case 'm':
            if      (strcmp(optarg, "group")  == 0) opts.mode = STORE_MODE_GROUP;
            else if (strcmp(optarg, "serial") == 0) opts.mode = STORE_MODE_SERIAL;
            else if (strcmp(optarg, "nosync") == 0) opts.mode = STORE_MODE_NOSYNC;
            else goto bad;
            break;
        case 'h': usage(stdout, argv[0]); return 0;
        default: goto bad;
        }
    }
    if (optind != argc) goto bad;

    // Block the shutdown signals in every thread; main waits for them with
    // sigwait() below instead of running code in a signal handler.
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);
    // a client disconnecting mid-reply must not kill the process
    // (send() uses MSG_NOSIGNAL too; this covers everything else)
    signal(SIGPIPE, SIG_IGN);
    raise_fd_limit();

    store = store_open(wal_path, &opts);
    if (!store) return 1;

    Worker *workers = calloc((size_t)threads, sizeof(Worker));
    for (long i = 0; i < threads; i++) {
        if (worker_init(&workers[i], (int)i) != 0) {
            store_close(store);
            return 1;
        }
    }
    for (long i = 0; i < threads; i++) {
        pthread_create(&workers[i].thread, NULL, worker_main, &workers[i]);
    }

    StoreStats st = store_stats(store);
    printf("tinykv listening on port %s (%ld threads, mode %s, wal %s, %llu keys)\n",
           port, threads,
           opts.mode == STORE_MODE_GROUP ? "group" : opts.mode == STORE_MODE_SERIAL ? "serial" : "nosync",
           wal_path, (unsigned long long)st.keys);
    fflush(stdout);

    int sig = 0;
    sigwait(&sigs, &sig);
    printf("received %s, shutting down\n", sig == SIGINT ? "SIGINT" : "SIGTERM");
    fflush(stdout);

    atomic_store(&stopping, 1);
    for (long i = 0; i < threads; i++) wake(&workers[i]);
    for (long i = 0; i < threads; i++) pthread_join(workers[i].thread, NULL);

    store_close(store);          // commits anything still queued
    for (long i = 0; i < threads; i++) {
        close(workers[i].epfd);
        close(workers[i].efd);
        pthread_mutex_destroy(&workers[i].done_mu);
    }
    free(workers);
    printf("bye\n");
    return 0;

bad:
    usage(stderr, argv[0]);
    return 2;
}
