#include "rpc_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <limits.h>

extern client_session_t* sessions;
extern int session_count;
extern pthread_mutex_t sessions_mutex;

static uint64_t handle_counter = 1;
static pthread_mutex_t handle_mutex = PTHREAD_MUTEX_INITIALIZER;

static char users_file_path[PATH_MAX] = "users.txt";

static resource_t** global_resources = NULL;
static int global_resource_count = 0;
static int global_resource_capacity = 0;
static pthread_mutex_t global_resources_mutex = PTHREAD_MUTEX_INITIALIZER;

static shared_canvas_t** shared_canvases = NULL;
static int shared_canvas_count = 0;
static int shared_canvas_capacity = 0;
static pthread_mutex_t shared_canvases_mutex = PTHREAD_MUTEX_INITIALIZER;

uint64_t create_handle(void) {
    pthread_mutex_lock(&handle_mutex);
    uint64_t h = handle_counter++;
    if (handle_counter == 0) handle_counter = 1;
    pthread_mutex_unlock(&handle_mutex);
    return h;
}

void init_users_file_path(const char* dir_path) {
    if (!dir_path || dir_path[0] == '\0') return;
    snprintf(users_file_path, sizeof(users_file_path), "%s/users.txt", dir_path);
}

static void add_global_resource(resource_t* res) {
    if (!res) return;
    pthread_mutex_lock(&global_resources_mutex);
    if (global_resource_count >= global_resource_capacity) {
        global_resource_capacity = global_resource_capacity ? global_resource_capacity * 2 : 16;
        resource_t** new_resources = realloc(global_resources, global_resource_capacity * sizeof(resource_t*));
        if (!new_resources) {
            pthread_mutex_unlock(&global_resources_mutex);
            return;
        }
        global_resources = new_resources;
    }
    global_resources[global_resource_count++] = res;
    pthread_mutex_unlock(&global_resources_mutex);
}

static void remove_global_resource(uint64_t handle) {
    pthread_mutex_lock(&global_resources_mutex);
    for (int i = 0; i < global_resource_count; i++) {
        if (global_resources[i]->handle == handle) {
            /* Do not free the resource here: ownership of the resource_t
             * structure is with the client that registered it. Removing
             * from the global registry should only unlink the pointer so
             * the client can free its own resource later. */
            global_resources[i] = global_resources[global_resource_count - 1];
            global_resource_count--;
            break;
        }
    }
    pthread_mutex_unlock(&global_resources_mutex);
}

static resource_t* find_global_resource(uint64_t handle, int expected_type) {
    pthread_mutex_lock(&global_resources_mutex);
    for (int i = 0; i < global_resource_count; i++) {
        if (global_resources[i]->handle == handle && global_resources[i]->type == expected_type) {
            resource_t* res = global_resources[i];
            pthread_mutex_unlock(&global_resources_mutex);
            return res;
        }
    }
    pthread_mutex_unlock(&global_resources_mutex);
    return NULL;
}

static shared_canvas_t* find_shared_canvas(uint64_t canvas_handle) {
    for (int i = 0; i < shared_canvas_count; i++) {
        if (shared_canvases[i]->canvas_id == canvas_handle) return shared_canvases[i];
    }
    return NULL;
}

static shared_canvas_t* ensure_shared_canvas(uint64_t canvas_handle) {
    shared_canvas_t* sc = find_shared_canvas(canvas_handle);
    if (sc) return sc;
    if (shared_canvas_count >= shared_canvas_capacity) {
        shared_canvas_capacity = shared_canvas_capacity ? shared_canvas_capacity * 2 : 16;
        shared_canvas_t** new_list = realloc(shared_canvases, shared_canvas_capacity * sizeof(shared_canvas_t*));
        if (!new_list) return NULL;
        shared_canvases = new_list;
    }
    sc = calloc(1, sizeof(shared_canvas_t));
    if (!sc) return NULL;
    sc->canvas_id = canvas_handle;
    sc->usernames = NULL;
    sc->user_count = 0;
    sc->user_capacity = 0;
    sc->barrier_users = NULL;
    sc->barrier_count = 0;
    sc->barrier_capacity = 0;
    pthread_mutex_init(&sc->mutex, NULL);
    pthread_cond_init(&sc->cond, NULL);
    shared_canvases[shared_canvas_count++] = sc;
    return sc;
}

static bool username_exists(const char* username) {
    if (!username) return false;
    FILE* fp = fopen(users_file_path, "r");
    if (!fp) return false;
    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        char* p = line;
        while (*p && (*p == ' ' || *p == '\t')) p++;
        char user[33];
        int bal;
        if (sscanf(p, "%32s %d", user, &bal) == 2) {
            if (strcmp(user, username) == 0) {
                found = true;
                break;
            }
        }
    }
    fclose(fp);
    return found;
}

static bool shared_canvas_has_user(shared_canvas_t* sc, const char* username) {
    if (!sc || !username) return false;
    for (int i = 0; i < sc->user_count; i++) {
        if (strcmp(sc->usernames[i], username) == 0) return true;
    }
    return false;
}

static bool shared_canvas_add_user(shared_canvas_t* sc, const char* username) {
    if (!sc || !username) return false;
    if (shared_canvas_has_user(sc, username)) return true;
    if (sc->user_count >= sc->user_capacity) {
        sc->user_capacity = sc->user_capacity ? sc->user_capacity * 2 : 8;
        char** new_users = realloc(sc->usernames, sc->user_capacity * sizeof(char*));
        if (!new_users) return false;
        sc->usernames = new_users;
    }
    sc->usernames[sc->user_count++] = strdup(username);
    return true;
}

static bool shared_canvas_has_barrier_user(shared_canvas_t* sc, const char* username) {
    if (!sc || !username) return false;
    for (int i = 0; i < sc->barrier_count; i++) {
        if (strcmp(sc->barrier_users[i], username) == 0) return true;
    }
    return false;
}

static void shared_canvas_add_barrier_user(shared_canvas_t* sc, const char* username) {
    if (!sc || !username || shared_canvas_has_barrier_user(sc, username)) return;
    if (sc->barrier_count >= sc->barrier_capacity) {
        sc->barrier_capacity = sc->barrier_capacity ? sc->barrier_capacity * 2 : 8;
        char** new_users = realloc(sc->barrier_users, sc->barrier_capacity * sizeof(char*));
        if (!new_users) return;
        sc->barrier_users = new_users;
    }
    sc->barrier_users[sc->barrier_count++] = strdup(username);
}

static void shared_canvas_clear_barrier(shared_canvas_t* sc) {
    if (!sc) return;
    for (int i = 0; i < sc->barrier_count; i++) free(sc->barrier_users[i]);
    free(sc->barrier_users);
    sc->barrier_users = NULL;
    sc->barrier_count = 0;
    sc->barrier_capacity = 0;
}

static int count_connected_shared_users(shared_canvas_t* sc);

static void remove_shared_canvas(uint64_t canvas_handle) {
    pthread_mutex_lock(&shared_canvases_mutex);
    for (int i = 0; i < shared_canvas_count; i++) {
        if (shared_canvases[i]->canvas_id == canvas_handle) {
            shared_canvas_t* sc = shared_canvases[i];
            for (int j = 0; j < sc->user_count; j++) free(sc->usernames[j]);
            free(sc->usernames);
            shared_canvas_clear_barrier(sc);
            free(sc);
            if (i != shared_canvas_count - 1) {
                shared_canvases[i] = shared_canvases[shared_canvas_count - 1];
            }
            shared_canvas_count--;
            break;
        }
    }
    pthread_mutex_unlock(&shared_canvases_mutex);
}

void prune_shared_canvases(void) {
    pthread_mutex_lock(&shared_canvases_mutex);
    int i = 0;
    while (i < shared_canvas_count) {
        shared_canvas_t* sc = shared_canvases[i];
        int connected_count = count_connected_shared_users(sc);
        if (connected_count == 0) {
            resource_t* res = find_global_resource(sc->canvas_id, 0);
            if (res) {
                animate_destroy_canvas((struct canvas*)res->ptr);
                remove_global_resource(sc->canvas_id);
                free(res);
            }
            for (int j = 0; j < sc->user_count; j++) free(sc->usernames[j]);
            free(sc->usernames);
            shared_canvas_clear_barrier(sc);
            free(sc);
            if (i != shared_canvas_count - 1) {
                shared_canvases[i] = shared_canvases[shared_canvas_count - 1];
            }
            shared_canvas_count--;
            continue;
        }
        i++;
    }
    pthread_mutex_unlock(&shared_canvases_mutex);
}

static int count_connected_shared_users(shared_canvas_t* sc) {
    if (!sc) return 0;
    int count = 0;
    pthread_mutex_lock(&sessions_mutex);
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].logged_in && shared_canvas_has_user(sc, sessions[i].username)) {
            count++;
        }
    }
    pthread_mutex_unlock(&sessions_mutex);
    return count;
}

static void shared_canvas_notify_change(void) {
    pthread_mutex_lock(&shared_canvases_mutex);
    for (int i = 0; i < shared_canvas_count; i++) {
        shared_canvas_t* sc = shared_canvases[i];
        pthread_mutex_lock(&sc->mutex);
        pthread_cond_broadcast(&sc->cond);
        pthread_mutex_unlock(&sc->mutex);
    }
    pthread_mutex_unlock(&shared_canvases_mutex);
}

static bool canvas_handle_shared_with_user(uint64_t handle, const char* username) {
    bool result = false;
    pthread_mutex_lock(&shared_canvases_mutex);
    shared_canvas_t* sc = find_shared_canvas(handle);
    if (sc) result = shared_canvas_has_user(sc, username);
    pthread_mutex_unlock(&shared_canvases_mutex);
    return result;
}

static bool canvas_handle_shared(uint64_t handle) {
    bool result = false;
    pthread_mutex_lock(&shared_canvases_mutex);
    if (find_shared_canvas(handle)) result = true;
    pthread_mutex_unlock(&shared_canvases_mutex);
    return result;
}

void register_resource(client_session_t* client, uint64_t handle, void* ptr, int type) {
    if (!client || !ptr || handle == 0) return;
    if (client->resource_count >= client->resource_capacity) {
        client->resource_capacity = client->resource_capacity ? client->resource_capacity * 2 : 16;
        resource_t** new_resources = realloc(client->resources,
                                            client->resource_capacity * sizeof(resource_t*));
        if (!new_resources) return;
        client->resources = new_resources;
    }
    resource_t* res = malloc(sizeof(resource_t));
    if (!res) return;
    res->handle = handle;
    res->client_pid = client->client_pid;
    res->type = type;
    res->ptr = ptr;
    client->resources[client->resource_count++] = res;
    add_global_resource(res);
}

void unregister_resource(client_session_t* client, uint64_t handle) {
    if (!client || handle == 0) return;
    for (int i = 0; i < client->resource_count; i++) {
        if (client->resources[i]->handle == handle) {
            remove_global_resource(handle);
            free(client->resources[i]);
            client->resources[i] = client->resources[client->resource_count - 1];
            client->resource_count--;
            return;
        }
    }
}

void* get_resource(client_session_t* client, uint64_t handle, int expected_type) {
    if (!client || handle == 0) return NULL;
    for (int i = 0; i < client->resource_count; i++) {
        if (client->resources[i]->handle == handle && client->resources[i]->type == expected_type) {
            return client->resources[i]->ptr;
        }
    }
    if (expected_type == 0 && client->logged_in && canvas_handle_shared_with_user(handle, client->username)) {
        resource_t* res = find_global_resource(handle, expected_type);
        if (res) return res->ptr;
    }
    return NULL;
}

void cleanup_client_resources(client_session_t* client) {
    if (!client) return;
    for (int i = client->resource_count - 1; i >= 0; i--) {
        resource_t* res = client->resources[i];
        if (res->type == 2) {
            animate_destroy_placement((struct sprite_placement*)res->ptr);
            remove_global_resource(res->handle);
            free(res);
            client->resource_count--;
            client->resources[i] = client->resources[client->resource_count];
        }
    }
    for (int i = client->resource_count - 1; i >= 0; i--) {
        resource_t* res = client->resources[i];
        if (res->type == 1) {
            animate_destroy_sprite((struct sprite*)res->ptr);
            remove_global_resource(res->handle);
            free(res);
            client->resource_count--;
            client->resources[i] = client->resources[client->resource_count];
        }
    }
    for (int i = client->resource_count - 1; i >= 0; i--) {
        resource_t* res = client->resources[i];
        if (res->type == 0) {
            if (canvas_handle_shared(res->handle)) {
                client->resource_count--;
                client->resources[i] = client->resources[client->resource_count];
            } else {
                animate_destroy_canvas((struct canvas*)res->ptr);
                remove_global_resource(res->handle);
                free(res);
                client->resource_count--;
                client->resources[i] = client->resources[client->resource_count];
            }
        }
    }
    free(client->resources);
    client->resources = NULL;
    client->resource_count = 0;
    client->resource_capacity = 0;
    shared_canvas_notify_change();
}

int check_login(const char* username, int* balance) {
    FILE* fp = fopen(users_file_path, "r");
    if (!fp) return -1;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char* p = line;
        while (*p && (*p == ' ' || *p == '\t')) p++;
        char user[33];
        int bal;
        if (sscanf(p, "%32s %d", user, &bal) == 2) {
            if (strcmp(username, user) == 0) {
                *balance = bal;
                fclose(fp);
                return (bal > 0) ? 1 : 0;
            }
        }
    }
    fclose(fp);
    return -1;
}

static int parse_args(const char* cmd, char* tokens[], int max_tokens) {
    char buffer[2048];
    strncpy(buffer, cmd, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    int count = 0;
    char* token = strtok(buffer, " \t");
    while (token && count < max_tokens) {
        tokens[count++] = strdup(token);
        token = strtok(NULL, " \t");
    }
    return count;
}

static void free_tokens(char* tokens[], int count) {
    for (int i = 0; i < count; i++) free(tokens[i]);
}

static void cmd_login(client_session_t* client, char* tokens[], int argc,
                      char* response, size_t response_size) {
    if (argc != 2) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    int balance;
    int result = check_login(tokens[1], &balance);
    if (result == -1) {
        snprintf(response, response_size, "Reject UNAUTHORISED\n");
        return;
    }
    if (result == 0) {
        snprintf(response, response_size, "Reject BALANCE\n");
        return;
    }
    client->logged_in = 1;
    strncpy(client->username, tokens[1], sizeof(client->username) - 1);
    client->username[sizeof(client->username) - 1] = '\0';
    client->balance = balance;
    snprintf(response, response_size, "%d\n", balance);
}

static void cmd_create_rectangle(client_session_t* client, char* tokens[], int argc,
                                 char* response, size_t response_size) {
    if (argc != 5) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    char* endp;
    size_t w = strtoul(tokens[1], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    size_t h = strtoul(tokens[2], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    color_t c = (color_t)strtoul(tokens[3], &endp, 16);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    int filled = strtol(tokens[4], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    struct sprite* s = animate_create_rectangle(w, h, c, filled != 0);
    if (!s) { snprintf(response, response_size, "%d\n", RPC_INTERNAL_ERROR); return; }
    uint64_t handle = create_handle();
    register_resource(client, handle, s, 1);
    snprintf(response, response_size, "0 %lu\n", (unsigned long)handle);
}

static void cmd_create_circle(client_session_t* client, char* tokens[], int argc,
                             char* response, size_t response_size) {
    if (argc < 3 || argc > 4) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    char* endp;
    size_t r = strtoul(tokens[1], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    color_t c = (color_t)strtoul(tokens[2], &endp, 16);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    int filled = 1;
    if (argc > 3) {
        filled = strtol(tokens[3], &endp, 10);
        if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    }
    struct sprite* s = animate_create_circle(r, c, filled != 0);
    if (!s) { snprintf(response, response_size, "%d\n", RPC_INTERNAL_ERROR); return; }
    uint64_t handle = create_handle();
    register_resource(client, handle, s, 1);
    snprintf(response, response_size, "0 %lu\n", (unsigned long)handle);
}

static void cmd_create_sprite(client_session_t* client, char* tokens[], int argc,
                             char* response, size_t response_size) {
    if (argc != 2) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    struct sprite* s = animate_create_sprite(tokens[1]);
    if (!s) { snprintf(response, response_size, "%d\n", RPC_INTERNAL_ERROR); return; }
    uint64_t handle = create_handle();
    register_resource(client, handle, s, 1);
    snprintf(response, response_size, "0 %lu\n", (unsigned long)handle);
}

static void cmd_create_canvas(client_session_t* client, char* tokens[], int argc,
                             char* response, size_t response_size) {
    if (argc != 4) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    char* endp;
    size_t h = strtoul(tokens[1], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    size_t w = strtoul(tokens[2], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    color_t bg = (color_t)strtoul(tokens[3], &endp, 16);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    struct canvas* c = animate_create_canvas(h, w, bg);
    if (!c) { snprintf(response, response_size, "%d\n", RPC_INTERNAL_ERROR); return; }
    uint64_t handle = create_handle();
    register_resource(client, handle, c, 0);
    snprintf(response, response_size, "0 %lu\n", (unsigned long)handle);
}

static void cmd_place_sprite(client_session_t* client, char* tokens[], int argc,
                            char* response, size_t response_size) {
    if (argc != 5) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    char* endp;
    uint64_t canvas_h = strtoul(tokens[1], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    uint64_t sprite_h = strtoul(tokens[2], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    ssize_t x = strtol(tokens[3], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    ssize_t y = strtol(tokens[4], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    struct canvas* c = get_resource(client, canvas_h, 0);
    struct sprite* s = get_resource(client, sprite_h, 1);
    if (!c || !s) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    struct sprite_placement* p = animate_place_sprite(c, s, x, y);
    if (!p) { snprintf(response, response_size, "%d\n", RPC_INTERNAL_ERROR); return; }
    uint64_t handle = create_handle();
    register_resource(client, handle, p, 2);
    snprintf(response, response_size, "0 %lu\n", (unsigned long)handle);
}

static void cmd_placement_up(client_session_t* client, char* tokens[], int argc,
                            char* response, size_t response_size) {
    if (argc != 2) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    char* endp;
    uint64_t placement_h = strtoul(tokens[1], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    struct sprite_placement* p = get_resource(client, placement_h, 2);
    if (!p) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    animate_placement_up(p);
    snprintf(response, response_size, "%d\n", RPC_SUCCESS);
}

static void cmd_placement_down(client_session_t* client, char* tokens[], int argc,
                              char* response, size_t response_size) {
    if (argc != 2) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    char* endp;
    uint64_t placement_h = strtoul(tokens[1], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    struct sprite_placement* p = get_resource(client, placement_h, 2);
    if (!p) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    animate_placement_down(p);
    snprintf(response, response_size, "%d\n", RPC_SUCCESS);
}

static void cmd_placement_top(client_session_t* client, char* tokens[], int argc,
                             char* response, size_t response_size) {
    if (argc != 2) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    char* endp;
    uint64_t placement_h = strtoul(tokens[1], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    struct sprite_placement* p = get_resource(client, placement_h, 2);
    if (!p) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    animate_placement_top(p);
    snprintf(response, response_size, "%d\n", RPC_SUCCESS);
}

static void cmd_placement_bottom(client_session_t* client, char* tokens[], int argc,
                                char* response, size_t response_size) {
    if (argc != 2) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    char* endp;
    uint64_t placement_h = strtoul(tokens[1], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    struct sprite_placement* p = get_resource(client, placement_h, 2);
    if (!p) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    animate_placement_bottom(p);
    snprintf(response, response_size, "%d\n", RPC_SUCCESS);
}

static void cmd_destroy_placement(client_session_t* client, char* tokens[], int argc,
                                 char* response, size_t response_size) {
    if (argc != 2) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    char* endp;
    uint64_t placement_h = strtoul(tokens[1], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    struct sprite_placement* p = get_resource(client, placement_h, 2);
    if (!p) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    animate_destroy_placement(p);
    unregister_resource(client, placement_h);
    snprintf(response, response_size, "%d\n", RPC_SUCCESS);
}

static void cmd_destroy_sprite(client_session_t* client, char* tokens[], int argc,
                              char* response, size_t response_size) {
    if (argc != 2) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    char* endp;
    uint64_t sprite_h = strtoul(tokens[1], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    struct sprite* s = get_resource(client, sprite_h, 1);
    if (!s) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    bool result = animate_destroy_sprite(s);
    unregister_resource(client, sprite_h);
    snprintf(response, response_size, "%d %d\n", RPC_SUCCESS, result ? 1 : 0);
}

static void cmd_destroy_canvas(client_session_t* client, char* tokens[], int argc,
                              char* response, size_t response_size) {
    if (argc != 2) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    char* endp;
    uint64_t canvas_h = strtoul(tokens[1], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    struct canvas* c = get_resource(client, canvas_h, 0);
    if (!c) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    /* Remove any placement resources that reference this canvas from all clients
     * before destroying the canvas itself to avoid later use-after-free when
     * placement pointers are referenced again during cleanup. */
    pthread_mutex_lock(&sessions_mutex);
    for (int si = 0; si < session_count; si++) {
        client_session_t* cs = &sessions[si];
        for (int i = cs->resource_count - 1; i >= 0; i--) {
            resource_t* res = cs->resources[i];
            if (res->type == 2) {
                struct sprite_placement* p = (struct sprite_placement*)res->ptr;
                if (p && animate_get_placement_canvas(p) == c) {
                    remove_global_resource(res->handle);
                    free(res);
                    cs->resource_count--;
                    cs->resources[i] = cs->resources[cs->resource_count];
                }
            }
        }
    }
    pthread_mutex_unlock(&sessions_mutex);

    animate_destroy_canvas(c);
    unregister_resource(client, canvas_h);
    if (canvas_handle_shared(canvas_h)) {
        remove_shared_canvas(canvas_h);
    }
    snprintf(response, response_size, "%d\n", RPC_SUCCESS);
}

static void cmd_set_animation_params(client_session_t* client, char* tokens[], int argc,
                                    char* response, size_t response_size) {
    if (argc != 6) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    char* endp;
    uint64_t placement_h = strtoul(tokens[1], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    ssize_t vx = strtol(tokens[2], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    ssize_t vy = strtol(tokens[3], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    ssize_t ax = strtol(tokens[4], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    ssize_t ay = strtol(tokens[5], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    struct sprite_placement* p = get_resource(client, placement_h, 2);
    if (!p) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    animate_set_animation_params(p, vx, vy, ax, ay);
    snprintf(response, response_size, "%d\n", RPC_SUCCESS);
}

static void cmd_set_animation_function(client_session_t* client, char* tokens[], int argc,
                                       char* response, size_t response_size) {
    if (argc != 2 && argc != 3) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    char* endp;
    uint64_t placement_h = strtoul(tokens[1], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    struct sprite_placement* p = get_resource(client, placement_h, 2);
    if (!p) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    if (argc == 3 && strcasecmp(tokens[2], "none") != 0) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    animate_set_animation_function(p, NULL, NULL);
    snprintf(response, response_size, "%d\n", RPC_SUCCESS);
}

static void cmd_frame_size_bytes(client_session_t* client, char* tokens[], int argc,
                                 char* response, size_t response_size) {
    if (argc != 2) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    char* endp;
    uint64_t canvas_h = strtoul(tokens[1], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    struct canvas* c = get_resource(client, canvas_h, 0);
    if (!c) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    size_t size = animate_frame_size_bytes(c);
    snprintf(response, response_size, "0 %zu\n", size);
}

static void cmd_share_canvas(client_session_t* client, char* tokens[], int argc,
                            char* response, size_t response_size) {
    if (argc != 3) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    char* endp;
    uint64_t canvas_h = strtoul(tokens[1], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    const char* other_username = tokens[2];
    if (!username_exists(other_username)) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    struct canvas* c = get_resource(client, canvas_h, 0);
    if (!c && !canvas_handle_shared_with_user(canvas_h, client->username)) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    pthread_mutex_lock(&shared_canvases_mutex);
    shared_canvas_t* sc = ensure_shared_canvas(canvas_h);
    if (!sc) {
        pthread_mutex_unlock(&shared_canvases_mutex);
        snprintf(response, response_size, "%d\n", RPC_INTERNAL_ERROR);
        return;
    }
    pthread_mutex_lock(&sc->mutex);
    shared_canvas_add_user(sc, client->username);
    shared_canvas_add_user(sc, other_username);
    pthread_mutex_unlock(&sc->mutex);
    pthread_mutex_unlock(&shared_canvases_mutex);
    snprintf(response, response_size, "%d\n", RPC_SUCCESS);
}

static void cmd_barrier(client_session_t* client, char* tokens[], int argc,
                       char* response, size_t response_size) {
    if (argc != 2) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    char* endp;
    uint64_t canvas_h = strtoul(tokens[1], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    pthread_mutex_lock(&shared_canvases_mutex);
    shared_canvas_t* sc = find_shared_canvas(canvas_h);
    if (!sc) {
        pthread_mutex_unlock(&shared_canvases_mutex);
        snprintf(response, response_size, "%d\n", RPC_SUCCESS);
        return;
    }
    pthread_mutex_lock(&sc->mutex);
    pthread_mutex_unlock(&shared_canvases_mutex);
    if (!shared_canvas_has_user(sc, client->username)) {
        pthread_mutex_unlock(&sc->mutex);
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    int connected_count = count_connected_shared_users(sc);
    if (connected_count <= 1) {
        pthread_mutex_unlock(&sc->mutex);
        snprintf(response, response_size, "%d\n", RPC_SUCCESS);
        return;
    }
    shared_canvas_add_barrier_user(sc, client->username);
    while (true) {
        connected_count = count_connected_shared_users(sc);
        if (sc->barrier_count >= connected_count) break;
        pthread_cond_wait(&sc->cond, &sc->mutex);
    }
    if (sc->barrier_count >= connected_count) {
        shared_canvas_clear_barrier(sc);
        pthread_cond_broadcast(&sc->cond);
    }
    pthread_mutex_unlock(&sc->mutex);
    snprintf(response, response_size, "%d\n", RPC_SUCCESS);
}

static void cmd_generate(client_session_t* client, char* tokens[], int argc,
                        char* response, size_t response_size) {
    if (argc != 6) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    char* endp;
    uint64_t canvas_h = strtoul(tokens[1], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    const char* filename = tokens[2];
    int start = strtol(tokens[3], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    int end = strtol(tokens[4], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    int fps = strtol(tokens[5], &endp, 10);
    if (*endp) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    if (start < 0 || end < start || fps <= 0) {
        snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR);
        return;
    }
    struct canvas* c = get_resource(client, canvas_h, 0);
    if (!c) { snprintf(response, response_size, "%d\n", RPC_VALUE_ERROR); return; }
    size_t w = animate_get_canvas_width(c);
    size_t h = animate_get_canvas_height(c);
    size_t frame_size = w * h * sizeof(color_t);
    void* buf = malloc(frame_size);
    if (!buf) { snprintf(response, response_size, "%d\n", RPC_INTERNAL_ERROR); return; }
    char datfile[256], mp4file[256], logfile[256];
    snprintf(datfile, sizeof(datfile), "%s.dat", filename);
    snprintf(mp4file, sizeof(mp4file), "%s.mp4", filename);
    snprintf(logfile, sizeof(logfile), "%s.log", filename);
    FILE* dat = fopen(datfile, "wb");
    if (!dat) {
        free(buf);
        snprintf(response, response_size, "%d\n", RPC_INTERNAL_ERROR);
        return;
    }
    for (int f = start; f <= end; f++) {
        animate_generate_frame(c, f, fps, buf);
        if (fwrite(buf, 1, frame_size, dat) != frame_size) {
            fclose(dat);
            free(buf);
            snprintf(response, response_size, "%d\n", RPC_INTERNAL_ERROR);
            return;
        }
    }
    fclose(dat);
    free(buf);
    const char* ffmpeg_cmd = "ffmpeg";
    char video_size[64];
    char fps_str[32];
    snprintf(video_size, sizeof(video_size), "%zux%zu", w, h);
    snprintf(fps_str, sizeof(fps_str), "%d", fps);

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(response, response_size, "%d\n", RPC_INTERNAL_ERROR);
        return;
    }

    if (pid == 0) {
        int log_fd = open(logfile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (log_fd < 0) _exit(127);
        if (dup2(log_fd, STDOUT_FILENO) < 0) _exit(127);
        if (dup2(log_fd, STDERR_FILENO) < 0) _exit(127);
        close(log_fd);
        execlp(ffmpeg_cmd, ffmpeg_cmd,
               "-y",
               "-f", "rawvideo",
               "-pixel_format", "bgra",
               "-video_size", video_size,
               "-framerate", fps_str,
               "-i", datfile,
               "-c:v", "libx264",
               "-pix_fmt", "yuv420p",
               mp4file,
               (char*)NULL);
        _exit(127);
    }

    int status;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            snprintf(response, response_size, "%d\n", RPC_INTERNAL_ERROR);
            return;
        }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(response, response_size, "%d\n", RPC_INTERNAL_ERROR);
        return;
    }

    snprintf(response, response_size, "0\n");
}

static void cmd_disconnect(client_session_t* client, char* tokens[], int argc,
                          char* response, size_t response_size) {
    (void)tokens;
    (void)argc;
    client->logged_in = 0;
    snprintf(response, response_size, "%d\n", RPC_SUCCESS);
}

void handle_rpc(client_session_t* client, const char* command, char* response,
                size_t response_size) {
    if (!client || !command || !response) return;
    char* tokens[32];
    int argc = parse_args(command, tokens, 32);
    if (argc == 0) {
        snprintf(response, response_size, "%d\n", RPC_FAILED);
        return;
    }
    if (strcasecmp(tokens[0], "Login") != 0 && strcasecmp(tokens[0], "Disconnect") != 0) {
        if (!client->logged_in) {
            snprintf(response, response_size, "Not logged in\n");
            free_tokens(tokens, argc);
            return;
        }
    }

    if (strcasecmp(tokens[0], "Login") == 0) {
        cmd_login(client, tokens, argc, response, response_size);
    } else if (argc >= 2 && strcasecmp(tokens[0], "create") == 0) {
        if (strcasecmp(tokens[1], "canvas") == 0) {
            cmd_create_canvas(client, tokens + 1, argc - 1, response, response_size);
        } else if (strcasecmp(tokens[1], "rectangle") == 0) {
            cmd_create_rectangle(client, tokens + 1, argc - 1, response, response_size);
        } else if (strcasecmp(tokens[1], "circle") == 0) {
            cmd_create_circle(client, tokens + 1, argc - 1, response, response_size);
        } else if (strcasecmp(tokens[1], "sprite") == 0) {
            cmd_create_sprite(client, tokens + 1, argc - 1, response, response_size);
        } else {
            snprintf(response, response_size, "%d\n", RPC_FAILED);
        }
    } else if (argc >= 2 && strcasecmp(tokens[0], "place") == 0 && strcasecmp(tokens[1], "sprite") == 0) {
        cmd_place_sprite(client, tokens + 1, argc - 1, response, response_size);
    } else if (argc >= 2 && strcasecmp(tokens[0], "placement") == 0) {
        if (strcasecmp(tokens[1], "up") == 0) {
            cmd_placement_up(client, tokens + 1, argc - 1, response, response_size);
        } else if (strcasecmp(tokens[1], "down") == 0) {
            cmd_placement_down(client, tokens + 1, argc - 1, response, response_size);
        } else if (strcasecmp(tokens[1], "top") == 0) {
            cmd_placement_top(client, tokens + 1, argc - 1, response, response_size);
        } else if (strcasecmp(tokens[1], "bottom") == 0) {
            cmd_placement_bottom(client, tokens + 1, argc - 1, response, response_size);
        } else {
            snprintf(response, response_size, "%d\n", RPC_FAILED);
        }
    } else if (argc >= 2 && strcasecmp(tokens[0], "destroy") == 0) {
        if (strcasecmp(tokens[1], "placement") == 0) {
            cmd_destroy_placement(client, tokens + 1, argc - 1, response, response_size);
        } else if (strcasecmp(tokens[1], "sprite") == 0) {
            cmd_destroy_sprite(client, tokens + 1, argc - 1, response, response_size);
        } else if (strcasecmp(tokens[1], "canvas") == 0) {
            cmd_destroy_canvas(client, tokens + 1, argc - 1, response, response_size);
        } else {
            snprintf(response, response_size, "%d\n", RPC_FAILED);
        }
    } else if (argc >= 3 && strcasecmp(tokens[0], "set") == 0 && strcasecmp(tokens[1], "animation") == 0 && strcasecmp(tokens[2], "params") == 0) {
        cmd_set_animation_params(client, tokens + 2, argc - 2, response, response_size);
    } else if (strcasecmp(tokens[0], "create_rectangle") == 0 || strcasecmp(tokens[0], "animate_create_rectangle") == 0) {
        cmd_create_rectangle(client, tokens, argc, response, response_size);
    } else if (strcasecmp(tokens[0], "create_circle") == 0 || strcasecmp(tokens[0], "animate_create_circle") == 0) {
        cmd_create_circle(client, tokens, argc, response, response_size);
    } else if (strcasecmp(tokens[0], "create_sprite") == 0 || strcasecmp(tokens[0], "animate_create_sprite") == 0) {
        cmd_create_sprite(client, tokens, argc, response, response_size);
    } else if (strcasecmp(tokens[0], "create_canvas") == 0 || strcasecmp(tokens[0], "animate_create_canvas") == 0) {
        cmd_create_canvas(client, tokens, argc, response, response_size);
    } else if (strcasecmp(tokens[0], "place_sprite") == 0 || strcasecmp(tokens[0], "animate_place_sprite") == 0) {
        cmd_place_sprite(client, tokens, argc, response, response_size);
    } else if (strcasecmp(tokens[0], "placement_up") == 0 || strcasecmp(tokens[0], "animate_placement_up") == 0) {
        cmd_placement_up(client, tokens, argc, response, response_size);
    } else if (strcasecmp(tokens[0], "placement_down") == 0 || strcasecmp(tokens[0], "animate_placement_down") == 0) {
        cmd_placement_down(client, tokens, argc, response, response_size);
    } else if (strcasecmp(tokens[0], "placement_top") == 0 || strcasecmp(tokens[0], "animate_placement_top") == 0) {
        cmd_placement_top(client, tokens, argc, response, response_size);
    } else if (strcasecmp(tokens[0], "placement_bottom") == 0 || strcasecmp(tokens[0], "animate_placement_bottom") == 0) {
        cmd_placement_bottom(client, tokens, argc, response, response_size);
    } else if (strcasecmp(tokens[0], "destroy_placement") == 0 || strcasecmp(tokens[0], "animate_destroy_placement") == 0) {
        cmd_destroy_placement(client, tokens, argc, response, response_size);
    } else if (strcasecmp(tokens[0], "destroy_sprite") == 0 || strcasecmp(tokens[0], "animate_destroy_sprite") == 0) {
        cmd_destroy_sprite(client, tokens, argc, response, response_size);
    } else if (strcasecmp(tokens[0], "destroy_canvas") == 0 || strcasecmp(tokens[0], "animate_destroy_canvas") == 0) {
        cmd_destroy_canvas(client, tokens, argc, response, response_size);
    } else if (strcasecmp(tokens[0], "set_animation_params") == 0 || strcasecmp(tokens[0], "animate_set_animation_params") == 0) {
        cmd_set_animation_params(client, tokens, argc, response, response_size);
    } else if (strcasecmp(tokens[0], "animate_set_animation_function") == 0) {
        cmd_set_animation_function(client, tokens, argc, response, response_size);
    } else if (strcasecmp(tokens[0], "animate_frame_size_bytes") == 0) {
        cmd_frame_size_bytes(client, tokens, argc, response, response_size);
    } else if (strcasecmp(tokens[0], "share_canvas") == 0) {
        cmd_share_canvas(client, tokens, argc, response, response_size);
    } else if (strcasecmp(tokens[0], "barrier") == 0) {
        cmd_barrier(client, tokens, argc, response, response_size);
    } else if (strcasecmp(tokens[0], "generate") == 0 || strcasecmp(tokens[0], "animate_generate_frame") == 0) {
        cmd_generate(client, tokens, argc, response, response_size);
    } else if (strcasecmp(tokens[0], "Disconnect") == 0 || strcasecmp(tokens[0], "disconnect") == 0) {
        cmd_disconnect(client, tokens, argc, response, response_size);
    } else {
        snprintf(response, response_size, "%d\n", RPC_FAILED);
    }
    free_tokens(tokens, argc);
}
