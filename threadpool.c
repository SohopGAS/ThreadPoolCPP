//
//  threadpool.c
//  ThreadPool
//
//  Created by Gal Argov Sofer on 2/4/18.
//  Copyright © 2018 Gal Argov Sofer. All rights reserved.
//

#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#include "threadpool.h"

// Structs
typedef struct {
    pthread_t * tids;               // Number of abailable threads.
    void (*function)(void *);       // Function pointer to the function that will perform the task.
    void *argument;                 // Argument to be passed to the function.
} threadpool_task_t;

struct threadpool_t {
    pthread_mutex_t lock;           // Mutex variable to lock threads.
    pthread_cond_t notify;          // Condition variable to notify worker threads.
    pthread_t *threads;             // Array containing worker threads ID.
    threadpool_task_t *queue;       // Array containing the task queue.
    int thread_count;               // Number of threads.
    unsigned long long queue_size;  // Size of the task queue.
    int head;                       // Index of the first element.
    int tail;                       // Index of the last element.
    int count;                      // Number of pending tasks
    int shutdown;                   // Flag indicating if the pool is shutting down.
    int started;                    // Number of started threads.
};

// Functions
threadpool_t *threadpool_create(unsigned long long thread_count, unsigned long long queue_size, int flags) {
    threadpool_t *pool;
    int i;
    
    if(thread_count <= 0 || thread_count > MAX_THREADS || queue_size <= 0 || queue_size > MAX_QUEUE) {
        return NULL;
    }
    
    if((pool = (threadpool_t *)malloc(sizeof(threadpool_t))) == NULL) {
        goto err;
    }
    
    /* Initialize */
    pool->thread_count = 0;
    pool->queue_size = queue_size;
    pool->head = pool->tail = pool->count = 0;
    pool->shutdown = pool->started = 0;
    
    /* Allocate thread and task queue */
    pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * thread_count);
    pool->queue = (threadpool_task_t *)malloc
    (sizeof(threadpool_task_t) * queue_size);
    
    /* Initialize mutex and conditional variable first */
    if((pthread_mutex_init(&(pool->lock), NULL) != 0) ||
       (pthread_cond_init(&(pool->notify), NULL) != 0) ||
       (pool->threads == NULL) ||
       (pool->queue == NULL)) {
        goto err;
    }
    
    /* Start worker threads */
    for(i = 0; i < thread_count; i++) {
        if(pthread_create(&(pool->threads[i]), NULL,
                          threadpool_thread, (void*)pool) != 0) {
            threadpool_destroy(pool, 0);
            return NULL;
        }
        pool->thread_count++;
        pool->started++;
    }
    
    return pool;
    
err:
    if(pool) {
        threadpool_free(pool);
    }
    return NULL;

}

int threadpool_add(threadpool_t * pool, void* (function)(void *), void * argument) {
    int err = 0;
    int next;
//    (void) flags;
    
    if(pool == NULL || function == NULL) {
        return threadpool_invalid;
    }
    
    if(pthread_mutex_lock(&(pool->lock)) != 0) {
        return threadpool_lock_failure;
    }
    
    next = (pool->tail + 1) % pool->queue_size;
    
    do {
        /* Are we full ? */
        if(pool->count == pool->queue_size) {
            err = threadpool_queue_full;
            break;
        }
        
        /* Are we shutting down ? */
        if(pool->shutdown) {
            err = threadpool_shutdown;
            break;
        }
        
        /* Add task to queue */
        pool->queue[pool->tail].function = function;
        pool->queue[pool->tail].argument = argument;
        pool->tail = next;
        pool->count += 1;
        
        /* pthread_cond_broadcast */
        if(pthread_cond_signal(&(pool->notify)) != 0) {
            err = threadpool_lock_failure;
            break;
        }
    } while(0);
    
    if(pthread_mutex_unlock(&pool->lock) != 0) {
        err = threadpool_lock_failure;
    }
    
    return err;

}
int threadpool_destroy(threadpool_t *pool, int flags){
    int i, err = 0;
    
    if(pool == NULL) {
        return threadpool_invalid;
    }
    
    if(pthread_mutex_lock(&(pool->lock)) != 0) {
        return threadpool_lock_failure;
    }
    
    do {
        /* Already shutting down */
        if(pool->shutdown) {
            err = threadpool_shutdown;
            break;
        }
        
//        pool->shutdown = (flags & threadpool_graceful) ? graceful_shutdown : immediate_shutdown;
        
        /* Wake up all worker threads */
        if((pthread_cond_broadcast(&(pool->notify)) != 0) ||
           (pthread_mutex_unlock(&(pool->lock)) != 0)) {
            err = threadpool_lock_failure;
            break;
        }
        
        /* Join all worker thread */
        for(i = 0; i < pool->thread_count; i++) {
            if(pthread_join(pool->threads[i], NULL) != 0) {
                err = threadpool_thread_failure;
            }
        }
    } while(0);
    
    /* Only if everything went well do we deallocate the pool */
    if(!err) {
        threadpool_free(pool);
    }
    return err;

}
int threadpool_free(threadpool_t *pool){
    if(pool == NULL || pool->started > 0) {
        return -1;
    }
    
    /* Did we manage to allocate ? */
    if(pool->threads) {
        free(pool->threads);
        free(pool->queue);
        
        /* Because we allocate pool->threads after initializing the
         mutex and condition variable, we're sure they're
         initialized. Let's lock the mutex just in case. */
        pthread_mutex_lock(&(pool->lock));
        pthread_mutex_destroy(&(pool->lock));
        pthread_cond_destroy(&(pool->notify));
    }
    free(pool);
    return 0;

}

int thpool_num_threads_working(threadpool_t *pool){
    return pool->thread_count;
}
void *threadpool_thread(void *threadpool){
    threadpool_t *pool = (threadpool_t *)threadpool;
    threadpool_task_t task;
    
    for(;;) {
        /* Lock must be taken to wait on conditional variable */
        pthread_mutex_lock(&(pool->lock));
        
        /* Wait on condition variable, check for spurious wakeups.
         When returning from pthread_cond_wait(), we own the lock. */
        while((pool->count == 0) && (!pool->shutdown)) {
            pthread_cond_wait(&(pool->notify), &(pool->lock));
        }
        
        
        /* Grab our task */
        task.function = pool->queue[pool->head].function;
        task.argument = pool->queue[pool->head].argument;
        pool->head = (pool->head + 1) % pool->queue_size;
        pool->count -= 1;
        
        /* Unlock */
        pthread_mutex_unlock(&(pool->lock));
        
        /* Get to work */
        (*(task.function))(task.argument);
    }
    
    pool->started--;
    
    pthread_mutex_unlock(&(pool->lock));
    pthread_exit(NULL);
    return(NULL);
}
