#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>

#define FIFO_BASE "/tmp/animate_fifos"
#define WELL_KNOWN_FIFO FIFO_BASE "/server_fifo"
#define MAX_CMD 1024
#define MAX_RESP 1024

int main() {
    char my_fifo[256];
    snprintf(my_fifo, sizeof(my_fifo), "%s/client_%d", FIFO_BASE, getpid());
    unlink(my_fifo);
    if (mkfifo(my_fifo, 0666) == -1) {
        fprintf(stderr, "mkfifo failed: %s\n", strerror(errno));
        return 1;
    }
    
    setbuf(stdout, NULL);
    printf("Client ready\n");
    
    // REQUIRED: Signal parent (marking harness) that client is ready
    if (getppid() > 1) {
        kill(getppid(), SIGUSR1);
    }
    
    char input[MAX_CMD];
    while (fgets(input, sizeof(input), stdin)) {
        input[strcspn(input, "\n")] = 0;
        if (strcmp(input, "quit") == 0) break;
        if (strlen(input) == 0) continue;
        
        if (access(WELL_KNOWN_FIFO, F_OK) == -1) {
            printf("Server not running\n");
            continue;
        }
        
        char msg[MAX_CMD+512];
        snprintf(msg, sizeof(msg), "%d:%s:%s", getpid(), my_fifo, input);
        
        int sfd = open(WELL_KNOWN_FIFO, O_WRONLY);
        if (sfd == -1) {
            printf("Server not running\n");
            continue;
        }
        write(sfd, msg, strlen(msg));
        write(sfd, "\n", 1);
        close(sfd);
        
        int rfd = open(my_fifo, O_RDONLY);
        if (rfd == -1) {
            perror("open response FIFO");
            continue;
        }
        char resp[MAX_RESP];
        int n = read(rfd, resp, sizeof(resp)-1);
        if (n > 0) {
            resp[n] = '\0';
            printf("%s", resp);
        } else if (n == 0) {
            printf("Server closed connection\n");
        } else {
            perror("read");
        }
        close(rfd);
    }
    unlink(my_fifo);
    return 0;
}
