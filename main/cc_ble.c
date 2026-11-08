// main/cc_ble.c — see cc_ble.h.
#include "cc_ble.h"

#include <stdio.h>
#include <string.h>

#include "cc_store.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "cc_ble";

// Provided by NimBLE's NVS-backed store (CONFIG_BT_NIMBLE_NVS_PERSIST=y); ESP-IDF
// ships no public header for it.
void ble_store_config_init(void);

// 9aa10001-aa98-4d97-a56d-233f9c782706 (service), ...0002 (RX), ...0003 (TX).
// NimBLE stores 128-bit UUIDs little-endian. tests/test_cc_companion.py checks
// these against tools/cc_companion/ccp.py.
static const ble_uuid128_t SVC_UUID =
    BLE_UUID128_INIT(0x06, 0x27, 0x78, 0x9c, 0x3f, 0x23, 0x6d, 0xa5, 0x97, 0x4d, 0x98, 0xaa,
                     0x01, 0x00, 0xa1, 0x9a);
static const ble_uuid128_t RX_UUID =
    BLE_UUID128_INIT(0x06, 0x27, 0x78, 0x9c, 0x3f, 0x23, 0x6d, 0xa5, 0x97, 0x4d, 0x98, 0xaa,
                     0x02, 0x00, 0xa1, 0x9a);
static const ble_uuid128_t TX_UUID =
    BLE_UUID128_INIT(0x06, 0x27, 0x78, 0x9c, 0x3f, 0x23, 0x6d, 0xa5, 0x97, 0x4d, 0x98, 0xaa,
                     0x03, 0x00, 0xa1, 0x9a);

#define RX_SCRATCH 512

static cc_ble_rx_fn s_rx;
static void *s_rx_ctx;
static uint8_t s_rx_scratch[RX_SCRATCH];   // NimBLE host task only
static uint16_t s_tx_handle;
static uint8_t s_own_addr_type;
static char s_name[24];

static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_subscribed;
static volatile bool s_secure;
static volatile uint16_t s_mtu = 23;
static volatile uint32_t s_generation;
static volatile int64_t s_pair_until_us;

static int gap_event(struct ble_gap_event *event, void *arg);

// ---------------------------------------------------------------------------
// Link status helpers (run under the store lock)
// ---------------------------------------------------------------------------
typedef struct {
    bool connected;
    bool secure;
    bool passkey_active;
    uint32_t passkey;
    bool clear_data;
    int pairing_blocked;  // 1 set, 0 clear, -1 keep (stays visible after the refused link drops)
    int bonds;            // -1 = unchanged
    bool started;
    bool failed;
    bool pair_window;
} link_patch_t;

static void apply_patch(cc_link_info_t *link, void *arg)
{
    const link_patch_t *p = arg;
    link->connected = p->connected;
    link->secure = p->secure;
    link->passkey_active = p->passkey_active;
    link->passkey = p->passkey;
    if (p->clear_data) link->have_data = false;
    if (p->pairing_blocked >= 0) link->pairing_blocked = p->pairing_blocked != 0;
    if (p->bonds >= 0) link->bonds = (uint8_t)p->bonds;
    link->ble_started = p->started;
    link->ble_failed = p->failed;
    link->pair_window = p->pair_window;
}

static int bond_count(void)
{
    int count = 0;
    if (ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &count) != 0) return -1;
    return count;
}

// Live link security from NimBLE. Decisions use this instead of a cached flag:
// when the Mac restores encryption from its stored bond while our own security
// request is pending, NimBLE can report the request as failed or timed out even
// though the link is encrypted, and a cached flag would then stay wrong.
static bool link_secure(uint16_t conn)
{
    struct ble_gap_conn_desc desc;
    if (conn == BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_find(conn, &desc) != 0) return false;
    // LE Secure Connections with MITM protection (passkey) and a full-size key.
    return desc.sec_state.encrypted && desc.sec_state.authenticated && desc.sec_state.key_size >= 16;
}

static bool pair_window_open(void)
{
    return esp_timer_get_time() < s_pair_until_us;
}

static void publish(bool passkey_active, uint32_t passkey, int pairing_blocked, bool clear_data)
{
    link_patch_t p = {
        .connected = s_conn != BLE_HS_CONN_HANDLE_NONE,
        .secure = s_secure,
        .passkey_active = passkey_active,
        .passkey = passkey,
        .clear_data = clear_data,
        .pairing_blocked = pairing_blocked,
        .bonds = bond_count(),
        .started = true,
        .failed = false,
        .pair_window = pair_window_open(),
    };
    cc_store_link_update(apply_patch, &p);
}

// ---------------------------------------------------------------------------
// GATT
// ---------------------------------------------------------------------------
static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return 0;   // TX has no readable value; reads exist only to gate on encryption
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (conn_handle != s_conn || !link_secure(conn_handle)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if (!s_secure) {
        s_secure = true;   // the link was secured without a successful ENC_CHANGE report
        publish(false, 0, 0, false);
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > sizeof(s_rx_scratch)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint16_t copied = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, s_rx_scratch, sizeof(s_rx_scratch), &copied) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (s_rx && !s_rx(s_rx_scratch, copied, s_rx_ctx)) return BLE_ATT_ERR_INSUFFICIENT_RES;
    return 0;
}

static const struct ble_gatt_svc_def SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &RX_UUID.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                         BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN,
            },
            {
                .uuid = &TX_UUID.u,
                .access_cb = chr_access,
                .val_handle = &s_tx_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN |
                         BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC |
                         BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN,
            },
            { 0 },
        },
    },
    { 0 },
};

// ---------------------------------------------------------------------------
// Advertising
// ---------------------------------------------------------------------------
static void advertise(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) return;

    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &SVC_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields: %d", rc);
        return;
    }
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (const uint8_t *)s_name;
    rsp.name_len = (uint8_t)strlen(s_name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan response: %d", rc);
        return;
    }
    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // 200-300 ms: the Mac finds the device within a second or two without
    // spending much battery on advertising while it is away.
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(200);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(300);
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv start: %d", rc);
}

static void update_security(uint16_t conn_handle, int status)
{
    struct ble_gap_conn_desc desc;
    s_secure = link_secure(conn_handle);
    if (ble_gap_conn_find(conn_handle, &desc) == 0) {
        ESP_LOGI(TAG, "security (status %d): enc=%d auth=%d bonded=%d key=%d", status,
                 desc.sec_state.encrypted, desc.sec_state.authenticated, desc.sec_state.bonded,
                 desc.sec_state.key_size);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            advertise();
            return 0;
        }
        s_conn = event->connect.conn_handle;
        s_secure = false;
        s_subscribed = false;
        s_mtu = 23;
        s_generation++;
        ESP_LOGI(TAG, "connected (handle %d)", s_conn);
        publish(false, 0, -1, true);
        // Ask the central to encrypt now: bonded Macs resume silently, new ones pair.
        (void)ble_gap_security_initiate(s_conn);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason 0x%x)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_secure = false;
        s_subscribed = false;
        s_generation++;
        publish(false, 0, -1, true);
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.conn_handle == s_conn) {
            // Trust the link's actual state, not the procedure's status (see link_secure()).
            update_security(s_conn, event->enc_change.status);
            publish(false, 0, s_secure ? 0 : -1, false);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.conn_handle == s_conn && event->subscribe.attr_handle == s_tx_handle) {
            s_subscribed = event->subscribe.cur_notify != 0;
        }
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        if (event->passkey.conn_handle != s_conn ||
            event->passkey.params.action != BLE_SM_IOACT_DISP) {
            return BLE_HS_ENOTSUP;
        }
        if (bond_count() > 0 && !pair_window_open()) {
            // Already paired with a Mac: refuse strangers unless the user opened the window.
            ESP_LOGW(TAG, "pairing refused: open Settings > Pair new Mac first");
            publish(false, 0, 1, false);
            (void)ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        struct ble_sm_io io = {
            .action = BLE_SM_IOACT_DISP,
            .passkey = esp_random() % 1000000u,
        };
        publish(true, io.passkey, -1, false);
        return ble_sm_inject_io(event->passkey.conn_handle, &io);
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // The Mac lost its keys but this device still has them. Only start over
        // when the user explicitly allowed pairing.
        if (!pair_window_open()) {
            publish(false, 0, 1, false);
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) != 0) {
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        (void)ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Host lifecycle
// ---------------------------------------------------------------------------
static void on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset: %d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "address setup failed: %d", rc);
        return;
    }
    publish(false, 0, -1, false);
    advertise();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();   // returns only if the host is stopped, which this app never does
    vTaskDelete(NULL);
}

esp_err_t cc_ble_start(cc_ble_rx_fn rx, void *ctx)
{
    s_rx = rx;
    s_rx_ctx = ctx;

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_name, sizeof(s_name), "CC-Companion-%02X%02X", mac[4], mac[5]);
    cc_store_set_device_name(s_name);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_sc_only = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(SERVICES);
    if (rc == 0) rc = ble_gatts_add_svcs(SERVICES);
    if (rc == 0) rc = ble_svc_gap_device_name_set(s_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT setup failed: %d", rc);
        return ESP_FAIL;
    }
    ble_store_config_init();

    if (xTaskCreate(host_task, "nimble_host", NIMBLE_HS_STACK_SIZE, NULL,
                    configMAX_PRIORITIES - 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "host task creation failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "advertising as %s", s_name);
    return ESP_OK;
}

esp_err_t cc_ble_send(const char *data, size_t len)
{
    uint16_t conn = s_conn;
    if (!s_subscribed || !link_secure(conn)) return ESP_ERR_INVALID_STATE;
    size_t chunk = s_mtu > 3 ? (size_t)s_mtu - 3 : 20;
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = len - off < chunk ? len - off : chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data + off, (uint16_t)n);
        if (!om) return ESP_ERR_NO_MEM;
        int rc = ble_gatts_notify_custom(conn, s_tx_handle, om);   // consumes om
        if (rc != 0) return ESP_FAIL;
    }
    return ESP_OK;
}

void cc_ble_note_bridge_subscribed(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) s_subscribed = true;
}

uint32_t cc_ble_generation(void)
{
    return s_generation;
}

void cc_ble_open_pair_window(uint32_t seconds)
{
    s_pair_until_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    publish(false, 0, 0, false);
}

esp_err_t cc_ble_forget_all(void)
{
    (void)ble_gap_adv_stop();
    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        for (int i = 0; i < 30 && s_conn != BLE_HS_CONN_HANDLE_NONE; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    (void)ble_gap_adv_stop();   // the disconnect handler restarts advertising
    int rc = ble_store_clear();
    ESP_LOGI(TAG, "bonds cleared: %d", rc);
    publish(false, 0, 0, true);
    advertise();
    return rc == 0 ? ESP_OK : ESP_FAIL;
}
