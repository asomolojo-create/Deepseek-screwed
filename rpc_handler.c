#include "rpc_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

int check_login(const char* username, int* balance) {
    FILE* fp = fopen("users.txt", "r");
    if (!fp) return 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char user[33];
        int bal;
        if (sscanf(line, "%32s %d", user, &bal) == 2) {
            if (strcmp(username, user) == 0) {
                *balance = bal;
                fclose(fp);
                return (bal > 0) ? 1 : 0;
            }
        }
    }
    fclose(fp);
    return 0;
}

static int parse_args(const char* cmd, char* tokens[], int max_tokens) {
    char buffer[512];
    strncpy(buffer, cmd, sizeof(buffer)-1);
    buffer[sizeof(buffer)-1] = '\0';
    int count = 0;
    char* token = strtok(buffer, " \t\n");
    while (token && count < max_tokens) {
        tokens[count++] = strdup(token);
        token = strtok(NULL, " \t\n");
    }
    return count;
}

static void free_tokens(char* tokens[], int count) {
    for (int i = 0; i < count; i++) free(tokens[i]);
}

static void cmd_login(client_session_t* client, char* tokens[], int argc, char* response) {
    if (argc != 2) { snprintf(response, 256, "%d\n", RPC_VALUE_ERROR); return; }
    int balance;
    if (check_login(tokens[1], &balance)) {
        client->logged_in = 1;
        strcpy(client->username, tokens[1]);
        client->balance = balance;
        snprintf(response, 256, "0\n");
    } else {
        snprintf(response, 256, "-1\n");
    }
}

static void cmd_create_rectangle(client_session_t* client, char* tokens[], int argc, char* response) {
    if (!client->logged_in) { snprintf(response, 256, "Not logged in\n"); return; }
    if (argc != 5) { snprintf(response, 256, "%d\n", RPC_VALUE_ERROR); return; }
    size_t w = atoi(tokens[1]), h = atoi(tokens[2]);
    color_t c = (color_t)strtoul(tokens[3], NULL, 16);
    struct sprite* s = animate_create_rectangle(w, h, c, atoi(tokens[4]) != 0);
    if (!s) { snprintf(response, 256, "%d\n", RPC_INTERNAL_ERROR); return; }
    snprintf(response, 256, "%d %lu\n", RPC_SUCCESS, (unsigned long)s);
}

static void cmd_create_circle(client_session_t* client, char* tokens[], int argc, char* response) {
    if (!client->logged_in) { snprintf(response, 256, "Not logged in\n"); return; }
    if (argc < 3) { snprintf(response, 256, "%d\n", RPC_VALUE_ERROR); return; }
    size_t r = atoi(tokens[1]);
    color_t c = (color_t)strtoul(tokens[2], NULL, 16);
    bool filled = (argc > 3) ? atoi(tokens[3]) : 1;
    struct sprite* s = animate_create_circle(r, c, filled);
    if (!s) { snprintf(response, 256, "%d\n", RPC_INTERNAL_ERROR); return; }
    snprintf(response, 256, "%d %lu\n", RPC_SUCCESS, (unsigned long)s);
}

static void cmd_create_sprite(client_session_t* client, char* tokens[], int argc, char* response) {
    if (!client->logged_in) { snprintf(response, 256, "Not logged in\n"); return; }
    if (argc != 2) { snprintf(response, 256, "%d\n", RPC_VALUE_ERROR); return; }
    struct sprite* s = animate_create_sprite(tokens[1]);
    if (!s) { snprintf(response, 256, "%d\n", RPC_INTERNAL_ERROR); return; }
    snprintf(response, 256, "%d %lu\n", RPC_SUCCESS, (unsigned long)s);
}

static void cmd_create_canvas(client_session_t* client, char* tokens[], int argc, char* response) {
    if (!client->logged_in) { snprintf(response, 256, "Not logged in\n"); return; }
    if (argc != 4) { snprintf(response, 256, "%d\n", RPC_VALUE_ERROR); return; }
    size_t h = atoi(tokens[1]), w = atoi(tokens[2]);
    color_t bg = (color_t)strtoul(tokens[3], NULL, 16);
    struct canvas* c = animate_create_canvas(h, w, bg);
    if (!c) { snprintf(response, 256, "%d\n", RPC_INTERNAL_ERROR); return; }
    snprintf(response, 256, "%d %lu\n", RPC_SUCCESS, (unsigned long)c);
}

static void cmd_place_sprite(client_session_t* client, char* tokens[], int argc, char* response) {
    if (!client->logged_in) { snprintf(response, 256, "Not logged in\n"); return; }
    if (argc != 5) { snprintf(response, 256, "%d\n", RPC_VALUE_ERROR); return; }
    struct canvas* c = (struct canvas*)strtoul(tokens[1], NULL, 10);
    struct sprite* s = (struct sprite*)strtoul(tokens[2], NULL, 10);
    struct sprite_placement* p = animate_place_sprite(c, s, atol(tokens[3]), atol(tokens[4]));
    if (!p) { snprintf(response, 256, "%d\n", RPC_INTERNAL_ERROR); return; }
    snprintf(response, 256, "%d %lu\n", RPC_SUCCESS, (unsigned long)p);
}

static void cmd_placement_up(client_session_t* client, char* tokens[], int argc, char* response) {
    (void)argc;
    if (!client->logged_in) { snprintf(response, 256, "Not logged in\n"); return; }
    animate_placement_up((struct sprite_placement*)strtoul(tokens[1], NULL, 10));
    snprintf(response, 256, "%d\n", RPC_SUCCESS);
}

static void cmd_placement_down(client_session_t* client, char* tokens[], int argc, char* response) {
    (void)argc;
    if (!client->logged_in) { snprintf(response, 256, "Not logged in\n"); return; }
    animate_placement_down((struct sprite_placement*)strtoul(tokens[1], NULL, 10));
    snprintf(response, 256, "%d\n", RPC_SUCCESS);
}

static void cmd_placement_top(client_session_t* client, char* tokens[], int argc, char* response) {
    (void)argc;
    if (!client->logged_in) { snprintf(response, 256, "Not logged in\n"); return; }
    animate_placement_top((struct sprite_placement*)strtoul(tokens[1], NULL, 10));
    snprintf(response, 256, "%d\n", RPC_SUCCESS);
}

static void cmd_placement_bottom(client_session_t* client, char* tokens[], int argc, char* response) {
    (void)argc;
    if (!client->logged_in) { snprintf(response, 256, "Not logged in\n"); return; }
    animate_placement_bottom((struct sprite_placement*)strtoul(tokens[1], NULL, 10));
    snprintf(response, 256, "%d\n", RPC_SUCCESS);
}

static void cmd_destroy_placement(client_session_t* client, char* tokens[], int argc, char* response) {
    (void)argc;
    if (!client->logged_in) { snprintf(response, 256, "Not logged in\n"); return; }
    animate_destroy_placement((struct sprite_placement*)strtoul(tokens[1], NULL, 10));
    snprintf(response, 256, "%d\n", RPC_SUCCESS);
}

static void cmd_destroy_sprite(client_session_t* client, char* tokens[], int argc, char* response) {
    (void)argc;
    if (!client->logged_in) { snprintf(response, 256, "Not logged in\n"); return; }
    animate_destroy_sprite((struct sprite*)strtoul(tokens[1], NULL, 10));
    snprintf(response, 256, "%d\n", RPC_SUCCESS);
}

static void cmd_destroy_canvas(client_session_t* client, char* tokens[], int argc, char* response) {
    (void)argc;
    if (!client->logged_in) { snprintf(response, 256, "Not logged in\n"); return; }
    animate_destroy_canvas((struct canvas*)strtoul(tokens[1], NULL, 10));
    snprintf(response, 256, "%d\n", RPC_SUCCESS);
}

static void cmd_set_animation_params(client_session_t* client, char* tokens[], int argc, char* response) {
    if (!client->logged_in) { snprintf(response, 256, "Not logged in\n"); return; }
    if (argc != 6) { snprintf(response, 256, "%d\n", RPC_VALUE_ERROR); return; }
    animate_set_animation_params((struct sprite_placement*)strtoul(tokens[1], NULL, 10),
                                  atol(tokens[2]), atol(tokens[3]), atol(tokens[4]), atol(tokens[5]));
    snprintf(response, 256, "%d\n", RPC_SUCCESS);
}

static void cmd_disconnect(client_session_t* client, char* tokens[], int argc, char* response) {
    (void)tokens;
    (void)argc;
    client->logged_in = 0;
    snprintf(response, 256, "%d\n", RPC_SUCCESS);
}

static void cmd_generate(client_session_t* client, char* tokens[], int argc, char* response) {
    if (!client->logged_in) { snprintf(response, 256, "Not logged in\n"); return; }
    if (argc != 6) { snprintf(response, 256, "%d\n", RPC_VALUE_ERROR); return; }
    struct canvas* c = (struct canvas*)strtoul(tokens[1], NULL, 10);
    const char* filename = tokens[2];
    int start = atoi(tokens[3]), end = atoi(tokens[4]), fps = atoi(tokens[5]);
    if (!c || start < 0 || end < start || fps <= 0) {
        snprintf(response, 256, "%d\n", RPC_VALUE_ERROR); return;
    }
    size_t w = animate_get_canvas_width(c);
    size_t h = animate_get_canvas_height(c);
    size_t frame_size = w * h * sizeof(color_t);
    void* buf = malloc(frame_size);
    if (!buf) { snprintf(response, 256, "%d\n", RPC_INTERNAL_ERROR); return; }
    char datfile[256], mp4file[256], logfile[256];
    snprintf(datfile, sizeof(datfile), "%s.dat", filename);
    snprintf(mp4file, sizeof(mp4file), "%s.mp4", filename);
    snprintf(logfile, sizeof(logfile), "%s.log", filename);
    FILE* dat = fopen(datfile, "wb");
    if (!dat) { free(buf); snprintf(response, 256, "0 -1\n"); return; }
    for (int f = start; f <= end; f++) {
        animate_generate_frame(c, f, fps, buf);
        fwrite(buf, 1, frame_size, dat);
    }
    fclose(dat);
    free(buf);
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "ffmpeg -f rawvideo -pixel_format rgba -video_size %zux%zu -framerate %d -i %s -c:v libx264 -pix_fmt yuv420p %s 2> %s",
             w, h, fps, datfile, mp4file, logfile);
    int ret = system(cmd);
    if (ret != 0) { snprintf(response, 256, "0 0 -1\n"); return; }
    snprintf(response, 256, "0 0 0\n");
    printf("Generated video: %s (%d frames)\n", mp4file, end - start + 1);
}

void handle_rpc(client_session_t* client, const char* command, char* response, size_t response_size) {
    char* tokens[16];
    int argc = parse_args(command, tokens, 16);
    if (argc == 0) { snprintf(response, response_size, "%d\n", RPC_FAILED); return; }
    
    if (strcmp(tokens[0], "Login") != 0 && strcmp(tokens[0], "Disconnect") != 0) {
        if (!client->logged_in) { snprintf(response, response_size, "Not logged in\n"); free_tokens(tokens, argc); return; }
    }
    
    if (strcmp(tokens[0], "create_rectangle") == 0) cmd_create_rectangle(client, tokens, argc, response);
    else if (strcmp(tokens[0], "create_circle") == 0) cmd_create_circle(client, tokens, argc, response);
    else if (strcmp(tokens[0], "create_sprite") == 0) cmd_create_sprite(client, tokens, argc, response);
    else if (strcmp(tokens[0], "create_canvas") == 0) cmd_create_canvas(client, tokens, argc, response);
    else if (strcmp(tokens[0], "place_sprite") == 0) cmd_place_sprite(client, tokens, argc, response);
    else if (strcmp(tokens[0], "placement_up") == 0) cmd_placement_up(client, tokens, argc, response);
    else if (strcmp(tokens[0], "placement_down") == 0) cmd_placement_down(client, tokens, argc, response);
    else if (strcmp(tokens[0], "placement_top") == 0) cmd_placement_top(client, tokens, argc, response);
    else if (strcmp(tokens[0], "placement_bottom") == 0) cmd_placement_bottom(client, tokens, argc, response);
    else if (strcmp(tokens[0], "destroy_placement") == 0) cmd_destroy_placement(client, tokens, argc, response);
    else if (strcmp(tokens[0], "destroy_sprite") == 0) cmd_destroy_sprite(client, tokens, argc, response);
    else if (strcmp(tokens[0], "destroy_canvas") == 0) cmd_destroy_canvas(client, tokens, argc, response);
    else if (strcmp(tokens[0], "set_animation_params") == 0) cmd_set_animation_params(client, tokens, argc, response);
    else if (strcmp(tokens[0], "generate") == 0) cmd_generate(client, tokens, argc, response);
    else if (strcmp(tokens[0], "Disconnect") == 0) cmd_disconnect(client, tokens, argc, response);
    else if (strcmp(tokens[0], "Login") == 0) cmd_login(client, tokens, argc, response);
    else snprintf(response, response_size, "%d\n", RPC_FAILED);
    
    free_tokens(tokens, argc);
}
