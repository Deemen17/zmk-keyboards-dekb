#include "defb.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/sensor_event.h>
#include <zmk/events/keycode_state_changed.h>

LOG_MODULE_REGISTER(defb_event, CONFIG_ZMK_LOG_LEVEL);

// ====================== CONFIG ======================

#define DEFB_EVENT_DEBOUNCE_MS 100

// ====================== FORWARD CORE API ======================

extern void defb_event_output_changed(bool is_usb);
extern void defb_event_ble_connected(void);
extern void defb_event_activity(bool active);

// ====================== INTERNAL STATE ======================

static int64_t last_event_ts = 0;

// ====================== HELPER ======================

static bool debounce_ok(void) {
    int64_t now = k_uptime_get();

    if (now - last_event_ts < DEFB_EVENT_DEBOUNCE_MS) {
        return false;
    }

    last_event_ts = now;
    return true;
}

// ====================== LISTENER ======================

static int defb_event_listener(const zmk_event_t *eh) {

    // ===== OUTPUT CHANGE (USB / BLE) =====
    const struct zmk_endpoint_changed *ep =
        as_zmk_endpoint_changed(eh);

    if (ep) {
        bool is_usb = (ep->endpoint.transport == ZMK_TRANSPORT_USB);

        LOG_DBG("DEFB: endpoint changed → %s", is_usb ? "USB" : "BLE");

        defb_event_output_changed(is_usb);

        return ZMK_EV_EVENT_BUBBLE;
    }

    // ===== BLE PROFILE =====
    const struct zmk_ble_active_profile_changed *ble =
        as_zmk_ble_active_profile_changed(eh);

    if (ble) {
        if (!debounce_ok()) {
            return ZMK_EV_EVENT_BUBBLE;
        }

        LOG_DBG("DEFB: BLE profile changed");

        defb_event_ble_connected();

        return ZMK_EV_EVENT_BUBBLE;
    }

    // ===== ACTIVITY (SLEEP / WAKE) =====
    const struct zmk_activity_state_changed *act =
        as_zmk_activity_state_changed(eh);

    if (act) {

#ifdef ZMK_ACTIVITY_SLEEP
        if (act->state == ZMK_ACTIVITY_SLEEP) {
            LOG_DBG("DEFB: sleep");
            defb_event_activity(false);
            return ZMK_EV_EVENT_BUBBLE;
        }
#endif

#ifdef ZMK_ACTIVITY_ACTIVE
        if (act->state == ZMK_ACTIVITY_ACTIVE) {
            LOG_DBG("DEFB: wake");
            defb_event_activity(true);
            return ZMK_EV_EVENT_BUBBLE;
        }
#endif
    }

    // ===== USER ACTIVITY (KEY / SENSOR) =====
    if (as_zmk_position_state_changed(eh) ||
        as_zmk_sensor_event(eh)) {

        defb_event_activity(true);
        return ZMK_EV_EVENT_BUBBLE;
    }

    // ===== KEYCODE (OPTIONAL EXTENSION) =====
    const struct zmk_keycode_state_changed *kc =
        as_zmk_keycode_state_changed(eh);

    if (kc && kc->state) {
        // future: custom keycode hook
        // e.g. trigger output check, battery check...
    }

    return ZMK_EV_EVENT_BUBBLE;
}

// ====================== ZMK REGISTRATION ======================

ZMK_LISTENER(defb_event_listener, defb_event_listener);

ZMK_SUBSCRIPTION(defb_event_listener, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(defb_event_listener, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(defb_event_listener, zmk_activity_state_changed);
ZMK_SUBSCRIPTION(defb_event_listener, zmk_position_state_changed);
ZMK_SUBSCRIPTION(defb_event_listener, zmk_sensor_event);
ZMK_SUBSCRIPTION(defb_event_listener, zmk_keycode_state_changed);