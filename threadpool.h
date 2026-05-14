#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>

typedef struct threadpool_task {
    void (*function)(void*);
    void* argument;
    struct threadpool_task* next;
} threadpool_task_t;

typedef struct {
    pthread_t* threads;
    threadpool_task_t* task_queue;
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
    pthread_cond_t queue_empty;
    int num_threads;
    int running;
    int pending_tasks;
} threadpool_t;

threadpool_t* threadpool_create(int num_threads);
void threadpool_destroy(threadpool_t* pool);
int threadpool_add_task(threadpool_t* pool, void (*function)(void*), void* argument);

#endif