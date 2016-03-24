#include <stdio.h>
#include <signal.h>

#include "qemu-common.h"
#include "qemu/sockets.h"
#include "qemu/timer.h"
#include "migration/migration.h"
#include "migration/migr-task.h"
#include "sysemu/sysemu.h"
#include "block/block.h"
#include "hw/hw.h"

#define TARGET_PHYS_ADDR_MAX UINT64_MAX
#define TARGET_PHYS_ADDR_BITS 64

#define RAM_SAVE_FLAG_CONTINUE 0x20

#define MIGRATION_DIRTY_FLAG 0x08

#define DIRTY_MEMORY_NUM       3

void * host_memory_master(void *data);
void create_host_memory_master(void *opaque);
unsigned long ram_save_iter(int stage, struct migration_task_queue *task_queue, QEMUFile *f);

//borrowed from savevm.c
#define QEMU_VM_EOF                  0x00
#define QEMU_VM_SECTION_START        0x01
#define QEMU_VM_SECTION_PART         0x02
#define QEMU_VM_SECTION_END          0x03
#define QEMU_VM_SECTION_FULL         0x04
#define QEMU_VM_SUBSECTION           0x05

unsigned long
ram_save_iter(int stage, struct migration_task_queue *task_queue, QEMUFile *f) {
    unsigned long bytes_transferred = 0;

    if (stage == 3) {
        /* flush all remaining blocks regardless of rate limiting */
        bytes_transferred = ram_save_block_master(task_queue);
        DPRINTF("Total memory sent last iter %lx\n", bytes_transferred);
    } else {
        /* try transferring iterative blocks of memory */
        bytes_transferred = ram_save_block_master(task_queue);
    }

    return bytes_transferred;
}

void * host_memory_master(void *data) {
    MigrationState *s = migrate_get_current();
    unsigned long total_sent = 0;
    unsigned long memory_size = ram_bytes_total();
    unsigned long data_remaining;
    double bwidth;
    QEMUFile *f = s->file;
    int iter_num = 0;
    int hold_lock = 0;
    sigset_t set;
    int i;
    unsigned long ret;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    sigaddset(&set, SIGIO);
    sigaddset(&set, SIGALRM);
    sigprocmask(SIG_BLOCK, &set, NULL);

    DPRINTF("Start memory master\n");

    /*
     * wait for all slaves and master to be ready
     */
    s->mem_task_queue->sent_last_iter = memory_size;
    s->sender_barr->mem_state = BARR_STATE_ITER_START;

    DPRINTF("Start processing memory, %lx\n", s->mem_task_queue->sent_last_iter);

    do {
        bwidth = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

        DPRINTF("Start mem iter %d\n", s->mem_task_queue->iter_num);
        /*
         * classicsong
         * dispatch job here
         * ram_save_iter will
         */
        ram_save_iter(QEMU_VM_SECTION_PART, s->mem_task_queue, s->file);

     skip_iter:
        /*
         * add barrier here to sync for iterations
         */
        s->sender_barr->mem_state = BARR_STATE_ITER_END;
        hold_lock = !pthread_mutex_trylock(&s->sender_barr->master_lock);

        /*
         * sync_dirty_bitmap in iteration for the next iter
         * the sync operation
         * 1. invoke ioctl KVM_GET_DIRTY_LOG to get dirty bitmap
         *    a. get dirty bitmap
         *    b. reset dirty bit in page table
         * 2. copy the dirty bitmap to user bitmap KVMDirtyLog d.dirty_bitmap
         *    a. KVMDirtyLog.dirty_bitmap is local
         * 3. copy the dirty_bitmap to a global bitmap using cpu_physical_memory_set_dirty that is
         *    modifying ram_list.phys_dirty
         * Thus calling cpu_physical_sync_dirty_bitmap will not clean the ram_list.phys_dirty
         *   The dirty flag is reset by cpu_physical_memory_reset_dirty(va, vb, MIGRATION_DIRTY_FLAG)
         */
//        if (cpu_physical_sync_dirty_bitmap(0, TARGET_PHYS_ADDR_MAX) != 0) {
            ret = -1;
            fprintf(stderr, "get dirty bitmap error\n");
            qemu_file_set_error(f,ret);
            return (void *)ret;
//        }

        s->mem_task_queue->sent_this_iter = 0;
        for ( i = 0; i < NUM_SLAVES; i++) {
            s->mem_task_queue->sent_this_iter += s->mem_task_queue->slave_sent[i];
            s->mem_task_queue->slave_sent[i] = 0;
        }

        bwidth = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - bwidth;
        DPRINTF("Mem send this iter %lx, bwidth %f\n", s->mem_task_queue->sent_this_iter, bwidth/1000000);
        bwidth = s->mem_task_queue->sent_this_iter / bwidth;

        data_remaining = ram_bytes_remaining();
        total_sent += s->mem_task_queue->sent_this_iter;

        if ((s->mem_task_queue->iter_num >= MAX_ITER) ||
            (total_sent > MAX_FACTOR * memory_size))
            s->mem_task_queue->force_end = 1;

        s->mem_task_queue->bwidth = bwidth;
        s->mem_task_queue->data_remaining = data_remaining;

        if (hold_lock) {
            /*
             * get lock fill memory info
             */
            DPRINTF("Iter [%d:%d], memory_remain %lx, bwidth %f\n",
                    s->mem_task_queue->iter_num, iter_num,
                    data_remaining, bwidth);
            pthread_mutex_unlock(&s->sender_barr->master_lock);
        }
        else {
            uint64_t total_expected_downtime;
            uint64_t sent_this_iter;
            uint64_t sent_last_iter;
            /*
             * failed to get lock first
             * check for disk info
             */
            pthread_mutex_lock(&s->sender_barr->master_lock);

            total_expected_downtime = (s->mem_task_queue->data_remaining + s->disk_task_queue->data_remaining)/
                (s->mem_task_queue->bwidth + s->disk_task_queue->bwidth);
            sent_this_iter = s->mem_task_queue->sent_this_iter + s->disk_task_queue->sent_this_iter;
            sent_last_iter = s->mem_task_queue->sent_last_iter + s->disk_task_queue->sent_last_iter;

            DPRINTF("Total Iter [%d:%d], data_remain %lx, bwidth %f\n", s->mem_task_queue->iter_num, iter_num,
                    s->mem_task_queue->data_remaining + s->disk_task_queue->data_remaining,
                    s->mem_task_queue->bwidth + s->disk_task_queue->bwidth);

            DPRINTF("Sent this iter %lx, sent last iter %lx, expect downtime %ld ns\n",
                    sent_this_iter, sent_last_iter, total_expected_downtime);

            if (total_expected_downtime < MAX_DOWNTIME ||
                sent_this_iter > sent_last_iter ||
                s->disk_task_queue->force_end == 1 ||
                s->mem_task_queue->force_end == 1)
                s->laster_iter =1;
            pthread_mutex_unlock(&s->sender_barr->master_lock);
        }

        //set last iter and reset this iter
        s->mem_task_queue->sent_last_iter = s->mem_task_queue->sent_this_iter;
        s->mem_task_queue->sent_this_iter = 0;
        //start the next iteration for slaves
        s->sender_barr->mem_state = BARR_STATE_ITER_START;

        //total iteration number count
        iter_num++;

        /*
         * if the data left to send is small enough
         *    and the iteration is not the last iteration
         * skip the next mem iteration
         */
        if (((data_remaining/(s->mem_task_queue->bwidth + s->disk_task_queue->bwidth)) < (MAX_DOWNTIME/2)) && s->laster_iter != 1) {
            bwidth = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            goto skip_iter;
        }
        /*
         * if skip the iteration
         * the iteration number of memory is not increased
         */
        s->mem_task_queue->iter_num ++;
    } while (s->laster_iter != 1);

    DPRINTF("Done mem iterating\n");

    //last iteration
    bwidth = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    //need to resync dirty after the VM is paused
//    if (cpu_physical_sync_dirty_bitmap(0, TARGET_PHYS_ADDR_MAX) != 0) {
        ret = -1;
        fprintf(stderr, "get dirty bitmap error\n");
        qemu_file_set_error(f,ret);
        return (void *)ret;
//    }

//    ram_save_iter(QEMU_VM_SECTION_END, s->mem_task_queue, s->file);

    //wait for slave end
    s->sender_barr->mem_state = BARR_STATE_ITER_TERMINATE;
    //last iteration end
    DPRINTF("last iteration time %f\n", (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - bwidth)/1000000);

    DPRINTF("Mem master end\n");

    return NULL;
}

void 
create_host_memory_master(void *opaque) {
    struct MigrationState *s = (struct MigrationState *)opaque;
    pthread_t tid;
    struct migration_master *master;
    s->mem_task_queue->section_id = s->section_id;
    pthread_create(&tid, NULL, host_memory_master, s);

    master = (struct migration_master *)malloc(sizeof(struct migration_master));
    master->type = MEMORY_MASTER;
    master->tid = tid;
    master->next = s->master_list;
    s->master_list = master;
}

