#define _POSIX_C_SOURCE 200809L
#include "wal.h"
#include "command.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct Wal {
    int fd;
    int sync;                 // 0 = never fsync (benchmark mode)
    int failed;               // sticky: set after the first I/O error
    uint64_t sync_count;
};

Wal *wal_open(const char *path, int sync) {
    Wal *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (w->fd == -1) {
        free(w);
        return NULL;
    }
    w->sync = sync;
    return w;
}

void wal_close(Wal *w) {
    if (!w) return;
    close(w->fd);
    free(w);
}

// write(2) may write less than asked (or be interrupted); loop until done.
static int write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

int wal_write(Wal *w, const char *data, size_t len) {
    if (w->failed) return -1;
    if (write_all(w->fd, data, len) != 0) {
        perror("wal: write");
        w->failed = 1;
        return -1;
    }
    if (w->sync) {
        w->sync_count++;
        if (fsync(w->fd) != 0) {
            // After a failed fsync the kernel may have dropped the dirty
            // pages, so retrying and reporting success could be a lie.
            perror("wal: fsync");
            w->failed = 1;
            return -1;
        }
    }
    return 0;
}

int wal_failed(const Wal *w) {
    return w->failed;
}

uint64_t wal_sync_count(const Wal *w) {
    return w->sync_count;
}

int replay_log(const char *path, HashMap *map, long *valid_bytes) {
    *valid_bytes = 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[WAL_MAX_RECORD];
    long offset = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        // A record without a trailing '\n' can only be a torn write at the
        // very end of the log (crash mid-append) -- stop replaying there.
        if (len == 0 || line[len - 1] != '\n') {
            break;
        }
        offset += (long)len;
        line[len - 1] = '\0';

        Command cmd;
        if (parse_command(line, &cmd) != 0) continue;

        if (cmd.type == CMD_SET) {
            if (hashmap_set(map, cmd.key, cmd.value) != 0) {
                fclose(f);
                return -1;
            }
        } else if (cmd.type == CMD_DEL) {
            hashmap_del(map, cmd.key);
        }
    }
    fclose(f);
    *valid_bytes = offset;
    return 0;
}
