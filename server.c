#define _POSIX_C_SOURCE 200809L
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
#include "store.h"

#define DEFAULT_PORT "9999"
#define DEFAULT_WAL  "kv.log"
#define BACKLOG 128

static Store *store;

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

//dispatch() is the glue -- it hands a parsed command to the store and turns
//the result into a reply line. All locking and durability live in store.c.
static void dispatch(const Command *cmd , char *reply , size_t reply_size){
    StoreResult r;
    switch (cmd->type) {
    case CMD_SET:
        r = store_set(store , cmd->key , cmd->value);
        break;
    case CMD_DEL:
        r = store_del(store , cmd->key);
        break;
    case CMD_GET:
        //leave room for the trailing '\n'
        r = store_get(store , cmd->key , reply , reply_size - 1);
        if (r == STORE_OK) {
            strcat(reply , "\n");
            return;
        }
        break;
    default:
        r = STORE_ERROR;
    }
    const char *text = (r == STORE_OK) ? "OK\n"
                     : (r == STORE_NOT_FOUND) ? "NOT_FOUND\n"
                     : "ERROR\n";
    snprintf(reply , reply_size , "%s" , text);
}

static void * handle_client(void *arg) {
    //the fd is passed by value, packed into the pointer argument itself
    //(see main), so each thread has its own copy and nothing needs freeing
    int fd = (int)(intptr_t)arg;

    //a fresh per-connection line buffer, scoped to this thread only
    LineBuf lb;
    linebuf_init(&lb);
    //chunk is the raw recv scratch space, line holds one extracted
    //command line, reply holds the response text
    char chunk[512], line[MAX_LINE] , reply[MAX_LINE];

    for(;;) {
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
                if (parse_command(line , &cmd) != 0 ) {
                    snprintf(reply , sizeof(reply) , "ERROR\n");
                }else {
                    dispatch(&cmd , reply , sizeof(reply));
                }
                //if the client already disconnected, send() fails with EPIPE
                //instead of raising SIGPIPE (ignored in main) -- drop this client.
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

static int make_listener(const char *port) {
    struct addrinfo hints , *res , *p;
    memset(&hints , 0 , sizeof(hints));
    hints.ai_family= AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int status = getaddrinfo(NULL , port , &hints , &res);

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
    if (listenfd == -1) {fprintf(stderr , "failed to bind port %s\n" , port); exit(1);}
    if (listen(listenfd , BACKLOG) == -1) {perror("listen"); exit(1);}
    return listenfd;
}

static void usage(const char *prog) {
    fprintf(stderr ,
        "usage: %s [-p port] [-w wal_path] [-m group|serial|nosync]\n"
        "  -p  TCP port to listen on (default " DEFAULT_PORT ")\n"
        "  -w  write-ahead log file (default " DEFAULT_WAL ")\n"
        "  -m  group   rwlock + group commit (default)\n"
        "      serial  one global lock, fsync per write (old design, for benchmarks)\n"
        "      nosync  group mode without fsync -- NOT durable, benchmarks only\n" ,
        prog);
}

int main (int argc , char **argv) {
    const char *port = DEFAULT_PORT;
    const char *wal_path = DEFAULT_WAL;
    StoreMode mode = STORE_MODE_GROUP;

    int opt;
    while ((opt = getopt(argc , argv , "p:w:m:h")) != -1) {
        switch (opt) {
        case 'p': port = optarg; break;
        case 'w': wal_path = optarg; break;
        case 'm':
            if      (strcmp(optarg , "group")  == 0) mode = STORE_MODE_GROUP;
            else if (strcmp(optarg , "serial") == 0) mode = STORE_MODE_SERIAL;
            else if (strcmp(optarg , "nosync") == 0) mode = STORE_MODE_NOSYNC;
            else { usage(argv[0]); return 2; }
            break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }

    //a client disconnecting mid-response makes send() fail with EPIPE
    //rather than raising SIGPIPE, whose default action would kill the
    //whole process (every other connected client) over one bad client.
    signal(SIGPIPE , SIG_IGN);

    store = store_open(wal_path , mode);
    if (!store) return 1;

    int listenfd = make_listener(port);
    printf("listening on port %s (wal: %s, mode: %s)\n" , port , wal_path ,
           mode == STORE_MODE_GROUP ? "group" : mode == STORE_MODE_SERIAL ? "serial" : "nosync");
    fflush(stdout);

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
