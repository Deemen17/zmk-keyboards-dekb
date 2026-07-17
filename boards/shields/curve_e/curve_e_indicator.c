#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/hid_indicators.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/hid_indicators_changed.h>

#if __has_include(<zmk/events/activity_state_changed.h>)
#include <zmk/events/activity_state_changed.h>
#define CURVE_E_HAS_ACTIVITY_EVENT 1
#else
#define CURVE_E_HAS_ACTIVITY_EVENT 0
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define CAPSLOCK_BIT BIT(1)

#define LED_GPIO_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(indicator_b)),
             "Alias indicator_b is required for the blue BLE LED");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(indicator_w)),
             "Alias indicator_w is required for the white caps lock LED");

static const struct device *led_dev = DEVICE_DT_GET(LED_GPIO_NODE_ID);
static const uint8_t led_blue_idx = DT_NODE_CHILD_IDX(DT_ALIAS(indicator_b));
static const uint8_t led_white_idx = DT_NODE_CHILD_IDX(DT_ALIAS(indicator_w));

static struct k_timer ble_blink_timer;
static bool ble_blink_enabled;
static int64_t ble_led_start_ts;
static bool curve_e_is_sleeping;

enum ble_led_mode {
    BLE_LED_OFF,
    BLE_LED_PAIRING,
    BLE_LED_CONNECTING,
    BLE_LED_CONNECTED,
};

static enum ble_led_mode ble_led_mode = BLE_LED_OFF;

#define BLE_LED_PAIRING_MS 120
#define BLE_LED_CONNECTING_MS 500
#define BLE_LED_CONNECTED_MS 3000
#define BLE_LED_TIMER_MS 50

static void led_blue_set(bool on) {
    if (!device_is_ready(led_dev)) {
        return;
    }

    if (on) {
        led_on(led_dev, led_blue_idx);
    } else {
        led_off(led_dev, led_blue_idx);
    }
}

static void led_white_set(bool on) {
    if (!device_is_ready(led_dev)) {
        return;
    }

    if (on) {
        led_on(led_dev, led_white_idx);
    } else {
        led_off(led_dev, led_white_idx);
    }
}

static void ble_led_timer_handler(struct k_timer *timer) {
    ARG_UNUSED(timer);

    int64_t now = k_uptime_get();
    bool on = false;

    switch (ble_led_mode) {
    case BLE_LED_PAIRING:
        on = ((now / BLE_LED_PAIRING_MS) & 0x1) != 0;
        break;
    case BLE_LED_CONNECTING:
        on = ((now / BLE_LED_CONNECTING_MS) & 0x1) != 0;
        break;
    case BLE_LED_CONNECTED:
        if ((now - ble_led_start_ts) < BLE_LED_CONNECTED_MS) {
            on = true;
        } else {
            on = false;
            k_timer_stop(&ble_blink_timer);
            ble_blink_enabled = false;
            ble_led_mode = BLE_LED_OFF;
        }
        break;
    case BLE_LED_OFF:
    default:
        on = false;
        break;
    }

    led_blue_set(on);
}

static void ble_led_set_mode(enum ble_led_mode mode) {
    if (ble_led_mode == mode) {
        return;
    }

    ble_led_mode = mode;
    ble_led_start_ts = k_uptime_get();

    if (mode == BLE_LED_OFF) {
        if (ble_blink_enabled) {
            ble_blink_enabled = false;
            k_timer_stop(&ble_blink_timer);
        }
        led_blue_set(false);
        return;
    }

    if (!ble_blink_enabled) {
        ble_blink_enabled = true;
        k_timer_start(&ble_blink_timer, K_MSEC(0), K_MSEC(BLE_LED_TIMER_MS));
    }
}

static void update_ble_status(void) {
    struct zmk_endpoint_instance selected = zmk_endpoints_selected();

    if (selected.transport == ZMK_TRANSPORT_USB) {
        ble_led_set_mode(BLE_LED_OFF);
        return;
    }

    if (zmk_ble_active_profile_is_connected()) {
        ble_led_set_mode(BLE_LED_CONNECTED);
        return;
    }

    if (zmk_ble_active_profile_is_open()) {
        ble_led_set_mode(BLE_LED_PAIRING);
        return;
    }

    bt_addr_le_t *addr = zmk_ble_active_profile_addr();
    if (addr != NULL && !bt_addr_le_eq(addr, BT_ADDR_LE_ANY)) {
        ble_led_set_mode(BLE_LED_CONNECTING);
        return;
    }

    ble_led_set_mode(BLE_LED_PAIRING);
}

static void update_caps_lock_status(void) {
    bool caps_on = (zmk_hid_indicators_get_current_profile() & CAPSLOCK_BIT) != 0;
    led_white_set(caps_on);
}

static void curve_e_set_sleep(bool sleeping) {
    curve_e_is_sleeping = sleeping;

    if (sleeping) {
        led_blue_set(false);
        led_white_set(false);
        if (ble_blink_enabled) {
            ble_blink_enabled = false;
            k_timer_stop(&ble_blink_timer);
        }
        ble_led_mode = BLE_LED_OFF;
    } else {
        update_ble_status();
        update_caps_lock_status();
    }
}

static int curve_e_ble_profile_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    update_ble_status();
    return ZMK_EV_EVENT_BUBBLE;
}

static int curve_e_endpoint_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    update_ble_status();
    return ZMK_EV_EVENT_BUBBLE;
}

static int curve_e_hid_listener(const zmk_event_t *eh) {
    ARG_UNUSED(eh);
    update_caps_lock_status();
    return ZMK_EV_EVENT_BUBBLE;
}

#if CURVE_E_HAS_ACTIVITY_EVENT
static int curve_e_activity_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

#ifdef ZMK_ACTIVITY_SLEEP
    if (ev->state == ZMK_ACTIVITY_SLEEP) {
        curve_e_set_sleep(true);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

#ifdef ZMK_ACTIVITY_ACTIVE
    if (ev->state == ZMK_ACTIVITY_ACTIVE) {
        curve_e_set_sleep(false);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    return ZMK_EV_EVENT_BUBBLE;
}
#endif

ZMK_LISTENER(curve_e_ble_profile, curve_e_ble_profile_listener);
ZMK_SUBSCRIPTION(curve_e_ble_profile, zmk_ble_active_profile_changed);

ZMK_LISTENER(curve_e_endpoint, curve_e_endpoint_listener);
ZMK_SUBSCRIPTION(curve_e_endpoint, zmk_endpoint_changed);

ZMK_LISTENER(curve_e_hid, curve_e_hid_listener);
ZMK_SUBSCRIPTION(curve_e_hid, zmk_hid_indicators_changed);

#if CURVE_E_HAS_ACTIVITY_EVENT
ZMK_LISTENER(curve_e_activity, curve_e_activity_listener);
ZMK_SUBSCRIPTION(curve_e_activity, zmk_activity_state_changed);
#endif

static int curve_e_indicator_init(const struct device *device) {
    ARG_UNUSED(device);

    if (!device_is_ready(led_dev)) {
        return -ENODEV;
    }

    k_timer_init(&ble_blink_timer, ble_led_timer_handler, NULL);
    update_ble_status();
    update_caps_lock_status();
    return 0;
}

SYS_INIT(curve_e_indicator_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
