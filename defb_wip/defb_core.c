#include "defb.h"
#include "defb_indicator.h"
#include "defb_buzzer.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(defb_core, CONFIG_ZMK_LOG_LEVEL);

// ====================== CONFIG ======================

#define DEFB_UPDATE_INTERVAL_MS 50
#define DEFB_EVENT_TIMEOUT_MS   1200
#define DEFB_PRE_SLEEP_MS       300

// ====================== STATE ======================

typedef enum {
    DEFB_STATE_IDLE,
    DEFB_STATE_SLEEP,
    DEFB_STATE_BOOT,

    DEFB_STATE_OUTPUT_USB,
    DEFB_STATE_OUTPUT_BLE,
    DEFB_STATE_OUTPUT_CHECK,

    DEFB_STATE_BLE_PAIRING,
    DEFB_STATE_BLE_CONNECTING,
    DEFB_STATE_BLE_CONNECTED,
    DEFB_STATE_BLE_SWITCH,

    DEFB_STATE_BATTERY_LOW,
    DEFB_STATE_BATTERY_CRITICAL,

    DEFB_STATE_LAYER,
    DEFB_STATE_CAPSLOCK,

} defb_state_t;

// ====================== CONTEXT ======================

typedef struct {

    struct {
        bool sleeping;
        bool booting;
    } system;

    struct {
        bool output_check;
        bool output_usb;
        bool ble_switch;
        bool layer_changed;
    } event;

    struct {
        bool usb_active;

        bool ble_connected;
        bool ble_connecting;
        bool ble_pairing;

        bool capslock;

        uint8_t battery_percent;
        uint8_t layer;
    } state;

    struct {
        int64_t now;

        int64_t last_activity;
        int64_t state_entered;

        int64_t event_ts;
    } time;

    defb_state_t current;
    defb_state_t last;

} defb_ctx_t;

static defb_ctx_t ctx;

// ====================== STATE RESOLVE ======================

static defb_state_t resolve_state(void) {

    if (ctx.system.sleeping)
        return DEFB_STATE_SLEEP;

    if (ctx.system.booting)
        return DEFB_STATE_BOOT;

    if (ctx.state.battery_percent <= 10)
        return DEFB_STATE_BATTERY_CRITICAL;

    if (ctx.state.battery_percent <= 20)
        return DEFB_STATE_BATTERY_LOW;

    if (ctx.event.output_usb)
        return DEFB_STATE_OUTPUT_USB;

    if (ctx.event.output_check)
        return DEFB_STATE_OUTPUT_CHECK;

    if (ctx.event.ble_switch)
        return DEFB_STATE_BLE_SWITCH;

    if (ctx.state.ble_pairing)
        return DEFB_STATE_BLE_PAIRING;

    if (ctx.state.ble_connecting)
        return DEFB_STATE_BLE_CONNECTING;

    if (ctx.state.ble_connected)
        return DEFB_STATE_BLE_CONNECTED;

    if (ctx.event.layer_changed)
        return DEFB_STATE_LAYER;

    if (ctx.state.capslock)
        return DEFB_STATE_CAPSLOCK;

    return DEFB_STATE_IDLE;
}

// ====================== EVENT TIMEOUT ======================

static void update_event_timeout(void) {
    if (ctx.event.output_check &&
        ctx.time.now - ctx.time.event_ts > DEFB_EVENT_TIMEOUT_MS) {

        ctx.event.output_check = false;
        ctx.event.output_usb = false;
        ctx.event.ble_switch = false;
        ctx.event.layer_changed = false;
    }
}

// ====================== RENDER ======================

static void render(bool changed) {

    defb_indicator_render(ctx.current);

    if (changed) {
        defb_buzzer_render(ctx.current);
    }
}

// ====================== UPDATE LOOP ======================

static void defb_update(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(defb_work, defb_update);

static void defb_update(struct k_work *work) {

    ctx.time.now = k_uptime_get();

    update_event_timeout();

    defb_state_t next = resolve_state();

    bool changed = (next != ctx.current);

    if (changed) {
        ctx.last = ctx.current;
        ctx.current = next;
        ctx.time.state_entered = ctx.time.now;
    }

    render(changed);

    k_work_schedule(&defb_work, K_MSEC(DEFB_UPDATE_INTERVAL_MS));
}

// ====================== PUBLIC EVENT API ======================

void defb_event_output_changed(bool is_usb) {
    ctx.state.usb_active = is_usb;

    ctx.event.output_check = true;
    ctx.event.output_usb = is_usb;

    ctx.time.event_ts = k_uptime_get();
}

void defb_event_ble_connected(void) {
    ctx.state.ble_connected = true;
    ctx.state.ble_connecting = false;
    ctx.state.ble_pairing = false;
}

void defb_event_activity(bool active) {
    ctx.time.last_activity = k_uptime_get();

    ctx.system.sleeping = !active;
}

// ====================== INIT ======================

void defb_core_init(void) {
    memset(&ctx, 0, sizeof(ctx));

    ctx.system.booting = true;
    ctx.time.state_entered = k_uptime_get();

    k_work_schedule(&defb_work, K_MSEC(DEFB_UPDATE_INTERVAL_MS));
}