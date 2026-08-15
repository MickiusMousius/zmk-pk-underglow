#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/pk_underglow_queue.h>

LOG_MODULE_DECLARE(zmk_pk_underglow, CONFIG_ZMK_PK_UNDERGLOW_LOG_LEVEL);

#define MAX_QUEUE_SIZE 8

static struct pk_ug_task task_queue[MAX_QUEUE_SIZE];
static int queue_head = 0;

K_MUTEX_DEFINE(queue_mutex);
K_SEM_DEFINE(queue_sem, 0, K_SEM_MAX_LIMIT);

static K_THREAD_STACK_DEFINE(ug_thread_stack, 1024);
static struct k_thread ug_thread_data;

void pk_ug_queue_push(pk_ug_task_type_t type) {
    k_mutex_lock(&queue_mutex, K_FOREVER);

    // For other types, deduplicate by removing existing instances of the same type,
    // ensuring the new one is placed at the tail
    int new_head = 0;
    for (int i = 0; i < queue_head; i++) {
        if (task_queue[i].type != type) {
            task_queue[new_head++] = task_queue[i];
        }
    }
    queue_head = new_head;

    if (queue_head < MAX_QUEUE_SIZE) {
        task_queue[queue_head].type = type;
        queue_head++;
        k_sem_give(&queue_sem);
    } else {
        LOG_ERR("PK Underglow queue overflow");
    }

    k_mutex_unlock(&queue_mutex);
}

void pk_ug_queue_push_power(pk_ug_task_type_t type, bool user_initiated) {
    k_mutex_lock(&queue_mutex, K_FOREVER);

    // Deduplication and Invalidation
    if (type == PK_UG_TASK_POWER_OFF) {
        // Power off invalidates all pending actions except saving settings and sync state
        int new_head = 0;
        for (int i = 0; i < queue_head; i++) {
            if (task_queue[i].type == PK_UG_TASK_SAVE_SETTINGS || task_queue[i].type == PK_UG_TASK_SYNC_STATE) {
                task_queue[new_head++] = task_queue[i];
            }
        }
        queue_head = new_head;
    } else {
        // For other types, deduplicate by removing existing instances of the same type,
        // ensuring the new one is placed at the tail
        int new_head = 0;
        for (int i = 0; i < queue_head; i++) {
            if (task_queue[i].type != type) {
                // If it's a POWER_ON, it invalidates POWER_OFF
                if (type == PK_UG_TASK_POWER_ON && task_queue[i].type == PK_UG_TASK_POWER_OFF) {
                    continue;
                }
                task_queue[new_head++] = task_queue[i];
            }
        }
        queue_head = new_head;
    }

    if (queue_head < MAX_QUEUE_SIZE) {
        task_queue[queue_head].type = type;
        task_queue[queue_head].payload.power.user_initiated = user_initiated;
        queue_head++;
        k_sem_give(&queue_sem);
    } else {
        LOG_ERR("PK Underglow queue overflow");
    }

    k_mutex_unlock(&queue_mutex);
}


void pk_ug_queue_push_sync(uint8_t layer) {
    k_mutex_lock(&queue_mutex, K_FOREVER);

    int new_head = 0;
    for (int i = 0; i < queue_head; i++) {
        if (task_queue[i].type != PK_UG_TASK_SYNC_STATE) {
            task_queue[new_head++] = task_queue[i];
        }
    }
    queue_head = new_head;

    if (queue_head < MAX_QUEUE_SIZE) {
        task_queue[queue_head].type = PK_UG_TASK_SYNC_STATE;
        task_queue[queue_head].payload.sync.layer = layer;
        queue_head++;
        k_sem_give(&queue_sem);
    } else {
        LOG_ERR("PK Underglow queue overflow");
    }

    k_mutex_unlock(&queue_mutex);
}


static void ug_worker_thread(void *p1, void *p2, void *p3) {
    while (1) {
        k_sem_take(&queue_sem, K_FOREVER);

        k_mutex_lock(&queue_mutex, K_FOREVER);
        if (queue_head == 0) {
            k_mutex_unlock(&queue_mutex);
            continue;
        }

        // FIFO pop from index 0
        struct pk_ug_task current_task = task_queue[0];
        for (int i = 1; i < queue_head; i++) {
            task_queue[i - 1] = task_queue[i];
        }
        queue_head--;

        k_mutex_unlock(&queue_mutex);

        switch (current_task.type) {
        case PK_UG_TASK_POWER_ON:
            pk_ug_task_power_on_execute();
            if (current_task.payload.power.user_initiated) {
                pk_ug_queue_push(PK_UG_TASK_SAVE_SETTINGS);
            }
            break;
        case PK_UG_TASK_POWER_OFF:
            pk_ug_task_power_off_execute();
            if (current_task.payload.power.user_initiated) {
                pk_ug_queue_push(PK_UG_TASK_SAVE_SETTINGS);
            }
            break;
        case PK_UG_TASK_RENDER_FRAME:
            pk_ug_task_render_frame_execute();
            break;
        case PK_UG_TASK_SYNC_STATE:
            pk_ug_task_sync_state_execute(current_task.payload.sync.layer);
            break;
        case PK_UG_TASK_SAVE_SETTINGS:
            pk_ug_task_save_settings_execute();
            break;
        }
    }
}


int pk_ug_queue_init(void) {
    k_thread_create(&ug_thread_data, ug_thread_stack, K_THREAD_STACK_SIZEOF(ug_thread_stack), ug_worker_thread, NULL,
                    NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
    return 0;
}
