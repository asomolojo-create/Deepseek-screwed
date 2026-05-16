#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <pthread.h>
#include "rpc_handler.h"

#define FIFO_BASE "/tmp/animate_fifos"
#define WELL_KNOWN_FIFO FIFO_BASE "/server_fifo"
#define MAX_CMD 1024
#define MAX_RESP 1024

static volatile int running = 1;

void handle_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) running = 0;
}

int main() {
    printf("Server PID: %d\n", getpid());
    fflush(stdout);
    
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    mkdir(FIFO_BASE, 0755);
    unlink(WELL_KNOWN_FIFO);
    mkfifo(WELL_KNOWN_FIFO, 0666);
    
    printf("Animation Server running on %s\n", WELL_KNOWN_FIFO);
    fflush(stdout);
    
    while (running) {
        int server_fd = open(WELL_KNOWN_FIFO, O_RDONLY);
        if (server_fd == -1) continue;
        
        char buffer[512];
        int n = read(server_fd, buffer, sizeof(buffer) - 1);
        close(server_fd);
        
        if (n > 0) {
            buffer[n] = '\0';
            buffer[strcspn(buffer, "\n")] = '\0';
            
            char* pid_str = strtok(buffer, ":");
            char* fifo_name = strtok(NULL, ":");
            char* command = strtok(NULL, "");
            
            if (pid_str && fifo_name && command) {
                int client_pid = atoi(pid_str);
                
                // Create a simple session
                client_session_t client = {0};
                client.client_pid = client_pid;
                client.logged_in = 0;
                
                char response[MAX_RESP];
                handle_rpc(&client, command, response, MAX_RESP);
                
                // Send response
                int resp_fd = open(fifo_name, O_WRONLY);
                if (resp_fd != -1) {
                    write(resp_fd, response, strlen(response));
                    close(resp_fd);
                }
            }
        }
    }
    
    unlink(WELL_KNOWN_FIFO);
    return 0;
}
