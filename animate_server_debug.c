#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <pthread.h>

#define FIFO_BASE "/tmp/animate_fifos"
#define WELL_KNOWN_FIFO FIFO_BASE "/server_fifo"

int main() {
    printf("Step 1: Starting server\n");
    printf("Server PID: %d\n", getpid());
    fflush(stdout);
    
    printf("Step 2: Creating directory\n");
    mkdir(FIFO_BASE, 0755);
    
    printf("Step 3: Unlinking old FIFO\n");
    unlink(WELL_KNOWN_FIFO);
    
    printf("Step 4: Creating FIFO\n");
    if (mkfifo(WELL_KNOWN_FIFO, 0666) == -1) {
        perror("mkfifo failed");
        return 1;
    }
    
    printf("Step 5: Server ready, waiting for connections...\n");
    fflush(stdout);
    
    while (1) {
        printf("Step 6: Opening FIFO\n");
        int fd = open(WELL_KNOWN_FIFO, O_RDONLY);
        if (fd == -1) {
            perror("open failed");
            continue;
        }
        
        char buffer[256];
        int n = read(fd, buffer, sizeof(buffer)-1);
        close(fd);
        
        if (n > 0) {
            buffer[n] = '\0';
            printf("Received: %s\n", buffer);
        }
    }
    
    return 0;
}
