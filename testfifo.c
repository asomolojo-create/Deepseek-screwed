#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#define FIFO_BASE "/tmp/animate_fifos"
int main() {
    char my_fifo[256];
    snprintf(my_fifo, sizeof(my_fifo), "%s/client_%d", FIFO_BASE, getpid());
    unlink(my_fifo);
    if (mkfifo(my_fifo, 0666) == -1) {
        perror("mkfifo");
        return 1;
    }
    printf("FIFO created: %s\n", my_fifo);
    unlink(my_fifo);
    return 0;
}
