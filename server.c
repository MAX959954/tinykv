#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>

#include "linebuf.h"
#include "command.h"
#include "hashmap.h"
#include "wal.h"

#define PORT "9999"
#define BACKLOG 10
#define WAL_PATH "kv.log"

static HashMap *map;
static FILE * wal_file;

//Set (under g_lock) after the first failed WAL write. From then on every
//write is refused: after a failed fputs/fflush the log may end in a partial
//record, and appending after it would make later records unreadable on
//replay. Reads keep working; an operator restarts the server once the disk
//problem (full disk, I/O error) is fixed.
static int wal_failed = 0;

//Without a lock, two threads doing e.g. SET foo bar concurrently
// could interleave their reads/writes inside hashmap and corrupt
// it — classic data race, undefined behavior,
static pthread_mutex_t g_lock  = PTHREAD_MUTEX_INITIALIZER; //intializes it in compile time 


//Appends a record to the WAL. Must be called with g_lock held.
//Returns 1 if the record is durable, 0 if the write was refused or failed.
static int log_record(const char *record) {
    if (wal_failed) return 0;
    if (wal_write(wal_file , record) != 0) {
        perror("wal_write");
        fprintf(stderr , "WAL write failed; refusing further writes until restart\n");
        wal_failed = 1;
        return 0;
    }
    return 1;
}

//send() on a stream socket may write only part of the buffer (e.g. when the
//socket's send buffer is nearly full), so keep going until all of it is out.
//Returns 0 on success, -1 if the connection is broken.
static int send_all(int fd , const char *buf , size_t len) {
    while (len > 0) {
        ssize_t n = send(fd , buf , len , 0);
        if (n == -1) {
            if (errno == EINTR) continue;   //interrupted by a signal: retry
            return -1;                      //EPIPE, ECONNRESET, ...
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

//dispatch() is the glue — it takes a parsed command and turns it into
// an actual state change (hashmap + WAL) plus a reply, all under one 
// lock so SET-then-log can't be interrupted by another thread
static void dispatch(Command *cmd , char *reply , size_t reply_size){


    //(a struct with internal state — locked/unlocked, waiting 
    //threads, etc.). If you passed it by value, the function 
    //would get a copy of that state, lock the copy, and your 
    //real g_lock would stay untouched — so it wouldn't actually block other threads.
    pthread_mutex_lock(&g_lock);

    switch (cmd->type) {
    case CMD_SET: {
        //builds the text record that gets appended to the write-ahead log (WAL) file.
        //The buffer is sized for the largest possible key + value, and we
        //still check for truncation: a record cut short would lose its '\n'
        //and stop replay_log() from reading anything after it.
        char record[WAL_MAX_RECORD];
        int len = snprintf(record , sizeof(record) , "SET %s %s\n" , cmd->key , cmd->value);
        if (len < 0 || (size_t)len >= sizeof(record)) {
            snprintf(reply , reply_size , "ERROR\n");
            break;
        }
        //log first, then apply: memory must never contain a write that
        //isn't on disk, or a GET could return a value that a crash undoes.
        if (!log_record(record)) {
            snprintf(reply , reply_size , "ERROR\n");
            break;
        }
        if (hashmap_set(map, cmd->key, cmd->value) != 0) {
            //The write is already durable but can't be applied in memory.
            //Replying ERROR would be a lie (it reappears after a restart),
            //so stop instead: the WAL is the source of truth and replay on
            //the next start rebuilds a consistent state.
            fprintf(stderr , "out of memory applying SET; exiting (WAL is intact)\n");
            abort();
        }
        snprintf(reply  , reply_size , "OK\n");
        break;
    }
    case CMD_GET: {
        //looks up the value
       const char *v  = hashmap_get(map , cmd->key);
       snprintf(reply , reply_size , v ? "%s\n" : "NOT_FOUND\n", v ? v : "");
       break;
    }
    case CMD_DEL: {
        //deleting a missing key changes nothing, so there's nothing to log
        if (hashmap_get(map , cmd->key) == NULL) {
            snprintf(reply , reply_size , "NOT_FOUND\n");
            break;
        }
        //same order as SET: the DEL record must be durable before the key
        //disappears from memory, so a crash can't resurrect a deleted key
        //that a client was already told is gone.
        char record[WAL_MAX_RECORD];
        snprintf(record , sizeof(record) , "DEL %s\n" , cmd->key);
        if (!log_record(record)) {
            snprintf(reply , reply_size , "ERROR\n");
            break;
        }
        hashmap_del(map , cmd->key);
        snprintf(reply, reply_size, "OK\n");
        break;
    }
    default:
      snprintf(reply , reply_size , "ERROR\n");
    }

    //Unlock the mutex , so the next thread can get in
    pthread_mutex_unlock(&g_lock);
}

static void * handle_client(void *arg) {
    //the fd is passed by value, packed into the pointer argument itself
    //(see main), so each thread has its own copy and nothing needs freeing
    int fd = (int)(intptr_t)arg;

    //sets up a fresh per-connection line buffer (the linebuf.c reassembly
    // logic from earlier), scoped to this thread/connection only
    LineBuf lb;
    linebuf_init(&lb);
    /*
    three buffers: chunk is the raw recv scratch space, 
    line holds one extracted command line, reply holds the response text.
    */
    char chunk[512], line[MAX_LINE] , reply[MAX_LINE];

    for(;;) {
        //main read loop.
        ssize_t n = recv(fd , chunk , sizeof(chunk) , 0 );
        if ( n <=  0 ) break; // disconnect or error

        //feed the bytes into the line buffer in pieces that fit, draining
        //complete lines in between. Appending the whole chunk at once would
        //wrongly reject valid input when the buffer holds the start of one
        //line and the chunk carries its end plus further pipelined commands.
        size_t off = 0;
        while (off < (size_t)n) {
            size_t space = linebuf_space(&lb);
            //buffer full and still no '\n': a single line longer than
            //MAX_LINE -- protocol violation, drop the client.
            if (space == 0) goto disconnected;

            size_t take = (size_t)n - off;
            if (take > space) take = space;
            linebuf_append(&lb , chunk + off , take);
            off += take;

            while (linebuf_extract(&lb , line , sizeof(line)) == 1) {
                Command  cmd ;
                //turns the raw text line into a structured Command
                if (parse_command(line , &cmd) != 0 ) {
                    snprintf(reply , sizeof(reply) , "ERROR\n");
                }else {
                    dispatch(&cmd , reply , sizeof(reply));
                }
                //write the reply back to the client over the same socket.
                //if the client already disconnected, send() fails with EPIPE
                //instead of raising SIGPIPE (ignored below) -- just drop this client.
                if (send_all(fd , reply , strlen(reply)) == -1) {
                    goto disconnected;
                }
            }
        }
    }

disconnected:
    close(fd);
    return NULL;
}

static int make_listener(void ) {
    struct addrinfo hints , *res , *p;
    memset(&hints , 0 , sizeof(hints));
    hints.ai_family= AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int status = getaddrinfo(NULL , PORT , &hints , &res);

    if (status != 0 ) {fprintf(stderr , "getaddrinfo: %s\n" , gai_strerror(status)); exit(1); }

    int listenfd = -1;
    for (p = res; p ; p = p->ai_next){
        listenfd = socket(p->ai_family , p->ai_socktype , p->ai_protocol);
        if(listenfd == -1 ) continue;
        int  yes = 1;

        setsockopt(listenfd , SOL_SOCKET , SO_REUSEADDR , &yes , sizeof(yes));
        if(bind(listenfd , p->ai_addr , p->ai_addrlen) == 0)break;
        close(listenfd);
        listenfd = -1;
    }

    freeaddrinfo(res);
    if (listenfd == -1) {fprintf(stderr , "failed to bind\n"); exit(1);}
    if (listen(listenfd , BACKLOG) == -1) {perror("listen"); exit(1);}
    return listenfd;
}

int main () {
    //a client disconnecting mid-response makes send() fail with EPIPE
    //rather than raising SIGPIPE, whose default action would kill the
    //whole process (every other connected client) over one bad client.
    signal(SIGPIPE , SIG_IGN);

    map = hashmap_create(1024);
    if (!map) { fprintf(stderr , "out of memory\n"); return 1; }
    if (replay_log(WAL_PATH , map) != 0) {
        fprintf(stderr , "out of memory while replaying %s\n" , WAL_PATH);
        return 1;
    }
    wal_file = wal_open(WAL_PATH);

    int listenfd = make_listener();
    printf("listening on port %s\n" , PORT);

    for (;;) {
        int clientfd = accept(listenfd , NULL , NULL);
        if (clientfd == -1) { perror("accept"); continue; }

        //pass the fd by value inside the void* argument: no heap allocation,
        //so nothing to leak if thread creation fails
        pthread_t tid;
        int err = pthread_create(&tid , NULL , handle_client , (void *)(intptr_t)clientfd);
        if (err != 0) {
            //e.g. EAGAIN when the thread limit is hit: drop this client,
            //keep serving the others
            fprintf(stderr , "pthread_create: %s\n" , strerror(err));
            close(clientfd);
            continue;
        }
        pthread_detach(tid);
    }
}
