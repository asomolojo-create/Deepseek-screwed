#ifndef RPC_HANDLER_H
#define RPC_HANDLER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include "animate.h"

typedef struct resource {
    uint64_t handle;
    int client_pid;
    int type;
    void* ptr;
} resource_t;

typedef struct shared_canvas {
    uint64_t canvas_id;
    char** usernames;
    int user_count;
    int user_capacity;
    char** barrier_users;
    int barrier_count;
    int barrier_capacity;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} shared_canvas_t;

typedef struct client_session {
    int client_pid;
    char username[33];
    int logged_in;
    int balance;
    resource_t** resources;
    int resource_count;
    int resource_capacity;
    struct client_session* next;
} client_session_t;

#define RPC_SUCCESS 0
#define RPC_FAILED -1
#define RPC_VALUE_ERROR -2
#define RPC_INTERNAL_ERROR -3

int check_login(const char* username, int* balance);
void init_users_file_path(const char* dir_path);
void handle_rpc(client_session_t* client, const char* command, char* response, size_t response_size);
void register_resource(client_session_t* client, uint64_t handle, void* ptr, int type);
void unregister_resource(client_session_t* client, uint64_t handle);
void* get_resource(client_session_t* client, uint64_t handle, int expected_type);
void cleanup_client_resources(client_session_t* client);
void prune_shared_canvases(void);
uint64_t create_handle(void);

#endif

