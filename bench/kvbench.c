// kvbench -- load generator for tinykv.
//
// Opens C connections (one thread each); every thread sends N requests one
// at a time (send, wait for the reply, repeat) and records each request's
// latency. Prints total throughput and p50/p99 latency for SET and GET.
//
//   kvbench [-H host] [-p port] [-c clients] [-n ops_per_client]
//           [-r get_percent] [-k keys] [-d value_bytes] [-q]
//
// -r 0 = only SETs, -r 100 = only GETs, -r 90 = 90% GET / 10% SET.
// -q prints one machine-readable line:
//   ops_per_sec set_p50_us set_p99_us get_p50_us get_p99_us
#define _POSIX_C_SOURCE 200809L
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <time.h>
#include <unistd.h>

static const char *host = "127.0.0.1";
static const char *port = "9999";
static int clients = 8, ops = 2000, get_pct = 0, keys = 1000, value_size = 32, quiet = 0;

static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int ready = 0, go = 0;

typedef struct {
    int id;
    double *set_lat, *get_lat;   // microseconds
    int nset, nget, errors;
} Worker;

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static int connect_to_server(void) {
    struct addrinfo hints = {0}, *res, *p;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == -1) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd != -1) {
        int one = 1;   // small request/response messages: don't let Nagle delay them
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return fd;
}

// Sends one request line and reads exactly one reply line.
static int roundtrip(int fd, const char *req, size_t len, char *reply, size_t cap) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, req + off, len - off, 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    size_t got = 0;
    for (;;) {
        ssize_t n = recv(fd, reply + got, cap - 1 - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
        if (memchr(reply, '\n', got)) break;
        if (got == cap - 1) return -1;
    }
    reply[got] = '\0';
    return 0;
}

static void *run(void *arg) {
    Worker *w = arg;
    int fd = connect_to_server();
    if (fd == -1) { w->errors = ops; return NULL; }

    char value[1024];
    memset(value, 'v', (size_t)value_size);
    value[value_size] = '\0';
    char req[1200], reply[1200];
    unsigned seed = (unsigned)w->id * 2654435761u + 1;

    pthread_mutex_lock(&gate_mu);         // start all clients at the same moment
    ready++;
    pthread_cond_broadcast(&gate_cv);
    while (!go) pthread_cond_wait(&gate_cv, &gate_mu);
    pthread_mutex_unlock(&gate_mu);

    for (int i = 0; i < ops; i++) {
        seed = seed * 1103515245u + 12345u;
        int key = (int)((seed >> 8) % (unsigned)keys);
        int is_get = (int)((seed >> 4) % 100) < get_pct;
        int len = is_get ? snprintf(req, sizeof(req), "GET key:%d\n", key)
                         : snprintf(req, sizeof(req), "SET key:%d %s\n", key, value);
        double t0 = now_us();
        if (roundtrip(fd, req, (size_t)len, reply, sizeof(reply)) != 0) { w->errors++; break; }
        double dt = now_us() - t0;
        if (strncmp(reply, "ERROR", 5) == 0) w->errors++;
        if (is_get) w->get_lat[w->nget++] = dt;
        else        w->set_lat[w->nset++] = dt;
    }
    close(fd);
    return NULL;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double percentile(double *v, int n, double p) {
    if (n == 0) return 0;
    int idx = (int)(p / 100.0 * (n - 1) + 0.5);
    return v[idx];
}

static void prepopulate(void) {
    int fd = connect_to_server();
    if (fd == -1) { fprintf(stderr, "cannot connect to %s:%s\n", host, port); exit(1); }
    char req[1200], reply[64], value[1024];
    memset(value, 'v', (size_t)value_size);
    value[value_size] = '\0';
    for (int k = 0; k < keys; k++) {
        int len = snprintf(req, sizeof(req), "SET key:%d %s\n", k, value);
        if (roundtrip(fd, req, (size_t)len, reply, sizeof(reply)) != 0) {
            fprintf(stderr, "prepopulate failed\n"); exit(1);
        }
    }
    close(fd);
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "H:p:c:n:r:k:d:q")) != -1) {
        switch (opt) {
        case 'H': host = optarg; break;
        case 'p': port = optarg; break;
        case 'c': clients = atoi(optarg); break;
        case 'n': ops = atoi(optarg); break;
        case 'r': get_pct = atoi(optarg); break;
        case 'k': keys = atoi(optarg); break;
        case 'd': value_size = atoi(optarg); break;
        case 'q': quiet = 1; break;
        default:
            fprintf(stderr, "usage: %s [-H host] [-p port] [-c clients] [-n ops] "
                            "[-r get_percent] [-k keys] [-d value_bytes] [-q]\n", argv[0]);
            return 2;
        }
    }
    if (clients < 1 || ops < 1 || keys < 1 || value_size < 1 || value_size > 400 ||
        get_pct < 0 || get_pct > 100) {
        fprintf(stderr, "bad arguments\n");
        return 2;
    }

    if (get_pct > 0) prepopulate();      // so GETs hit real values

    Worker *w = calloc((size_t)clients, sizeof(Worker));
    pthread_t *th = calloc((size_t)clients, sizeof(pthread_t));
    for (int i = 0; i < clients; i++) {
        w[i].id = i + 1;
        w[i].set_lat = malloc(sizeof(double) * (size_t)ops);
        w[i].get_lat = malloc(sizeof(double) * (size_t)ops);
        pthread_create(&th[i], NULL, run, &w[i]);
    }

    pthread_mutex_lock(&gate_mu);
    while (ready < clients) pthread_cond_wait(&gate_cv, &gate_mu);
    double start = now_us();
    go = 1;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_mu);

    for (int i = 0; i < clients; i++) pthread_join(th[i], NULL);
    double elapsed = (now_us() - start) / 1e6;

    long total_set = 0, total_get = 0, errors = 0;
    for (int i = 0; i < clients; i++) {
        total_set += w[i].nset; total_get += w[i].nget; errors += w[i].errors;
    }
    double *all_set = malloc(sizeof(double) * (size_t)(total_set + 1));
    double *all_get = malloc(sizeof(double) * (size_t)(total_get + 1));
    long ps = 0, pg = 0;
    for (int i = 0; i < clients; i++) {
        memcpy(all_set + ps, w[i].set_lat, sizeof(double) * (size_t)w[i].nset); ps += w[i].nset;
        memcpy(all_get + pg, w[i].get_lat, sizeof(double) * (size_t)w[i].nget); pg += w[i].nget;
    }
    qsort(all_set, (size_t)total_set, sizeof(double), cmp_double);
    qsort(all_get, (size_t)total_get, sizeof(double), cmp_double);

    double tput = (double)(total_set + total_get) / elapsed;
    double s50 = percentile(all_set, (int)total_set, 50), s99 = percentile(all_set, (int)total_set, 99);
    double g50 = percentile(all_get, (int)total_get, 50), g99 = percentile(all_get, (int)total_get, 99);

    if (quiet) {
        printf("%.0f %.0f %.0f %.0f %.0f\n", tput, s50, s99, g50, g99);
    } else {
        printf("clients=%d ops=%ld (SET %ld, GET %ld) in %.2fs -> %.0f ops/s\n",
               clients, total_set + total_get, total_set, total_get, elapsed, tput);
        if (total_set) printf("  SET latency: p50 %.0f us, p99 %.0f us\n", s50, s99);
        if (total_get) printf("  GET latency: p50 %.0f us, p99 %.0f us\n", g50, g99);
    }
    if (errors) fprintf(stderr, "%ld errors\n", errors);
    return errors ? 1 : 0;
}
