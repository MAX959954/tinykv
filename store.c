#include "store.h"
#include "hashmap.h"
#include "snapshot.h"
#include "wal.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define INITIAL_BUCKETS 1024
#define MAX_BATCH 256            // max writes committed by one fsync
#define PATH_MAX_LEN 4096

typedef enum { REQ_SET, REQ_DEL, REQ_COMPACT } ReqType;

// One queued request. Heap-allocated by the submitting thread, owned by the
// commit thread from then on, freed after its callback has run.
typedef struct Request {
    ReqType op;
    char key[MAX_KEY + 1];
    char value[MAX_LINE];
    char record[WAL_MAX_RECORD];
    size_t record_len;
    StoreResult result;
    store_done_fn done;
    void *ctx;
    struct Request *next;
} Request;

struct Store {
    StoreOptions opts;
    char snap_path[PATH_MAX_LEN];
    HashMap *map;
    Wal *wal;                    // touched only by the commit thread after open

    // Guards the map. Readers share it; the commit thread takes it
    // exclusively only to apply a batch (in SERIAL mode: also during fsync).
    pthread_rwlock_t map_lock;

    // The commit queue and everything below is guarded by q_mu.
    pthread_mutex_t q_mu;
    pthread_cond_t q_cv;
    Request *q_head, *q_tail;
    int stopping;
    int failed;                  // log failed: refuse new writes
    StoreStats stats;            // keys is filled in by store_stats()

    uint64_t next_compact_at;    // commit thread only
    char *batch_buf;             // commit thread only
    pthread_t committer;
};

// ---------------------------------------------------------------------------
// commit thread

// Applies one durable write to the map. Called with map_lock held exclusively.
static StoreResult apply(Store *s, const Request *r) {
    if (r->op == REQ_SET) {
        if (hashmap_set(s->map, r->key, r->value) != 0) {
            // The write is already durable but can't be applied in memory.
            // Replying ERROR would be a lie (it reappears after a restart),
            // so stop instead: the log is the source of truth, and replay on
            // the next start rebuilds a consistent state.
            fprintf(stderr, "store: out of memory applying SET; exiting (WAL is intact)\n");
            abort();
        }
        return STORE_OK;
    }
    // Another DEL of the same key may have been applied between the
    // existence check at submit time and now; the later one reports NOT_FOUND.
    return hashmap_del(s->map, r->key) == 0 ? STORE_OK : STORE_NOT_FOUND;
}

// Snapshot + truncate. Runs on the commit thread, so no write can be applied
// while it runs: the snapshot captures exactly the state the log describes.
// Readers keep going (shared lock); writers queue up until it's done.
//
// Crash safety: the snapshot becomes visible only by an atomic rename, and
// the log is truncated only after that. A crash in between leaves the new
// snapshot AND the full old log -- replaying the log on top of a snapshot
// that already contains it is harmless, because applying the same sequence
// of SET/DEL records twice gives the same result as applying it once.
static int compact(Store *s) {
    if (wal_failed(s->wal)) return -1;
    int sync = s->opts.mode != STORE_MODE_NOSYNC;

    pthread_rwlock_rdlock(&s->map_lock);
    int rc = snapshot_write(s->snap_path, s->map, sync);
    pthread_rwlock_unlock(&s->map_lock);

    if (rc == 0) rc = wal_truncate(s->wal);

    pthread_mutex_lock(&s->q_mu);
    if (rc == 0) s->stats.compactions++;
    if (wal_failed(s->wal)) s->failed = 1;
    pthread_mutex_unlock(&s->q_mu);

    // On failure (e.g. disk full) try again only after the log has grown by
    // another compact_bytes, instead of after every single batch.
    s->next_compact_at = wal_size(s->wal) + s->opts.compact_bytes;
    return rc;
}

static void commit_batch(Store *s, Request *batch) {
    int serial = s->opts.mode == STORE_MODE_SERIAL;

    size_t len = 0;
    for (Request *r = batch; r; r = r->next) {
        memcpy(s->batch_buf + len, r->record, r->record_len);
        len += r->record_len;
    }

    // SERIAL emulates the original design: the map stays locked while the
    // disk is flushed, so readers wait for every fsync.
    if (serial) pthread_rwlock_wrlock(&s->map_lock);
    int ok = wal_write(s->wal, s->batch_buf, len) == 0;
    if (!serial) pthread_rwlock_wrlock(&s->map_lock);
    // Apply in queue order == log order, so memory always matches what a
    // replay of the log would produce, even for concurrent writes to one key.
    if (ok) {
        for (Request *r = batch; r; r = r->next) r->result = apply(s, r);
    }
    pthread_rwlock_unlock(&s->map_lock);

    pthread_mutex_lock(&s->q_mu);
    if (!ok) s->failed = 1;
    s->stats.batches++;
    s->stats.syncs = wal_sync_count(s->wal);
    s->stats.wal_bytes = wal_size(s->wal);
    pthread_mutex_unlock(&s->q_mu);

    for (Request *r = batch; r; ) {
        Request *next = r->next;
        r->done(r->ctx, ok ? r->result : STORE_ERROR);
        free(r);
        r = next;
    }
}

static void *commit_loop(void *arg) {
    Store *s = arg;
    size_t max_batch = (s->opts.mode == STORE_MODE_SERIAL) ? 1 : MAX_BATCH;

    pthread_mutex_lock(&s->q_mu);
    for (;;) {
        while (!s->q_head && !s->stopping) pthread_cond_wait(&s->q_cv, &s->q_mu);
        if (!s->q_head) break;                       // stopping and drained

        // Take everything queued (up to max_batch), stopping before a
        // compaction request so that it runs on its own, in order.
        Request *batch = s->q_head, *last = batch;
        if (batch->op == REQ_COMPACT) {
            s->q_head = batch->next;
            batch->next = NULL;
        } else {
            size_t n = 1;
            while (last->next && last->next->op != REQ_COMPACT && n < max_batch) {
                last = last->next;
                n++;
            }
            s->q_head = last->next;
            last->next = NULL;
        }
        if (!s->q_head) s->q_tail = NULL;
        pthread_mutex_unlock(&s->q_mu);

        // The slow part runs without q_mu: clients keep queueing meanwhile,
        // and those writes become the next batch.
        if (batch->op == REQ_COMPACT) {
            int rc = compact(s);
            batch->done(batch->ctx, rc == 0 ? STORE_OK : STORE_ERROR);
            free(batch);
        } else {
            commit_batch(s, batch);
            if (s->opts.compact_bytes && wal_size(s->wal) >= s->next_compact_at) {
                compact(s);
            }
        }
        pthread_mutex_lock(&s->q_mu);
    }
    pthread_mutex_unlock(&s->q_mu);
    return NULL;
}

// ---------------------------------------------------------------------------
// open / close

// Truncates path to len bytes if it is a regular file longer than that.
static int truncate_file(const char *path, long len) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= len) return 0;
    return truncate(path, len);
}

static int recover(Store *s, const char *wal_path) {
    // A leftover temporary file means a crash in the middle of writing a
    // snapshot; the previous snapshot (if any) and the log are still intact.
    char tmp[PATH_MAX_LEN + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s->snap_path);
    if (unlink(tmp) == 0) fprintf(stderr, "store: removed unfinished snapshot %s\n", tmp);

    long entries = 0;
    SnapStatus ss = snapshot_load(s->snap_path, s->map, &entries);
    if (ss != SNAP_OK && ss != SNAP_MISSING) {
        fprintf(stderr, "store: snapshot %s is %s; refusing to start\n",
                s->snap_path, snapshot_status_str(ss));
        return -1;
    }

    ReplayResult rr = replay_log(wal_path, s->map);
    switch (rr.status) {
    case REPLAY_OK:
        break;
    case REPLAY_TORN_TAIL:
        // A crash mid-append: drop the incomplete record. Otherwise the next
        // record would be glued onto it and both would be unreadable.
        fprintf(stderr, "store: dropping torn record at end of %s (offset %ld)\n",
                wal_path, rr.bad_offset);
        if (truncate_file(wal_path, rr.valid_bytes) != 0) {
            perror("store: truncate");
            return -1;
        }
        break;
    case REPLAY_CORRUPT:
        if (!s->opts.repair) {
            fprintf(stderr,
                    "store: %s is damaged at offset %ld (bad checksum or record, with\n"
                    "       more data after it). %ld records before it are fine.\n"
                    "       Refusing to start. Restart with --repair to truncate the log at\n"
                    "       that offset -- every record after it will be LOST.\n",
                    wal_path, rr.bad_offset, rr.records);
            return -1;
        }
        fprintf(stderr, "store: --repair: truncating %s at offset %ld, discarding the rest\n",
                wal_path, rr.bad_offset);
        if (truncate_file(wal_path, rr.valid_bytes) != 0) {
            perror("store: truncate");
            return -1;
        }
        break;
    case REPLAY_NOMEM:
    case REPLAY_IO:
        fprintf(stderr, "store: replaying %s failed: %s\n", wal_path,
                replay_status_str(rr.status));
        return -1;
    }
    return 0;
}

Store *store_open(const char *wal_path, const StoreOptions *opts) {
    Store *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->opts = *opts;
    if (snprintf(s->snap_path, sizeof(s->snap_path), "%s.snap", wal_path)
            >= (int)sizeof(s->snap_path)) {
        fprintf(stderr, "store: path too long\n");
        free(s);
        return NULL;
    }

    s->map = hashmap_create(INITIAL_BUCKETS);
    s->batch_buf = malloc((size_t)MAX_BATCH * WAL_MAX_RECORD);
    if (!s->map || !s->batch_buf) {
        fprintf(stderr, "store: out of memory\n");
        goto fail;
    }
    if (recover(s, wal_path) != 0) goto fail;

    s->wal = wal_open(wal_path, opts->mode != STORE_MODE_NOSYNC);
    if (!s->wal) {
        perror("store: open wal");
        goto fail;
    }
    s->next_compact_at = opts->compact_bytes;
    s->stats.wal_bytes = wal_size(s->wal);

    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
#ifdef __GLIBC__
    // glibc's default rwlock prefers readers: a steady stream of GETs could
    // starve the commit thread forever. Ask for writer preference instead.
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
    pthread_rwlock_init(&s->map_lock, &attr);
    pthread_rwlockattr_destroy(&attr);
    pthread_mutex_init(&s->q_mu, NULL);
    pthread_cond_init(&s->q_cv, NULL);

    int err = pthread_create(&s->committer, NULL, commit_loop, s);
    if (err != 0) {
        fprintf(stderr, "store: pthread_create: %s\n", strerror(err));
        pthread_rwlock_destroy(&s->map_lock);
        pthread_mutex_destroy(&s->q_mu);
        pthread_cond_destroy(&s->q_cv);
        goto fail;
    }
    return s;

fail:
    wal_close(s->wal);
    hashmap_destroy(s->map);
    free(s->batch_buf);
    free(s);
    return NULL;
}

void store_close(Store *s) {
    if (!s) return;
    pthread_mutex_lock(&s->q_mu);
    s->stopping = 1;                 // the commit thread drains the queue, then exits
    pthread_cond_signal(&s->q_cv);
    pthread_mutex_unlock(&s->q_mu);
    pthread_join(s->committer, NULL);

    wal_close(s->wal);
    hashmap_destroy(s->map);
    pthread_rwlock_destroy(&s->map_lock);
    pthread_mutex_destroy(&s->q_mu);
    pthread_cond_destroy(&s->q_cv);
    free(s->batch_buf);
    free(s);
}

// ---------------------------------------------------------------------------
// submitting

static StoreResult submit(Store *s, Request *r) {
    pthread_mutex_lock(&s->q_mu);
    if (s->failed || s->stopping) {
        pthread_mutex_unlock(&s->q_mu);
        free(r);
        return STORE_ERROR;
    }
    r->next = NULL;
    if (s->q_tail) s->q_tail->next = r; else s->q_head = r;
    s->q_tail = r;
    pthread_cond_signal(&s->q_cv);
    pthread_mutex_unlock(&s->q_mu);
    return STORE_PENDING;
}

static Request *make_request(ReqType op, const char *key, const char *value,
                             store_done_fn done, void *ctx) {
    Request *r = malloc(sizeof(*r));
    if (!r) return NULL;
    r->op = op;
    r->done = done;
    r->ctx = ctx;
    r->result = STORE_ERROR;
    r->record_len = 0;
    if (op == REQ_COMPACT) return r;

    size_t klen = strlen(key);
    size_t vlen = value ? strlen(value) : 0;
    if (klen > MAX_KEY || vlen >= sizeof(r->value)) {
        free(r);
        return NULL;
    }
    memcpy(r->key, key, klen + 1);
    if (value) memcpy(r->value, value, vlen + 1); else r->value[0] = '\0';

    char payload[WAL_MAX_RECORD];
    int n = (op == REQ_SET) ? snprintf(payload, sizeof(payload), "SET %s %s", key, value)
                            : snprintf(payload, sizeof(payload), "DEL %s", key);
    // A record that didn't fit would be cut short and stop replay there.
    if (n < 0 || (size_t)n >= sizeof(payload)) {
        free(r);
        return NULL;
    }
    r->record_len = wal_format_record(r->record, sizeof(r->record), payload);
    if (r->record_len == 0) {
        free(r);
        return NULL;
    }
    return r;
}

StoreResult store_set_async(Store *s, const char *key, const char *value,
                            store_done_fn done, void *ctx) {
    Request *r = make_request(REQ_SET, key, value, done, ctx);
    return r ? submit(s, r) : STORE_ERROR;
}

StoreResult store_del_async(Store *s, const char *key, store_done_fn done, void *ctx) {
    // Deleting a missing key changes nothing, so don't log it.
    pthread_rwlock_rdlock(&s->map_lock);
    int present = hashmap_get(s->map, key) != NULL;
    pthread_rwlock_unlock(&s->map_lock);
    if (!present) return STORE_NOT_FOUND;

    Request *r = make_request(REQ_DEL, key, NULL, done, ctx);
    return r ? submit(s, r) : STORE_ERROR;
}

// ---------------------------------------------------------------------------
// synchronous wrappers

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int done;
    StoreResult result;
} Waiter;

static void waiter_init(Waiter *w) {
    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cv, NULL);
    w->done = 0;
    w->result = STORE_ERROR;
}

static void waiter_wake(void *ctx, StoreResult r) {
    Waiter *w = ctx;
    pthread_mutex_lock(&w->mu);
    w->result = r;
    w->done = 1;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

static StoreResult waiter_wait(Waiter *w, StoreResult submitted) {
    if (submitted == STORE_PENDING) {
        pthread_mutex_lock(&w->mu);
        while (!w->done) pthread_cond_wait(&w->cv, &w->mu);
        pthread_mutex_unlock(&w->mu);
        submitted = w->result;
    }
    pthread_mutex_destroy(&w->mu);
    pthread_cond_destroy(&w->cv);
    return submitted;
}

StoreResult store_set(Store *s, const char *key, const char *value) {
    Waiter w;
    waiter_init(&w);
    return waiter_wait(&w, store_set_async(s, key, value, waiter_wake, &w));
}

StoreResult store_del(Store *s, const char *key) {
    Waiter w;
    waiter_init(&w);
    return waiter_wait(&w, store_del_async(s, key, waiter_wake, &w));
}

int store_compact(Store *s) {
    Waiter w;
    waiter_init(&w);
    Request *r = make_request(REQ_COMPACT, NULL, NULL, waiter_wake, &w);
    StoreResult res = waiter_wait(&w, r ? submit(s, r) : STORE_ERROR);
    return res == STORE_OK ? 0 : -1;
}

StoreResult store_get(Store *s, const char *key, char *out, size_t out_size) {
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
    return v ? STORE_OK : STORE_NOT_FOUND;
}

StoreStats store_stats(Store *s) {
    pthread_mutex_lock(&s->q_mu);
    StoreStats st = s->stats;
    pthread_mutex_unlock(&s->q_mu);
    pthread_rwlock_rdlock(&s->map_lock);
    st.keys = hashmap_size(s->map);
    pthread_rwlock_unlock(&s->map_lock);
    return st;
}
