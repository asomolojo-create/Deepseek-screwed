#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <limits.h>

#include "rpc_handler.h"
#include "threadpool.h"

#define MAX_CMD 2048
#define MAX_RESP 4096
#define MAX_CLIENTS 256

static volatile int running = 1;
static threadpool_t* pool = NULL;
client_session_t* sessions = NULL;
int session_count = 0;
pthread_mutex_t sessions_mutex = PTHREAD_MUTEX_INITIALIZER;

static int signal_pipe[2] = {-1, -1};

typedef struct response_item {
    uint64_t sequence;
    char* response;
    struct response_item* next;
} response_item_t;

typedef struct {
    int client_pid;
    int c2s_fd;
    int s2c_fd;
    uint64_t next_request_seq;
    uint64_t next_response_seq;
    char pending_buffer[MAX_CMD];
    size_t pending_len;
    response_item_t* response_queue;
    bool c2s_eof;
    uint64_t disconnect_sequence;
} client_connection_t;

static client_connection_t* connections = NULL;
static int connection_count = 0;
static pthread_mutex_t connections_mutex = PTHREAD_MUTEX_INITIALIZER;

void sigterm_handler(int sig) {
    (void)sig;
    running = 0;
}

void sigusr1_handler(int sig, siginfo_t* info, void* context) {
    (void)sig;
    (void)context;
    if (!info || info->si_pid <= 0 || signal_pipe[1] < 0) return;
    pid_t client_pid = info->si_pid;
    ssize_t written = write(signal_pipe[1], &client_pid, sizeof(client_pid));
    (void)written;
}

static void handle_new_client(int client_pid) {
    char fifo_c2s[256];
    char fifo_s2c[256];
    snprintf(fifo_c2s, sizeof(fifo_c2s), "/tmp/FIFO_C2S_%d", client_pid);
    snprintf(fifo_s2c, sizeof(fifo_s2c), "/tmp/FIFO_S2C_%d", client_pid);


    unlink(fifo_c2s);
    unlink(fifo_s2c);

    if (mkfifo(fifo_c2s, 0666) == -1 && errno != EEXIST) {
        fprintf(stderr, "mkfifo failed %s errno=%d\n", fifo_c2s, errno);
        fflush(stderr);
        return;
    }
    if (mkfifo(fifo_s2c, 0666) == -1 && errno != EEXIST) {
        unlink(fifo_c2s);
        return;
    }

    pthread_mutex_lock(&sessions_mutex);
    client_session_t* client = NULL;
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].client_pid == client_pid) {
            client = &sessions[i];
            break;
        }
    }
    if (!client && session_count < MAX_CLIENTS) {
        client = &sessions[session_count++];
        client->client_pid = client_pid;
        client->logged_in = 0;
        client->balance = 0;
        client->username[0] = '\0';
        client->resources = NULL;
        client->resource_count = 0;
        client->resource_capacity = 0;
        client->next = NULL;
    }
    pthread_mutex_unlock(&sessions_mutex);

    pthread_mutex_lock(&connections_mutex);
    if (connection_count < MAX_CLIENTS) {
        client_connection_t* conn = &connections[connection_count];
        conn->client_pid = client_pid;
        conn->c2s_fd = -1;
        conn->s2c_fd = -1;
        conn->next_request_seq = 1;
        conn->next_response_seq = 1;
        conn->pending_len = 0;
        conn->response_queue = NULL;
        conn->c2s_eof = false;
        conn->disconnect_sequence = 0;
        connection_count++;
        fprintf(stderr, "New client registered %d, total connections=%d\n", client_pid, connection_count);
        fflush(stderr);
    }
    pthread_mutex_unlock(&connections_mutex);

    kill(client_pid, SIGUSR2);
}

typedef struct {
    int client_pid;
    uint64_t sequence;
    char command[MAX_CMD];
} client_request_t;

static bool send_pending_responses(int index) {
    client_connection_t* conn = &connections[index];
    bool remove_after_send = false;
    while (conn && conn->response_queue && conn->response_queue->sequence == conn->next_response_seq) {
        response_item_t* item = conn->response_queue;
        conn->response_queue = item->next;
        if (conn->s2c_fd >= 0) {
            int written = write(conn->s2c_fd, item->response, strlen(item->response));
            if (written < 0) {
                fprintf(stderr, "Failed to send response to client %d: %s\n", conn->client_pid, strerror(errno));
                fflush(stderr);
                remove_after_send = true;
            } else {
                fprintf(stderr, "Sent response to client %d: '%s'\n", conn->client_pid, item->response);
                fflush(stderr);
            }
        }
        if (item->sequence == conn->disconnect_sequence) {
            remove_after_send = true;
        }
        conn->next_response_seq++;
        free(item->response);
        free(item);
        if (remove_after_send) break;
    }
    return remove_after_send;
}

static void remove_connection(int index);

static void store_response(int client_pid, uint64_t sequence, const char* response, bool disconnect) {
    pthread_mutex_lock(&connections_mutex);
    for (int i = 0; i < connection_count; i++) {
        if (connections[i].client_pid == client_pid) {
            client_connection_t* conn = &connections[i];
            response_item_t* item = malloc(sizeof(response_item_t));
            if (!item) {
                pthread_mutex_unlock(&connections_mutex);
                return;
            }
            item->sequence = sequence;
            item->response = strdup(response);
            if (!item->response) {
                free(item);
                pthread_mutex_unlock(&connections_mutex);
                return;
            }
            item->next = NULL;
            if (disconnect) {
                conn->disconnect_sequence = sequence;
            }
            response_item_t** cur = &conn->response_queue;
            while (*cur && (*cur)->sequence < item->sequence) cur = &(*cur)->next;
            item->next = *cur;
            *cur = item;
            bool removed = send_pending_responses(i);
            if (removed) {
                remove_connection(i);
            }
            break;
        }
    }
    pthread_mutex_unlock(&connections_mutex);
}

void process_request(void* arg) {
    client_request_t* req = (client_request_t*)arg;

    pthread_mutex_lock(&sessions_mutex);
    client_session_t* client = NULL;
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].client_pid == req->client_pid) {
            client = &sessions[i];
            break;
        }
    }
    pthread_mutex_unlock(&sessions_mutex);

    char response[MAX_RESP];
    if (!client) {
        snprintf(response, MAX_RESP, "-1\n");
    } else {
        handle_rpc(client, req->command, response, MAX_RESP);
    }

    bool disconnect = strcasecmp(req->command, "Disconnect") == 0 || strcasecmp(req->command, "disconnect") == 0;
    store_response(req->client_pid, req->sequence, response, disconnect);
    free(req);
}

static void remove_connection(int index) {
    if (index < 0 || index >= connection_count) return;

    client_connection_t* conn = &connections[index];
    if (conn->c2s_fd >= 0) close(conn->c2s_fd);
    if (conn->s2c_fd >= 0) close(conn->s2c_fd);

    char fifo_c2s[256];
    char fifo_s2c[256];
    snprintf(fifo_c2s, sizeof(fifo_c2s), "/tmp/FIFO_C2S_%d", conn->client_pid);
    snprintf(fifo_s2c, sizeof(fifo_s2c), "/tmp/FIFO_S2C_%d", conn->client_pid);
    unlink(fifo_c2s);
    unlink(fifo_s2c);

    response_item_t* item = conn->response_queue;
    while (item) {
        response_item_t* next = item->next;
        free(item->response);
        free(item);
        item = next;
    }
    conn->response_queue = NULL;

    if (index != connection_count - 1) {
        connections[index] = connections[connection_count - 1];
    }
    connection_count--;

    pthread_mutex_lock(&sessions_mutex);
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].client_pid == conn->client_pid) {
            cleanup_client_resources(&sessions[i]);
            if (i != session_count - 1) {
                sessions[i] = sessions[session_count - 1];
            }
            session_count--;
            break;
        }
    }
    pthread_mutex_unlock(&sessions_mutex);
    prune_shared_canvases();
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <threadpool_size>\n", argv[0]);
        return 1;
    }

    int threadpool_size = atoi(argv[1]);
    if (threadpool_size < 1) {
        fprintf(stderr, "Threadpool size must be at least 1\n");
        return 1;
    }

    char exe_path[PATH_MAX];
    ssize_t exe_len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (exe_len > 0) {
        exe_path[exe_len] = '\0';
        char* last_slash = strrchr(exe_path, '/');
        if (last_slash) {
            *last_slash = '\0';
            init_users_file_path(exe_path);
        }
    }

    printf("Server PID: %d\n", getpid());
    fflush(stdout);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = sigusr1_handler;
    if (sigaction(SIGUSR1, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }
    signal(SIGINT, sigterm_handler);
    signal(SIGTERM, sigterm_handler);

    pool = threadpool_create(threadpool_size);
    if (!pool) {
        fprintf(stderr, "Failed to create threadpool\n");
        return 1;
    }

    sessions = calloc(MAX_CLIENTS, sizeof(client_session_t));
    if (!sessions) {
        fprintf(stderr, "Failed to allocate sessions\n");
        threadpool_destroy(pool);
        return 1;
    }

    connections = calloc(MAX_CLIENTS, sizeof(client_connection_t));
    if (!connections) {
        fprintf(stderr, "Failed to allocate connections\n");
        free(sessions);
        threadpool_destroy(pool);
        return 1;
    }

    if (pipe(signal_pipe) == -1) {
        perror("pipe");
        free(connections);
        free(sessions);
        threadpool_destroy(pool);
        return 1;
    }
    int flags = fcntl(signal_pipe[0], F_GETFL, 0);
    if (flags != -1) fcntl(signal_pipe[0], F_SETFL, flags | O_NONBLOCK);

    fprintf(stderr, "Server ready\n");
    fflush(stderr);

    while (running) {
        while (1) {
            pid_t client_pid;
            ssize_t n = read(signal_pipe[0], &client_pid, sizeof(client_pid));
            if (n == sizeof(client_pid)) {
                handle_new_client(client_pid);
                continue;
            }
            if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                break;
            }
            break;
        }

        pthread_mutex_lock(&connections_mutex);
        for (int i = 0; i < connection_count; i++) {
            client_connection_t* conn = &connections[i];

            if (conn->c2s_fd < 0 && !conn->c2s_eof) {
                char fifo_c2s[256];
                snprintf(fifo_c2s, sizeof(fifo_c2s), "/tmp/FIFO_C2S_%d", conn->client_pid);
                int fd = open(fifo_c2s, O_RDONLY | O_NONBLOCK);
                if (fd >= 0) {
                    conn->c2s_fd = fd;
                    fprintf(stderr, "Opened C2S fd=%d for client %d\n", fd, conn->client_pid);
                    fflush(stderr);
                }
            }

            if (conn->s2c_fd < 0) {
                char fifo_s2c[256];
                snprintf(fifo_s2c, sizeof(fifo_s2c), "/tmp/FIFO_S2C_%d", conn->client_pid);
                int fd = open(fifo_s2c, O_WRONLY | O_NONBLOCK);
                if (fd >= 0) {
                    conn->s2c_fd = fd;
                    fprintf(stderr, "Opened S2C fd=%d for client %d\n", fd, conn->client_pid);
                    fflush(stderr);
                }
            }

            if (conn->c2s_fd >= 0 && conn->s2c_fd >= 0) {
                if (kill(conn->client_pid, 0) == -1 && errno == ESRCH) {
                    fprintf(stderr, "Client process %d no longer exists, removing connection\n", conn->client_pid);
                    fflush(stderr);
                    remove_connection(i);
                    i--;
                    continue;
                }

                char buffer[MAX_CMD];
                int n = read(conn->c2s_fd, buffer, sizeof(buffer) - 1);
                if (n > 0) {
                    if ((size_t)n + conn->pending_len >= sizeof(conn->pending_buffer)) {
                        fprintf(stderr, "Input buffer overflow for client %d, discarding data\n", conn->client_pid);
                        fflush(stderr);
                        conn->pending_len = 0;
                        continue;
                    }
                    memcpy(conn->pending_buffer + conn->pending_len, buffer, (size_t)n);
                    conn->pending_len += (size_t)n;
                    conn->pending_buffer[conn->pending_len] = '\0';
                    while (true) {
                        char* newline = strchr(conn->pending_buffer, '\n');
                        if (!newline) break;
                        *newline = '\0';
                        if (conn->pending_buffer[0] != '\0') {
                            fprintf(stderr, "Read from client %d: '%s'\n", conn->client_pid, conn->pending_buffer);
                            fflush(stderr);
                            client_request_t* req = malloc(sizeof(client_request_t));
                            if (req) {
                                req->client_pid = conn->client_pid;
                                req->sequence = conn->next_request_seq++;
                                strncpy(req->command, conn->pending_buffer, sizeof(req->command) - 1);
                                req->command[sizeof(req->command) - 1] = '\0';
                                if (threadpool_add_task(pool, process_request, req) != 0) {
                                    free(req);
                                }
                            }
                        }
                        size_t consumed = (newline - conn->pending_buffer) + 1;
                        memmove(conn->pending_buffer, conn->pending_buffer + consumed, conn->pending_len - consumed);
                        conn->pending_len -= consumed;
                        conn->pending_buffer[conn->pending_len] = '\0';
                    }
                } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    fprintf(stderr, "Read error on client %d c2s_fd=%d: %s\n", conn->client_pid, conn->c2s_fd, strerror(errno));
                    fflush(stderr);
                    remove_connection(i);
                    i--;
                    continue;
                } else if (n == 0) {
                    fprintf(stderr, "Client %d closed C2S connection, preserving pending responses\n", conn->client_pid);
                    fflush(stderr);
                    conn->c2s_eof = true;
                    close(conn->c2s_fd);
                    conn->c2s_fd = -1;
                    conn->pending_len = 0;
                    continue;
                }
            }
        }
        pthread_mutex_unlock(&connections_mutex);
        {
            struct timespec ts_sleep = {0, 10000L * 1000L};
            nanosleep(&ts_sleep, NULL);
        }
    }

    pthread_mutex_lock(&connections_mutex);
    while (connection_count > 0) {
        remove_connection(connection_count - 1);
    }
    pthread_mutex_unlock(&connections_mutex);

    pthread_mutex_lock(&sessions_mutex);
    while (session_count > 0) {
        cleanup_client_resources(&sessions[--session_count]);
    }
    pthread_mutex_unlock(&sessions_mutex);
    free(sessions);
    free(connections);
    threadpool_destroy(pool);
    return 0;
}
