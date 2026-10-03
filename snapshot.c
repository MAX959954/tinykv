#include "snapshot.h"
#include "wal.h"
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    FILE *f;
    long count;
} WriteCtx;

static int write_entry(const char *key, const char *value, void *arg) {
    WriteCtx *ctx = arg;
    char payload[WAL_MAX_RECORD], record[WAL_MAX_RECORD];
    int n = snprintf(payload, sizeof(payload), "SET %s %s", key, value);
    if (n < 0 || (size_t)n >= sizeof(payload)) return -1;
    size_t len = wal_format_record(record, sizeof(record), payload);
    if (len == 0 || fwrite(record, 1, len, ctx->f) != len) return -1;
    ctx->count++;
    return 0;
}

// fsync the directory containing path, so a rename inside it is durable.
static int fsync_parent_dir(const char *path) {
    char *copy = strdup(path);
    if (!copy) return -1;
    int fd = open(dirname(copy), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(copy);
    if (fd == -1) return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

int snapshot_write(const char *path, const HashMap *map, int sync) {
    char tmp[4096];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;

    FILE *f = fopen(tmp, "w");
    if (!f) {
        perror("snapshot: open");
        return -1;
    }
    WriteCtx ctx = { f, 0 };
    int ok = hashmap_foreach(map, write_entry, &ctx) == 0;

    if (ok) {
        char payload[64], record[128];
        snprintf(payload, sizeof(payload), "END %ld", ctx.count);
        size_t len = wal_format_record(record, sizeof(record), payload);
        ok = fwrite(record, 1, len, f) == len;
    }
    ok = ok && fflush(f) == 0;
    ok = ok && (!sync || fsync(fileno(f)) == 0);         // data on disk before the rename
    ok = (fclose(f) == 0) && ok;
    ok = ok && rename(tmp, path) == 0;                   // atomic replace
    ok = ok && (!sync || fsync_parent_dir(path) == 0);   // the rename itself durable

    if (!ok) {
        perror("snapshot: write");
        unlink(tmp);
        return -1;
    }
    return 0;
}

const char *snapshot_status_str(SnapStatus s) {
    switch (s) {
    case SNAP_OK:      return "ok";
    case SNAP_MISSING: return "missing";
    case SNAP_CORRUPT: return "corrupt";
    case SNAP_NOMEM:   return "out of memory";
    case SNAP_IO:      return "I/O error";
    }
    return "?";
}

SnapStatus snapshot_load(const char *path, HashMap *map, long *entries) {
    *entries = 0;
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? SNAP_MISSING : SNAP_IO;

    SnapStatus status = SNAP_CORRUPT;                    // until we see a valid trailer
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    char payload[WAL_MAX_RECORD];
    long count = 0;
    while ((n = getline(&line, &cap, f)) != -1) {
        if (n == 0 || line[n - 1] != '\n') break;
        if (wal_record_payload(line, (size_t)n - 1, payload, sizeof(payload)) != 0) break;

        if (strncmp(payload, "END ", 4) == 0) {
            char *end;
            long want = strtol(payload + 4, &end, 10);
            // the trailer must match the record count and be the last line
            if (*end == '\0' && want == count && getc(f) == EOF) status = SNAP_OK;
            break;
        }
        Command cmd;
        if (parse_command(payload, &cmd) != 0 || cmd.type != CMD_SET) break;
        if (hashmap_set(map, cmd.key, cmd.value) != 0) {
            status = SNAP_NOMEM;
            break;
        }
        count++;
    }
    if (ferror(f)) status = SNAP_IO;
    free(line);
    fclose(f);
    *entries = count;
    return status;
}
