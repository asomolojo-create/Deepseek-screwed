#include "threadpool.h"
#include <stdlib.h>
#include <stdio.h>

static void* worker_thread(void* arg) {
    threadpool_t* pool = (threadpool_t*)arg;
    
    while (1) {
        pthread_mutex_lock(&pool->queue_mutex);
        
        while (pool->task_queue == NULL && pool->running) {
            pthread_cond_wait(&pool->queue_cond, &pool->queue_mutex);
        }
        
        if (!pool->running && pool->task_queue == NULL) {
            pthread_mutex_unlock(&pool->queue_mutex);
            break;
        }
        
        threadpool_task_t* task = pool->task_queue;
        if (task) {
            pool->task_queue = task->next;
            pool->pending_tasks--;
            pthread_mutex_unlock(&pool->queue_mutex);
            
            task->function(task->argument);
            free(task);
        } else {
            pthread_mutex_unlock(&pool->queue_mutex);
        }
    }
    
    return NULL;
}

threadpool_t* threadpool_create(int num_threads) {
    threadpool_t* pool = malloc(sizeof(threadpool_t));
    if (!pool) return NULL;
    
    pool->threads = malloc(sizeof(pthread_t) * num_threads);
    pool->task_queue = NULL;
    pool->num_threads = num_threads;
    pool->running = 1;
    pool->pending_tasks = 0;
    
    pthread_mutex_init(&pool->queue_mutex, NULL);
    pthread_cond_init(&pool->queue_cond, NULL);
    pthread_cond_init(&pool->queue_empty, NULL);
    
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&pool->threads[i], NULL, worker_thread, pool);
    }
    
    return pool;
}

int threadpool_add_task(threadpool_t* pool, void (*function)(void*), void* argument) {
    if (!pool || !function) return -1;
    
    threadpool_task_t* task = malloc(sizeof(threadpool_task_t));
    if (!task) return -1;
    
    task->function = function;
    task->argument = argument;
    task->next = NULL;
    
    pthread_mutex_lock(&pool->queue_mutex);
    
    threadpool_task_t** current = &pool->task_queue;
    while (*current) current = &(*current)->next;
    *current = task;
    pool->pending_tasks++;
    
    pthread_cond_signal(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);
    
    return 0;
}

void threadpool_destroy(threadpool_t* pool) {
    if (!pool) return;
    
    pthread_mutex_lock(&pool->queue_mutex);
    pool->running = 0;
    pthread_cond_broadcast(&pool->queue_cond);
    pthread_mutex_unlock(&pool->queue_mutex);
    
    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    
    // Clean up remaining tasks
    threadpool_task_t* task = pool->task_queue;
    while (task) {
        threadpool_task_t* next = task->next;
        free(task);
        task = next;
    }
    
    pthread_mutex_destroy(&pool->queue_mutex);
    pthread_cond_destroy(&pool->queue_cond);
    pthread_cond_destroy(&pool->queue_empty);
    
    free(pool->threads);
    free(pool);
}