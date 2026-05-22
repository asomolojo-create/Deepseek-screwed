#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#define MAX_CMD 2048
#define MAX_RESP 4096

static int server_pid = 0;
static int sigusr2_received = 0;
static char fifo_c2s[256];
static char fifo_s2c[256];

void sigusr2_handler(int sig) {
    (void)sig;
    sigusr2_received = 1;
    fprintf(stderr, "Received SIGUSR2\n");
    fflush(stderr);
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server_pid>\n", argv[0]);
        return 1;
    }

    server_pid = atoi(argv[1]);
    if (server_pid <= 0) {
        fprintf(stderr, "Invalid server PID\n");
        return 1;
    }

    int client_pid = getpid();

    signal(SIGUSR2, sigusr2_handler);

    if (kill(server_pid, SIGUSR1) == -1) {
        perror("kill SIGUSR1");
        return 1;
    }
    fprintf(stderr, "Sent SIGUSR1 to server %d\n", server_pid);
    fflush(stderr);

    time_t start = time(NULL);
    while (!sigusr2_received) {
        if (time(NULL) - start > 1) {
            fprintf(stderr, "Timeout waiting for server response\n");
            return 1;
        }
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
    }

    snprintf(fifo_c2s, sizeof(fifo_c2s), "/tmp/FIFO_C2S_%d", client_pid);
    snprintf(fifo_s2c, sizeof(fifo_s2c), "/tmp/FIFO_S2C_%d", client_pid);

    fprintf(stderr, "Opening C2S FIFO %s\n", fifo_c2s);
    fflush(stderr);
    int c2s_fd = open(fifo_c2s, O_WRONLY);
    if (c2s_fd == -1) {
        perror("open C2S FIFO");
        return 1;
    }
    fprintf(stderr, "Opened C2S FIFO %s\n", fifo_c2s);
    fflush(stderr);

    fprintf(stderr, "Opening S2C FIFO %s\n", fifo_s2c);
    fflush(stderr);
    int s2c_fd = open(fifo_s2c, O_RDONLY);
    if (s2c_fd == -1) {
        perror("open S2C FIFO");
        close(c2s_fd);
        return 1;
    }
    fprintf(stderr, "Opened S2C FIFO %s\n", fifo_s2c);
    fflush(stderr);

    char input[MAX_CMD];
    int logged_in = 0;

    while (fgets(input, sizeof(input), stdin)) {
        input[strcspn(input, "\n")] = '\0';

        if (strlen(input) == 0) continue;
        if (strcasecmp(input, "quit") == 0) break;

        if (!logged_in && strncasecmp(input, "Login", 5) != 0) {
            printf("Not logged in\n");
            continue;
        }

        fprintf(stderr, "Writing request: '%s'\n", input);
        fflush(stderr);
        if (write(c2s_fd, input, strlen(input)) < 0) {
            perror("write to C2S FIFO");
            break;
        }
        if (write(c2s_fd, "\n", 1) < 0) {
            perror("write newline to C2S FIFO");
            break;
        }
        fprintf(stderr, "Wrote request\n");
        fflush(stderr);

        char resp[MAX_RESP];
        int n;
        int read_error = 0;
        while (1) {
            n = read(s2c_fd, resp, sizeof(resp) - 1);
            if (n > 0) break;
            if (n == 0) {
                fprintf(stderr, "S2C EOF, server downstream closed\n");
                fflush(stderr);
                read_error = 1;
                break;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                usleep(10000);
                continue;
            }
            perror("read from S2C FIFO");
            read_error = 1;
            break;
        }
        if (read_error) break;
        resp[n] = '\0';

        if (strncmp(resp, "Reject", 6) == 0) {
            printf("%s", resp);
        } else {
            long code = strtol(resp, NULL, 10);
            char* val_start = strchr(resp, ' ');

            if (code == 0 && val_start) {
                printf("Success %s", val_start + 1);
            } else if (code == -1) {
                printf("RPC Failed\n");
            } else if (code == -2) {
                printf("Value error\n");
            } else if (code == -3) {
                printf("Internal error\n");
            } else {
                printf("%s\n", resp);
            }

            if (strncasecmp(input, "Login", 5) == 0) {
                if (code >= 0) {
                    logged_in = 1;
                    char username[33];
                    if (sscanf(input, "Login %32s", username) == 1) {
                        printf("Welcome %s. Your balance is %s", username, resp);
                    }
                }
            } else if (strcasecmp(input, "Disconnect") == 0 || strcasecmp(input, "disconnect") == 0) {
                if (code == 0) {
                    logged_in = 0;
                    break;
                }
            }
        }
    }

    close(c2s_fd);
    close(s2c_fd);

    unlink(fifo_c2s);
    unlink(fifo_s2c);

    return 0;
}
