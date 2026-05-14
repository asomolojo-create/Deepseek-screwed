#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define FIFO_BASE "/tmp/animate_fifos"
#define WELL_KNOWN_FIFO FIFO_BASE "/server_fifo"
#define MAX_CMD 1024
#define MAX_RESP 1024

int main() {
    char my_fifo[256];
    snprintf(my_fifo, sizeof(my_fifo), "%s/client_%d", FIFO_BASE, getpid());
    
    unlink(my_fifo);
    mkfifo(my_fifo, 0666);
    
    printf("Animation Client (PID: %d)\n", getpid());
    printf("Commands: Login <user>, create_rectangle <w> <h> <color> <filled>\n");
    printf("          create_canvas <h> <w> <bg>, place_sprite <c> <s> <x> <y>\n");
    printf("Type 'quit' to exit\n\n");
    
    char input[MAX_CMD];
    char response[MAX_RESP];
    
    while (1) {
        printf("> ");
        fflush(stdout);
        
        if (fgets(input, sizeof(input), stdin) == NULL) break;
        input[strcspn(input, "\n")] = 0;
        
        if (strcmp(input, "quit") == 0) break;
        
        // Send: PID:FIFO_NAME:COMMAND
        char message[MAX_CMD + 512];
        snprintf(message, sizeof(message), "%d:%s:%s", getpid(), my_fifo, input);
        
        int server_fd = open(WELL_KNOWN_FIFO, O_WRONLY);
        if (server_fd == -1) {
            printf("Server not running\n");
            continue;
        }
        write(server_fd, message, strlen(message));
        write(server_fd, "\n", 1);
        close(server_fd);
        
        // Read response
        int resp_fd = open(my_fifo, O_RDONLY);
        if (resp_fd == -1) {
            printf("Failed to read response\n");
            continue;
        }
        int n = read(resp_fd, response, sizeof(response) - 1);
        if (n > 0) {
            response[n] = '\0';
            printf("%s", response);
        }
        close(resp_fd);
    }
    
    unlink(my_fifo);
    return 0;
}
