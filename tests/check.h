#ifndef TINYKV_CHECK_H
#define TINYKV_CHECK_H
// Minimal test helpers. Unlike assert(), CHECK is not compiled out by
// NDEBUG (Release builds), and it reports every failure instead of
// stopping at the first one.
#include <ftw.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int check_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        check_failures++; \
    } \
} while (0)

#define CHECK_STR(a, b) do { \
    const char *a_ = (a), *b_ = (b); \
    if (!a_ || !b_ || strcmp(a_, b_) != 0) { \
        fprintf(stderr, "%s:%d: CHECK_STR failed: %s == \"%s\", expected \"%s\"\n", \
                __FILE__, __LINE__, #a, a_ ? a_ : "(null)", b_ ? b_ : "(null)"); \
        check_failures++; \
    } \
} while (0)

#define RUN(test) do { \
    int before_ = check_failures; \
    test(); \
    printf("%s  %s\n", check_failures == before_ ? "PASS" : "FAIL", #test); \
} while (0)

#define CHECK_DONE() (check_failures == 0 ? 0 : 1)

static inline int remove_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb; (void)flag; (void)ftw;
    return remove(path);
}

// rm -rf dir (children first), without spawning a shell
static inline void remove_tree(const char *dir) {
    if (nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0) perror("remove_tree");
}

#endif
