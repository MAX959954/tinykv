#include "wal.h"
#include "command.h"
#include "crc32.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ---- record format ----

size_t wal_format_record(char *buf, size_t cap, const char *payload) {
    size_t plen = strlen(payload);
    if (WAL_CRC_PREFIX + plen + 2 > cap) return 0;       // + '\n' + NUL
    snprintf(buf, cap, "%08x %s\n", (unsigned)crc32(payload, plen), payload);
    return WAL_CRC_PREFIX + plen + 1;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int wal_record_payload(const char *line, size_t len, char *out, size_t cap) {
    if (len <= WAL_CRC_PREFIX || len >= WAL_MAX_RECORD || line[8] != ' ') return -1;
    uint32_t want = 0;
    for (int i = 0; i < 8; i++) {
        int d = hex_digit(line[i]);
        if (d < 0) return -1;
        want = (want << 4) | (uint32_t)d;
    }
    const char *payload = line + WAL_CRC_PREFIX;
    size_t plen = len - WAL_CRC_PREFIX;
    if (plen >= cap || crc32(payload, plen) != want) return -1;
    // the CRC matched, so the payload is exactly what was written -- but a
    // stray NUL would make the C-string parser see something shorter
    if (memchr(payload, '\0', plen)) return -1;
    memcpy(out, payload, plen);
    out[plen] = '\0';
    return 0;
}

int wal_parse_record(const char *line, size_t len, Command *cmd) {
    char payload[WAL_MAX_RECORD];
    if (wal_record_payload(line, len, payload, sizeof(payload)) != 0) return -1;
    if (parse_command(payload, cmd) != 0) return -1;
    return (cmd->type == CMD_SET || cmd->type == CMD_DEL) ? 0 : -1;
}

// ---- the log file ----

struct Wal {
    int fd;
    int sync;                 // 0 = never fsync (benchmark mode)
    int failed;               // sticky: set after the first I/O error
    uint64_t size;
    uint64_t sync_count;
};

Wal *wal_open(const char *path, int sync) {
    Wal *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (w->fd == -1) {
        free(w);
        return NULL;
    }
    struct stat st;
    if (fstat(w->fd, &st) == 0 && S_ISREG(st.st_mode)) w->size = (uint64_t)st.st_size;
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
    w->size += len;
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

int wal_truncate(Wal *w) {
    if (w->failed) return -1;
    // O_APPEND: the next write goes to the (new) end of file, i.e. offset 0
    if (ftruncate(w->fd, 0) != 0 || (w->sync && fsync(w->fd) != 0)) {
        perror("wal: truncate");
        w->failed = 1;
        return -1;
    }
    w->size = 0;
    return 0;
}

int wal_failed(const Wal *w) { return w->failed; }
uint64_t wal_size(const Wal *w) { return w->size; }
uint64_t wal_sync_count(const Wal *w) { return w->sync_count; }

// ---- replay ----

const char *replay_status_str(ReplayStatus s) {
    switch (s) {
    case REPLAY_OK:        return "ok";
    case REPLAY_TORN_TAIL: return "torn tail";
    case REPLAY_CORRUPT:   return "corrupt";
    case REPLAY_NOMEM:     return "out of memory";
    case REPLAY_IO:        return "I/O error";
    }
    return "?";
}

ReplayResult replay_log(const char *path, HashMap *map) {
    ReplayResult r = { REPLAY_OK, 0, 0, -1 };
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno != ENOENT) r.status = REPLAY_IO;
        return r;
    }
    // Only regular files hold records. (Tests point the log at /dev/full,
    // which reads as an endless stream of zeros.)
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(f);
        return r;
    }

    // getline (not fgets): it reports the real length even if the line
    // contains NUL bytes, e.g. zero-filled blocks left by a crash.
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    long offset = 0;
    while ((n = getline(&line, &cap, f)) != -1) {
        int complete = (n > 0 && line[n - 1] == '\n');
        Command cmd;
        if (!complete || wal_parse_record(line, (size_t)n - 1, &cmd) != 0) {
            // Bad record. If it's the last thing in the file it's what a
            // crash mid-append leaves behind; if valid-looking data follows,
            // the middle of the log is damaged.
            r.bad_offset = offset;
            r.status = (getc(f) == EOF) ? REPLAY_TORN_TAIL : REPLAY_CORRUPT;
            break;
        }
        int rc = (cmd.type == CMD_SET) ? hashmap_set(map, cmd.key, cmd.value)
                                       : (hashmap_del(map, cmd.key), 0);
        if (rc != 0) {
            r.status = REPLAY_NOMEM;
            break;
        }
        offset += (long)n;
        r.records++;
    }
    if (ferror(f) && r.status == REPLAY_OK) r.status = REPLAY_IO;
    free(line);
    fclose(f);
    r.valid_bytes = offset;
    return r;
}
