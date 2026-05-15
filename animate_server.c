#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
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
    // Must print this exactly and flush
    printf("Server PID: %d\n", getpid());
    fflush(stdout);
    
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    mkdir(FIFO_BASE, 0755);
    unlink(WELL_KNOWN_FIFO);
    if (mkfifo(WELL_KNOWN_FIFO, 0666) == -1) {
        perror("mkfifo");
        return 1;
    }
    
    fprintf(stderr, "Server ready\n");  // stderr is fine for debug
    
    while (running) {
        int fd = open(WELL_KNOWN_FIFO, O_RDONLY);
        if (fd == -1) continue;
        
        char buf[1024];
        int n = read(fd, buf, sizeof(buf)-1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';
        
        char *pid_str = strtok(buf, ":");
        char *fifo_name = strtok(NULL, ":");
        char *cmd = strtok(NULL, "\n");
        if (!pid_str || !fifo_name || !cmd) continue;
        
        int client_pid = atoi(pid_str);
        client_session_t client = {0};
        client.client_pid = client_pid;
        client.logged_in = 0;
        
        char response[MAX_RESP];
        handle_rpc(&client, cmd, response, sizeof(response));
        
        int resp_fd = open(fifo_name, O_WRONLY);
        if (resp_fd != -1) {
            write(resp_fd, response, strlen(response));
            close(resp_fd);
        }
    }
    
    unlink(WELL_KNOWN_FIFO);
    return 0;
}
