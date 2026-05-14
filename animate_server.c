#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <pthread.h>
#include "rpc_handler.h"
#include "threadpool.h"

#define FIFO_BASE "/tmp/animate_fifos"
#define WELL_KNOWN_FIFO FIFO_BASE "/server_fifo"
#define MAX_CMD 1024
#define MAX_RESP 1024

static volatile int running = 1;
static threadpool_t* pool = NULL;

// Track client sessions by PID
static client_session_t* sessions = NULL;
static pthread_mutex_t sessions_mutex = PTHREAD_MUTEX_INITIALIZER;

void handle_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) running = 0;
}

client_session_t* find_or_create_session(int pid) {
    pthread_mutex_lock(&sessions_mutex);
    
    client_session_t* curr = sessions;
    while (curr) {
        if (curr->client_pid == pid) {
            printf("Found existing session for PID %d\n", pid);
            pthread_mutex_unlock(&sessions_mutex);
            return curr;
        }
        curr = curr->next;
    }
    
    // Create new session
    printf("Creating new session for PID %d\n", pid);
    client_session_t* new_session = calloc(1, sizeof(client_session_t));
    new_session->client_pid = pid;
    new_session->logged_in = 0;
    new_session->next = sessions;
    sessions = new_session;
    
    pthread_mutex_unlock(&sessions_mutex);
    return new_session;
}

typedef struct client_request {
    char client_fifo[256];
    char command[MAX_CMD];
    char response[MAX_RESP];
    int client_pid;
} client_request_t;

void process_request(void* arg) {
    client_request_t* req = (client_request_t*)arg;
    
    // Find or create session for this client
    client_session_t* client = find_or_create_session(req->client_pid);
    
    printf("Processing command for PID %d: %s\n", req->client_pid, req->command);
    
    handle_rpc(client, req->command, req->response, MAX_RESP);
    
    // Send response back to client
    int resp_fd = open(req->client_fifo, O_WRONLY);
    if (resp_fd != -1) {
        write(resp_fd, req->response, strlen(req->response));
        close(resp_fd);
    }
    
    free(req);
}

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    // Clean up old FIFOs
    system("rm -rf /tmp/animate_fifos");
    mkdir(FIFO_BASE, 0755);
    unlink(WELL_KNOWN_FIFO);
    mkfifo(WELL_KNOWN_FIFO, 0666);
    
    pool = threadpool_create(4);
    
    printf("Animation Server running on %s\n", WELL_KNOWN_FIFO);
    
    while (running) {
        int server_fd = open(WELL_KNOWN_FIFO, O_RDONLY);
        if (server_fd == -1) continue;
        
        char buffer[512];
        int n = read(server_fd, buffer, sizeof(buffer) - 1);
        close(server_fd);
        
        if (n > 0) {
            buffer[n] = '\0';
            buffer[strcspn(buffer, "\n")] = '\0';
            
            // Format: "PID:FIFO_NAME:COMMAND"
            char* pid_str = strtok(buffer, ":");
            char* fifo_name = strtok(NULL, ":");
            char* command = strtok(NULL, "");
            
            if (pid_str && fifo_name && command) {
                int client_pid = atoi(pid_str);
                
                client_request_t* req = malloc(sizeof(client_request_t));
                strcpy(req->client_fifo, fifo_name);
                strcpy(req->command, command);
                req->client_pid = client_pid;
                
                threadpool_add_task(pool, process_request, req);
            }
        }
    }
    
    threadpool_destroy(pool);
    unlink(WELL_KNOWN_FIFO);
    return 0;
}
