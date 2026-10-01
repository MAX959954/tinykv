// _GNU_SOURCE only for pthread_rwlockattr_setkind_np on glibc (see below)
#define _GNU_SOURCE
#include "store.h"
#include "hashmap.h"
#include "wal.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NBUCKETS 1024
#define MAX_BATCH 128            // max writes committed by one fsync

typedef enum { OP_SET, OP_DEL } OpType;

// One pending write. Lives on the stack of the client thread that issued
// it; that thread sleeps on its own condition variable until a leader has
// committed the write (or it becomes the leader itself).
typedef struct Writer {
    OpType op;
    const char *key, *value;
    char record[WAL_MAX_RECORD];
    size_t record_len;

    int done;
    StoreResult result;
    pthread_cond_t cv;
    struct Writer *next;
} Writer;

struct Store {
    StoreMode mode;
    HashMap *map;
    Wal *wal;

    // Guards the hash map. GETs take it shared, so any number of them run
    // in parallel; writers take it exclusively, but only for the brief
    // in-memory update -- never while the disk is being flushed.
    pthread_rwlock_t map_lock;

    // Group commit queue (guarded by q_mu). The writer at the head is the
    // leader: it takes a batch of queued writers, writes all their records
    // with one write() + one fsync(), applies them to the map in queue order
    // under one exclusive lock, then wakes each of them individually.
    // Writers that arrive while the leader is in fsync simply queue up and
    // form the next batch -- that's where the batching comes from.
    pthread_mutex_t q_mu;
    Writer *q_head, *q_tail;
    int failed;                  // copy of wal_failed(), readable under q_mu
    uint64_t syncs;              // copy of wal_sync_count(), under q_mu

    char *batch_buf;             // used only by the current leader

    // STORE_MODE_SERIAL only: the original single global lock, held for the
    // whole operation (including fsync), so every batch has exactly one write.
    pthread_mutex_t serial_mu;
};

Store *store_open(const char *wal_path, StoreMode mode) {
    Store *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->mode = mode;

    s->map = hashmap_create(NBUCKETS);
    s->batch_buf = malloc((size_t)MAX_BATCH * WAL_MAX_RECORD);
    if (!s->map || !s->batch_buf) goto fail;

    long valid = 0;
    if (replay_log(wal_path, s->map, &valid) != 0) {
        fprintf(stderr, "store: out of memory replaying %s\n", wal_path);
        goto fail;
    }
    // Drop a torn record left by a crash mid-append; otherwise the first
    // new record would be glued onto it and corrupt both on the next replay.
    struct stat st;
    if (stat(wal_path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > valid) {
        fprintf(stderr, "store: dropping %ld bytes of torn record at end of %s\n",
                (long)st.st_size - valid, wal_path);
        if (truncate(wal_path, valid) != 0) {
            perror("store: truncate wal");
            goto fail;
        }
    }

    s->wal = wal_open(wal_path, mode != STORE_MODE_NOSYNC);
    if (!s->wal) {
        perror("store: open wal");
        goto fail;
    }

    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
#ifdef __GLIBC__
    // glibc's default rwlock prefers readers: a steady stream of GETs could
    // starve writers forever. Ask for writer preference instead.
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
    pthread_rwlock_init(&s->map_lock, &attr);
    pthread_rwlockattr_destroy(&attr);

    pthread_mutex_init(&s->q_mu, NULL);
    pthread_mutex_init(&s->serial_mu, NULL);
    return s;

fail:
    if (s->map) hashmap_destroy(s->map);
    free(s->batch_buf);
    free(s);
    return NULL;
}

void store_close(Store *s) {
    if (!s) return;
    wal_close(s->wal);
    hashmap_destroy(s->map);
    pthread_rwlock_destroy(&s->map_lock);
    pthread_mutex_destroy(&s->q_mu);
    pthread_mutex_destroy(&s->serial_mu);
    free(s->batch_buf);
    free(s);
}

static void serial_lock(Store *s) {
    if (s->mode == STORE_MODE_SERIAL) pthread_mutex_lock(&s->serial_mu);
}
static void serial_unlock(Store *s) {
    if (s->mode == STORE_MODE_SERIAL) pthread_mutex_unlock(&s->serial_mu);
}

// Applies one durable write to the map. Called with map_lock held exclusively.
static StoreResult apply(Store *s, const Writer *w) {
    if (w->op == OP_SET) {
        if (hashmap_set(s->map, w->key, w->value) != 0) {
            // The write is already durable but can't be applied in memory.
            // Replying ERROR would be a lie (it reappears after a restart),
            // so stop instead: the log is the source of truth, and replay on
            // the next start rebuilds a consistent state.
            fprintf(stderr, "store: out of memory applying SET; exiting (WAL is intact)\n");
            abort();
        }
        return STORE_OK;
    }
    // Another DEL of the same key may have been applied between our
    // existence check and now; the later one reports NOT_FOUND.
    return hashmap_del(s->map, w->key) == 0 ? STORE_OK : STORE_NOT_FOUND;
}

static StoreResult commit(Store *s, Writer *w) {
    w->done = 0;
    w->next = NULL;
    pthread_cond_init(&w->cv, NULL);

    pthread_mutex_lock(&s->q_mu);
    if (s->failed) {
        pthread_mutex_unlock(&s->q_mu);
        pthread_cond_destroy(&w->cv);
        return STORE_ERROR;
    }
    if (s->q_tail) s->q_tail->next = w; else s->q_head = w;
    s->q_tail = w;

    // Sleep until either a leader has committed us, or we reach the head.
    while (!w->done && s->q_head != w) {
        pthread_cond_wait(&w->cv, &s->q_mu);
    }
    if (w->done) {
        pthread_mutex_unlock(&s->q_mu);
        pthread_cond_destroy(&w->cv);
        return w->result;
    }

    // We are the leader. Take up to MAX_BATCH writers starting with us.
    // Writers arriving from now on queue up behind the batch.
    Writer *last = w;
    size_t n = 1;
    while (last->next && n < MAX_BATCH) { last = last->next; n++; }
    int failed_before = s->failed;
    pthread_mutex_unlock(&s->q_mu);

    // Slow part, done without q_mu: everyone else can keep queueing.
    size_t len = 0;
    for (Writer *x = w; ; x = x->next) {
        memcpy(s->batch_buf + len, x->record, x->record_len);
        len += x->record_len;
        if (x == last) break;
    }
    int ok = !failed_before && wal_write(s->wal, s->batch_buf, len) == 0;

    // Apply in queue order == log order, so memory always matches what a
    // replay of the log would produce, even for concurrent writes to one key.
    if (ok) {
        pthread_rwlock_wrlock(&s->map_lock);
        for (Writer *x = w; ; x = x->next) {
            x->result = apply(s, x);
            if (x == last) break;
        }
        pthread_rwlock_unlock(&s->map_lock);
    }

    pthread_mutex_lock(&s->q_mu);
    if (!ok) s->failed = 1;
    s->syncs = wal_sync_count(s->wal);
    Writer *next_head = last->next;
    for (Writer *x = w; ; ) {
        Writer *after = x->next;             // read before x's owner may return
        if (!ok) x->result = STORE_ERROR;
        x->done = 1;
        if (x != w) pthread_cond_signal(&x->cv);
        if (x == last) break;
        x = after;
    }
    s->q_head = next_head;
    if (!next_head) s->q_tail = NULL;
    else pthread_cond_signal(&next_head->cv);   // hand leadership on
    StoreResult r = w->result;
    pthread_mutex_unlock(&s->q_mu);
    pthread_cond_destroy(&w->cv);
    return r;
}

StoreResult store_set(Store *s, const char *key, const char *value) {
    Writer w;
    int len = snprintf(w.record, sizeof(w.record), "SET %s %s\n", key, value);
    // A truncated record would lose its '\n' and stop replay at that point.
    if (len < 0 || (size_t)len >= sizeof(w.record)) return STORE_ERROR;
    w.record_len = (size_t)len;
    w.op = OP_SET;
    w.key = key;
    w.value = value;

    serial_lock(s);
    StoreResult r = commit(s, &w);
    serial_unlock(s);
    return r;
}

StoreResult store_del(Store *s, const char *key) {
    serial_lock(s);

    // Deleting a missing key changes nothing, so don't log it.
    pthread_rwlock_rdlock(&s->map_lock);
    int present = (hashmap_get(s->map, key) != NULL);
    pthread_rwlock_unlock(&s->map_lock);

    StoreResult r = STORE_NOT_FOUND;
    if (present) {
        Writer w;
        int len = snprintf(w.record, sizeof(w.record), "DEL %s\n", key);
        if (len < 0 || (size_t)len >= sizeof(w.record)) {
            r = STORE_ERROR;
        } else {
            w.record_len = (size_t)len;
            w.op = OP_DEL;
            w.key = key;
            w.value = NULL;
            r = commit(s, &w);
        }
    }
    serial_unlock(s);
    return r;
}

StoreResult store_get(Store *s, const char *key, char *out, size_t out_size) {
    serial_lock(s);
    pthread_rwlock_rdlock(&s->map_lock);
    const char *v = hashmap_get(s->map, key);
    if (v && out_size > 0) {
        // copy while still holding the lock: the map owns v, and a
        // concurrent SET of this key would free it
        size_t n = strlen(v);
        if (n >= out_size) n = out_size - 1;
        memcpy(out, v, n);
        out[n] = '\0';
    }
    pthread_rwlock_unlock(&s->map_lock);
    serial_unlock(s);
    return v ? STORE_OK : STORE_NOT_FOUND;
}

uint64_t store_sync_count(Store *s) {
    pthread_mutex_lock(&s->q_mu);
    uint64_t n = s->syncs;
    pthread_mutex_unlock(&s->q_mu);
    return n;
}
