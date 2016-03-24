#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "qemu-common.h"
#include "qemu/sockets.h"
#include "qemu/rcu.h"
#include "qemu/main-loop.h"
#include "migration/migration.h"
#include "migration/qemu-file.h"
#include "migration/migr-task.h"
#include "exec/cpu-common.h"

#define MULTI_TRY 100

#define RAM_SAVE_FLAG_COMPRESS 0x02
#define RAM_SAVE_FLAG_PAGE     0x08
#define RAM_SAVE_FLAG_CONTINUE 0x20

#define MEM_VNUM_OFFSET        6

//borrowed from savevm.c
#define QEMU_VM_EOF                  0x00
#define QEMU_VM_SECTION_START        0x01
#define QEMU_VM_SECTION_PART         0x02
#define QEMU_VM_SECTION_END          0x03
#define QEMU_VM_SECTION_FULL         0x04
#define QEMU_VM_SUBSECTION           0x05
#define QEMU_VM_ITER_END             0x07
//borrowed from block-migration.c
#define BLK_MIG_FLAG_EOS                0x02

//borrowed from arch_init.c
#define RAM_SAVE_FLAG_EOS      0x10

#define UNIX_PATH_MAX 108

typedef QLIST_HEAD(migr_handler, LoadStateEntry) migr_handler;

//static int queue_pop_task(struct migration_task_queue *task_queue, void **arg);
void *start_host_slave(void *data);
void init_host_slaves(struct MigrationState *s);

void slave_process_incoming_migration(QEMUFile *f, void *loadvm_handlers);
void *start_dest_slave(void *data);

const char *socket_paths[4]={"qmp-sock0", "qmp-sock1", "qmp-sock2", "qmp-sock3"};

const char *socket_path = "/home/jxiao/migration/clean/socket123";

static void unix_wait_for_connect2(int fd, Error *err, void *opaque)
{
    MigrationState *s = opaque;

    if (fd < 0) {
        DPRINTF("migrate connect error: %s\n", error_get_pretty(err));
        s->file = NULL;
        migrate_fd_error(s);
    } else {
        DPRINTF("migrate connect success again\n");
        s->file = qemu_fopen_socket(fd, "wb");
        if (s->file == NULL) {
            DPRINTF("could not qemu_fopen socket %s\n", __FILE__);
        }
        DPRINTF("socket opened for write%s\n", __FILE__);
    //    QEMUFile *f;
    //    f = s->file;
    //    qemu_put_byte(f, QEMU_VM_SECTION_FOOTER);
    }
}

void *start_host_slave(void *data) {
    Error *local_err = NULL;
    MigrationState *s = migrate_get_current();
    unix_nonblocking_connect(socket_path, unix_wait_for_connect2, s, &local_err);
    DPRINTF("slave terminate\n");

  struct sockaddr_un addr;
  int fd;

  if ( (fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    perror("socket error");
    exit(-1);
  }

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path)-1);

  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
    perror("connect error");
    exit(-1);
  }

    return NULL;
}

void init_host_slaves(struct MigrationState *s) {
    MigrationStateSlave *slave;
    pthread_t tid;

    slave = g_malloc0(sizeof(*slave));

    DPRINTF("start init slaves %d %s\n", NUM_SLAVES, __FILE__);
    pthread_create(&tid, NULL, start_host_slave, slave);
    DPRINTF("one slave thread is created\n");
}

static void unix_accept_incoming_migration2(void *opaque)
{
    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);
    int s = (intptr_t)opaque;
    QEMUFile *f;
    int c, err;

    DPRINTF("ready for accept %s\n", __FILE__);
    do {
        c = qemu_accept(s, (struct sockaddr *)&addr, &addrlen);
        err = errno;
    } while (c < 0 && err == EINTR);
    qemu_set_fd_handler(s, NULL, NULL, NULL);
    close(s);

    DPRINTF("accepted migration again %s\n", __FILE__);

    if (c < 0) {
        DPRINTF("could not accept migration connection (%s)", __FILE__);
        return;
    }

    f = qemu_fopen_socket(c, "rb");
    if (f == NULL) {
        DPRINTF("could not qemu_fopen socket %s\n", __FILE__);
        goto out;
    }
    DPRINTF("socket opened for read %s\n", __FILE__);
    process_incoming_migration(f);

//    qemu_get_byte(f);
    
//    uint8_t read_mark;

//    read_mark = qemu_get_byte(f);

//    if (read_mark != QEMU_VM_SECTION_FOOTER) {
//        DPRINTF("Missing section footer for %s\n", __FILE__);
//        return;
//    }

    return;

out:
    close(c);
}

struct dest_slave_para{
    const char * path;
    void *handlers;
};

void *start_dest_slave(void *data) {

    int s;

    Error *local_err = NULL;
    s = unix_listen(socket_path, NULL, 0, &local_err);
    if (s < 0) {
        return NULL;
    }

    DPRINTF("listen successful %s\n", __FILE__);
    qemu_set_fd_handler(s, unix_accept_incoming_migration2, NULL,
                        (void *)(intptr_t)s);
    DPRINTF("dest slave end\n");
    return NULL;
}

pthread_t create_dest_slave(const char *path, void *loadvm_handlers);

pthread_t create_dest_slave(const char *path, void *loadvm_handlers) {
    struct dest_slave_para *data = (struct dest_slave_para *)malloc(sizeof(struct dest_slave_para));
    pthread_t tid;

    data->path = path;
    data->handlers = loadvm_handlers;
    pthread_create(&tid, NULL, start_dest_slave, data);

    return tid;
}

/*static int queue_pop_task(struct migration_task_queue *task_queue, void **arg) {
    pthread_mutex_lock(&(task_queue->task_lock));
    if (task_queue->list_head.next == &task_queue->list_head) {
        pthread_mutex_unlock(&(task_queue->task_lock));
        return -1;
    } else {
        struct migration_task *task;
        task = (struct migration_task *)task_queue->list_head.next;
        __list_del(task->list.prev, task->list.next);

        task_queue->task_pending --;
        *arg = task->body;
        free(task);
    }
    pthread_mutex_unlock(&(task_queue->task_lock));

    return 1;
}*/

static int vmstate_load(QEMUFile *f, SaveStateEntry *se, int version_id)
{
    if (!se->vmsd) {         /* Old style */
        //classicsong change it
        if (se->ops->load_state == ram_load) {
            return se->ops->load_state(f, se, version_id);
        }

        return se->ops->load_state(f, se->opaque, version_id);
    }
    return vmstate_load_state(f, se->vmsd, se->opaque, version_id);
}

void
slave_process_incoming_migration(QEMUFile *f, void *loadvm_handlers) {
    LoadStateEntry *le;
    uint8_t section_type;
    uint32_t section_id;
    int ret;

    while ((section_type = qemu_get_byte(f)) != QEMU_VM_EOF) {
        /*
         * start modifying here tomorrow
         */
        //DPRINTF("get incomming commands %d\n", section_type);
        switch (section_type) {
        case QEMU_VM_SECTION_PART:
        case QEMU_VM_SECTION_END:
            section_id = qemu_get_be32(f);

            QLIST_FOREACH(le, (migr_handler *)loadvm_handlers, entry) {
                if (le->section_id == section_id) {
                    break;
                }
            }

            if (le == NULL) {
                fprintf(stderr, "Unknown savevm section %d\n", section_id);
                ret = -EINVAL;
                goto out;
            }

            /*
             * ram use ram_load
             * disk use block_load
             */
            ret = vmstate_load(f, le->se, le->version_id);
            if (ret < 0) {
                fprintf(stderr, "qemu: warning: error while loading state section id %d\n",
                        section_id);
                goto out;
            }
            break;
        case QEMU_VM_ITER_END:
            fprintf(stderr, "receive end\n");
            break;
        }
    }

 out:
    return;
}

