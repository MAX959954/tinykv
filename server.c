#define _POSIX_C_SOURCE 200112L
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

//Without a lock, two threads doing e.g. SET foo bar concurrently
// could interleave their reads/writes inside hashmap and corrupt
// it — classic data race, undefined behavior,
static pthread_mutex_t g_lock  = PTHREAD_MUTEX_INITIALIZER; //intializes it in compile time 


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
        //stores the key/value in memory.
        hashmap_set(map, cmd->key, cmd->value);
        //writes it to disk
        wal_write(wal_file , record);
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
        //removes the key, returning 0 on success → found
        int found = (hashmap_del(map , cmd->key)== 0 );
        if (found) {
            char record[WAL_MAX_RECORD];
            snprintf(record , sizeof(record) , "DEL %s\n" , cmd->key);
            wal_write(wal_file , record);
        }
        //safe, bounded string formatting; the formatted line itself 
        //the durability record so a crash doesn't undo the delete
        snprintf(reply, reply_size, found ? "OK\n" : "NOT_FOUND\n");
        break;
    }
    default:
      snprintf(reply , reply_size , "ERROR\n");
    }

    //Unlock the mutex , so the next thread can get in
    pthread_mutex_unlock(&g_lock);
}

static void * handle_client(void *arg) {
    //expected to point to the client socket fd that 
    //was heap-allocated by the caller (so each thread
    // gets its own copy, not a shared variable)
    int fd = *(int*)arg;
    free(arg);

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
                if (send(fd , reply , strlen(reply) , 0 ) == -1) {
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
    replay_log(WAL_PATH , map);
    wal_file = wal_open(WAL_PATH);

    int listenfd = make_listener();
    printf("listening on port %s\n" , PORT);

    for (;;) {
        int *clientfd = (int*)malloc(sizeof(int));
        *clientfd = accept(listenfd , NULL , NULL);
        if (*clientfd == -1) { perror("accept" ); free(clientfd ); continue; }

        pthread_t tid;
        pthread_create(&tid , NULL , handle_client , clientfd);
        pthread_detach(tid);
    }
}
