#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/pk_underglow.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* PERIPHERAL SIDE: GATT Server */

#define PK_UUID_SVC BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)
#define PK_UUID_CHR BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1)

static struct bt_uuid_128 pk_svc_uuid = BT_UUID_INIT_128(PK_UUID_SVC);
static struct bt_uuid_128 pk_chr_uuid = BT_UUID_INIT_128(PK_UUID_CHR);

static ssize_t write_layer(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len,
                           uint16_t offset, uint8_t flags) {
    if (len != 1)
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    uint8_t layer = ((uint8_t *)buf)[0];
    LOG_DBG("Received layer from central: %d", layer);
    zmk_pk_underglow_set_peripheral_layer(layer);
    return len;
}

BT_GATT_SERVICE_DEFINE(pk_svc, BT_GATT_PRIMARY_SERVICE(&pk_svc_uuid),
                       BT_GATT_CHARACTERISTIC(&pk_chr_uuid.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                                              BT_GATT_PERM_WRITE_ENCRYPT, NULL, write_layer, NULL));

#else

/* CENTRAL SIDE: GATT Client */
/* A full implementation would perform GATT discovery and write to the peripheral.
   For brevity in this module, this is stubbed. */

int zmk_pk_split_sync_send_layer(uint8_t layer) {
    LOG_DBG("Sending layer %d to peripheral (stubbed)", layer);
    return 0;
}

#endif
