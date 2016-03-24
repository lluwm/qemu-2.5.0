#ifndef MIGR_TASK_H
#define MIGR_TASK_H

#include <stdio.h>
#include <pthread.h>

#include "block/block.h"
#include "qemu/atomic.h"

#define SLEEP_SHORT_TIME 1000

#define MIGR_DISK_TASK 0
#define MIGR_MEM_TASK 1

#define TASK_TYPE_MEM 0
#define TASK_TYPE_DISK 1

//#define NUM_SLAVES 4
#define FAKE_IP 123

#define DEFAULT_MEM_BATCH_SIZE (2 * 1024 * 1024) //2M
#define DEFAULT_DISK_BATCH_SIZE (8 * 1024 * 1024) //8M

#define MAX_DATA_BUF (512 * 1024 * 1024) //512M
#define MAX_TASK_PENDING (MAX_DATA_BUF/DEFAULT_DISK_BATCH_SIZE)

#define TARGET2_PAGE_BITS 12
#define TARGET2_PAGE_SIZE (1 << TARGET2_PAGE_BITS)
#define TARGET2_PAGE_MASK ~(TARGET2_PAGE_SIZE - 1)

#define DEFAULT_MEM_BATCH_LEN (DEFAULT_MEM_BATCH_SIZE/(1 << 12))

struct task_body {
    int type;
    int len;
    struct {
            uint8_t *ptr;
            unsigned long addr;
            void *block;
        } pages[DEFAULT_MEM_BATCH_LEN];
    int iter_num;
};

struct linked_list {
    struct linked_list *next;
    struct linked_list *prev;
};

static inline void INIT_LIST_HEAD(struct linked_list *list) {
    list->next = list;
    list->prev = list;
}

static inline void __list_add(struct linked_list *new, struct linked_list *prev, struct linked_list *next) {
    next->prev = new;
    new->next = next;
    new->prev = prev;
    prev->next = new;
}

static inline void list_add_tail(struct linked_list *new, struct linked_list *head) {
    __list_add(new, head->prev, head);
}

static inline void __list_del(struct linked_list *prev, struct linked_list *next) {
    next->prev = prev;
    prev->next = next;
}

#define BARR_STATE_ITER_ERR 0
#define BARR_STATE_ITER_START 1
#define BARR_STATE_ITER_END 2
#define BARR_STATE_SKIP 3
#define BARR_STATE_ITER_TERMINATE 4

struct migration_barrier {
    volatile int mem_state;
    volatile int disk_state;
    pthread_barrier_t sender_iter_barr;
    pthread_barrier_t next_iter_barr;
    pthread_mutex_t master_lock;
};

struct migration_task {
    struct linked_list list;
    void *body;
};

struct disk_task {
    void *bs;
    int64_t addr;
    void *buf;
    int nr_sectors;
};

struct banner {
    pthread_barrier_t end_barrier;
    int slave_done;
    volatile int end;
};

struct migration_task_queue {
    struct linked_list list_head;
    pthread_mutex_t task_lock;
    union {
        int section_id;
        int nr_slaves;
    };
    int force_end;
    int iter_num;
    unsigned long data_remaining;
    unsigned long sent_this_iter;
    unsigned long sent_last_iter;
    volatile int task_pending;
    double bwidth;
    unsigned long slave_sent[32];
};

struct migration_slave{
    struct migration_slave *next;
    int slave_id;
};

#endif
