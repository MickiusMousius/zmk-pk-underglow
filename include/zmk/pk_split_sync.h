#pragma once

#include <zephyr/types.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_ROLE_CENTRAL) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
int zmk_pk_split_sync_send_layer(uint8_t layer);
#endif
