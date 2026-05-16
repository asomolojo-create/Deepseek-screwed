#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#define FIFO_BASE "/tmp/animate_fifos"
#define WELL_KNOWN_FIFO FIFO_BASE "/server_fifo"

int main() {
    char my_fifo[256];
    snprintf(my_fifo, sizeof(my_fifo), "%s/client_%d", FIFO_BASE, getpid());
    unlink(my_fifo);
    if (mkfifo(my_fifo, 0666) == -1) {
        perror("mkfifo");
        return 1;
    }
    
    char msg[512];
    snprintf(msg, sizeof(msg), "%d:%s:%s", getpid(), my_fifo, "Login alice");
    
    int sfd = open(WELL_KNOWN_FIFO, O_WRONLY);
    if (sfd == -1) {
        perror("open server fifo");
        unlink(my_fifo);
        return 1;
    }
    write(sfd, msg, strlen(msg));
    write(sfd, "\n", 1);
    close(sfd);
    
    int rfd = open(my_fifo, O_RDONLY);
    if (rfd == -1) {
        perror("open response fifo");
        unlink(my_fifo);
        return 1;
    }
    char resp[256];
    int n = read(rfd, resp, sizeof(resp)-1);
    if (n > 0) {
        resp[n] = '\0';
        printf("Response: %s", resp);
    } else {
        printf("No response\n");
    }
    close(rfd);
    unlink(my_fifo);
    return 0;
}
