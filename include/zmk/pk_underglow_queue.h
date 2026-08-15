#pragma once

#include <zephyr/kernel.h>
#include <zmk/pk_underglow.h>

typedef enum {
    PK_UG_TASK_POWER_ON,
    PK_UG_TASK_POWER_OFF,
    PK_UG_TASK_RENDER_FRAME,
    PK_UG_TASK_SYNC_STATE,
    PK_UG_TASK_SAVE_SETTINGS
} pk_ug_task_type_t;

struct pk_ug_task {
    pk_ug_task_type_t type;
    union {
        struct {
            uint8_t layer;
        } sync;
        struct {
            bool user_initiated;
        } power;
    } payload;
};


void pk_ug_queue_push(pk_ug_task_type_t type);
void pk_ug_queue_push_power(pk_ug_task_type_t type, bool user_initiated);
void pk_ug_queue_push_sync(uint8_t layer);
int pk_ug_queue_init(void);

// Handlers implemented in pk_underglow.c
void pk_ug_task_power_on_execute(void);
void pk_ug_task_power_off_execute(void);
void pk_ug_task_render_frame_execute(void);
void pk_ug_task_sync_state_execute(uint8_t layer);
void pk_ug_task_save_settings_execute(void);
