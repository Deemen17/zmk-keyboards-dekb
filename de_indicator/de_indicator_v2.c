#include "de_indicator_v2.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/ble.h>
#include <zmk/battery.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/hid_indicators.h>
#include <zmk/keymap.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/keycode_state_changed.h>


#if __has_include(<zmk/events/activity_state_changed.h>)
#include <zmk/events/activity_state_changed.h>
#define DE_INDICATOR_HAS_ACTIVITY_EVENT 1
#else
#define DE_INDICATOR_HAS_ACTIVITY_EVENT 0
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define CAPSLOCK_BIT BIT(1)

#define LED_GPIO_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(indicator_r)),
             "An alias for a red LED is not found for RGBLED_WIDGET");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(indicator_g)),
             "An alias for a green LED is not found for RGBLED_WIDGET");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(indicator_b)),
             "An alias for a blue LED is not found for RGBLED_WIDGET");

static const struct device *led_dev = DEVICE_DT_GET(LED_GPIO_NODE_ID);
static const uint8_t led_idx[] = {
    DT_NODE_CHILD_IDX(DT_ALIAS(indicator_r)),
    DT_NODE_CHILD_IDX(DT_ALIAS(indicator_g)),
    DT_NODE_CHILD_IDX(DT_ALIAS(indicator_b)),
};

static const struct gpio_dt_spec led_gpio[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(indicator_r), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(indicator_g), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(indicator_b), gpios),
};

// =====================
// CONFIG
// =====================

#ifndef DE_INDICATOR_BLINK_FAST_MS
#define DE_INDICATOR_BLINK_FAST_MS 120
#endif

#ifndef DE_INDICATOR_BLINK_SLOW_MS
#define DE_INDICATOR_BLINK_SLOW_MS 500
#endif

#ifndef DE_INDICATOR_BATTERY_CHECK_MS
#define DE_INDICATOR_BATTERY_CHECK_MS 1500
#endif

#ifndef DE_INDICATOR_OUTPUT_CHECK_MS
#define DE_INDICATOR_OUTPUT_CHECK_MS 800
#endif

#ifndef DE_INDICATOR_BLE_SWITCH_MS
#define DE_INDICATOR_BLE_SWITCH_MS 500
#endif

#ifndef DE_INDICATOR_BLE_CONNECTED_MS
#define DE_INDICATOR_BLE_CONNECTED_MS 3000
#endif

#ifndef DE_INDICATOR_BLE_PAIRING_TIMEOUT_MS
#define DE_INDICATOR_BLE_PAIRING_TIMEOUT_MS 30000
#endif

#ifndef DE_INDICATOR_BLE_CONNECTING_TIMEOUT_MS
#define DE_INDICATOR_BLE_CONNECTING_TIMEOUT_MS 30000
#endif

#ifndef DE_INDICATOR_LAYER_EVENT_MS
#define DE_INDICATOR_LAYER_EVENT_MS 600
#endif

#ifndef DE_INDICATOR_BOOT_MS
#define DE_INDICATOR_BOOT_MS 840
#endif

#ifndef DE_INDICATOR_BATTERY_LOW_INTERVAL_MS
#define DE_INDICATOR_BATTERY_LOW_INTERVAL_MS 60000
#endif

#ifndef DE_INDICATOR_BATTERY_CRITICAL_INTERVAL_MS
#define DE_INDICATOR_BATTERY_CRITICAL_INTERVAL_MS 30000
#endif

#ifndef DE_INDICATOR_STATE_MIN_HOLD_MS
#define DE_INDICATOR_STATE_MIN_HOLD_MS 120
#endif

#ifndef DE_INDICATOR_ENDPOINT_DEBOUNCE_MS
#define DE_INDICATOR_ENDPOINT_DEBOUNCE_MS 120
#endif

#ifndef DE_INDICATOR_BLE_EVENT_DEBOUNCE_MS
#define DE_INDICATOR_BLE_EVENT_DEBOUNCE_MS 120
#endif

#ifndef DE_INDICATOR_BLE_VISUAL_GRACE_MS
#define DE_INDICATOR_BLE_VISUAL_GRACE_MS 10000
#endif

#ifndef DE_INDICATOR_OUTPUT_CHECK_SUPPRESS_AFTER_BLE_MS
#define DE_INDICATOR_OUTPUT_CHECK_SUPPRESS_AFTER_BLE_MS 1000
#endif

#ifndef DE_INDICATOR_LAYER_EVENT_DEBOUNCE_MS
#define DE_INDICATOR_LAYER_EVENT_DEBOUNCE_MS 80
#endif

#ifndef DE_INDICATOR_UPDATE_INTERVAL_MS
#define DE_INDICATOR_UPDATE_INTERVAL_MS 20
#endif

#ifndef SHOW_LED_BATTERY_KEYCODE
#define SHOW_LED_BATTERY_KEYCODE 0xAB
#endif

#ifndef SHUTDOWN_LED_SLEEP_KEYCODE
#define SHUTDOWN_LED_SLEEP_KEYCODE 0xAC
#endif

#ifndef SHOW_LED_OUTPUT_KEYCODE
#define SHOW_LED_OUTPUT_KEYCODE 0xAD
#endif

#ifndef SHOW_LED_BLE_PROFILE_STATUS_KEYCODE
#define SHOW_LED_BLE_PROFILE_STATUS_KEYCODE 0xAE
#endif

#ifndef STOP_LED_BLE_PROFILE_STATUS_KEYCODE
#define STOP_LED_BLE_PROFILE_STATUS_KEYCODE 0xAF
#endif

// =====================
// INTERNAL CONTEXT
// =====================

typedef struct {
    bool battery_check;
    bool output_check;
    bool ble_switch;
    bool layer_event;

    bool ble_pairing;
    bool ble_connecting;
    bool ble_connected;
    bool boot;

    bool sleep;
} indicator_flags_t;

typedef struct {
    int64_t battery_check_start;
    int64_t output_check_start;
    int64_t ble_switch_start;
    int64_t layer_start;
    int64_t ble_connected_start;

    int64_t battery_warn_last;
    int64_t state_enter_time;

    int64_t endpoint_event_last;
    int64_t ble_event_last;
    int64_t layer_event_last;
    int64_t boot_start;
    int64_t ble_pairing_start;
    int64_t ble_connecting_start;
} indicator_timers_t;

typedef struct {
    uint8_t battery_level;
    bool capslock;
    uint8_t layer;
    bool output_is_usb;
    uint8_t ble_profile_index;
    bool ble_profile_connected;
    bool ble_profile_paired;
} indicator_data_t;

typedef struct {
    de_indicator_state_t state;
    indicator_flags_t flags;
    indicator_timers_t timers;
    indicator_data_t data;
} indicator_ctx_t;

static indicator_ctx_t ctx;
static struct k_work_delayable de_indicator_update_work;
static bool de_indicator_runtime_started;

// =====================
// LOW LEVEL LED CONTROL
// =====================

static void set_indicator_color(uint8_t bits) {
    static uint8_t last_bits = 0xFF;

    for (uint8_t pos = 0; pos < ARRAY_SIZE(led_idx); pos++) {
        if (bits & BIT(pos)) {
            (void)led_on(led_dev, led_idx[pos]);
        } else {
            (void)led_off(led_dev, led_idx[pos]);
        }
    }

    last_bits = bits;
}

static inline void led_set(uint8_t color_bits) {
    set_indicator_color(color_bits);
}

static inline void led_off_all(void) {
    set_indicator_color(LED_OFF);
}

static inline void led_blink_periodic(uint8_t color, int64_t now, int32_t period_ms) {
    if (((now / period_ms) & 0x1) != 0) {
        led_set(color);
    } else {
        led_off_all();
    }
}

static inline void led_blink_duration(uint8_t color, int64_t now, int64_t start, int32_t period_ms, int32_t duration_ms) {
    if ((now - start) < duration_ms) {
        led_blink_periodic(color, now, period_ms);
    } else {
        led_off_all();
    }
}

static inline void led_flash_window(uint8_t color, int64_t now, int64_t start, int32_t duration_ms) {
    if ((now - start) < duration_ms) {
        led_set(color);
    } else {
        led_off_all();
    }
}

static inline void led_flash_double(uint8_t color, int64_t now, int64_t start) {
    int64_t t = now - start;

    if ((t >= 0 && t < 90) || (t >= 220 && t < 310)) {
        led_set(color);
    } else {
        led_off_all();
    }
}

static inline void led_flash_n(uint8_t color, int64_t now, int64_t start, uint8_t count) {
    int64_t t = now - start;
    int32_t total_ms = (int32_t)count * 200;

    if (t < total_ms && (((t / 100) & 0x1) != 0)) {
        led_set(color);
    } else {
        led_off_all();
    }
}

static inline bool debounce_ok(int64_t now, int64_t *last, int32_t interval_ms) {
    if ((now - *last) < interval_ms) {
        return false;
    }

    *last = now;
    return true;
}

static inline void set_leds_gpio_off(void) {
    for (int i = 0; i < ARRAY_SIZE(led_gpio); i++) {
        gpio_pin_configure_dt(&led_gpio[i], GPIO_OUTPUT_INACTIVE);
    }
}

static inline void set_leds_gpio_on(void) {
    for (int i = 0; i < ARRAY_SIZE(led_gpio); i++) {
        gpio_pin_configure_dt(&led_gpio[i], GPIO_OUTPUT_INACTIVE);
    }
}
// =====================
// TRANSIENT LIFECYCLE
// =====================

static void clear_transient_flags(int64_t now) {
    if (ctx.flags.output_check && (now - ctx.timers.output_check_start >= DE_INDICATOR_OUTPUT_CHECK_MS)) {
        ctx.flags.output_check = false;
    }

    if (ctx.flags.battery_check &&
        (now - ctx.timers.battery_check_start >= DE_INDICATOR_BATTERY_CHECK_MS)) {
        ctx.flags.battery_check = false;
    }

    if (ctx.flags.ble_switch && (now - ctx.timers.ble_switch_start >= DE_INDICATOR_BLE_SWITCH_MS)) {
        ctx.flags.ble_switch = false;
    }

    if (ctx.flags.boot && (now - ctx.timers.boot_start >= DE_INDICATOR_BOOT_MS)) {
        ctx.flags.boot = false;
    }

    if (ctx.flags.layer_event && (now - ctx.timers.layer_start >= DE_INDICATOR_LAYER_EVENT_MS)) {
        ctx.flags.layer_event = false;
    }

    if (ctx.flags.ble_connected &&
        (now - ctx.timers.ble_connected_start >= DE_INDICATOR_BLE_CONNECTED_MS)) {
        ctx.flags.ble_connected = false;
    }

    // Auto-clear BLE pairing if no state change after timeout
    if (ctx.flags.ble_pairing &&
        (now - ctx.timers.ble_pairing_start >= DE_INDICATOR_BLE_PAIRING_TIMEOUT_MS)) {
        ctx.flags.ble_pairing = false;
    }

    // Auto-clear BLE connecting if no state change after timeout
    if (ctx.flags.ble_connecting &&
        (now - ctx.timers.ble_connecting_start >= DE_INDICATOR_BLE_CONNECTING_TIMEOUT_MS)) {
        ctx.flags.ble_connecting = false;
    }

}

// =====================
// STATE RESOLVER
// =====================

static de_indicator_state_t resolve_state_raw(int64_t now) {

    bool ble_visual_allowed = !ctx.data.output_is_usb ||
                              ctx.flags.ble_switch ||
                              ((now - ctx.timers.ble_event_last) < DE_INDICATOR_BLE_VISUAL_GRACE_MS);

    if (ctx.flags.sleep) {
        return DE_INDICATOR_STATE_SLEEP;
    }

    if (ctx.flags.boot) {
        return DE_INDICATOR_STATE_BOOT;
    }

    if (ctx.flags.output_check) {
        return DE_INDICATOR_STATE_OUTPUT_CHECK;
    }

    if (ctx.flags.battery_check) {
        return DE_INDICATOR_STATE_BATTERY_CHECK;
    }

    if (ctx.flags.ble_switch) {
        return DE_INDICATOR_STATE_BLE_SWITCH_EVENT;
    }

    if (ctx.flags.layer_event) {
        return DE_INDICATOR_STATE_LAYER_EVENT;
    }

    // Only show BLE states if visual feedback is allowed
    if (ble_visual_allowed) {
        if (ctx.flags.ble_pairing) {
            return DE_INDICATOR_STATE_BLE_PAIRING;
        }

        if (ctx.flags.ble_connecting) {
            return DE_INDICATOR_STATE_BLE_CONNECTING;
        }

        if (ctx.flags.ble_connected) {
            return DE_INDICATOR_STATE_BLE_CONNECTED;
        }
    }

    if (ctx.data.battery_level <= 10) {
        if ((now - ctx.timers.battery_warn_last) > DE_INDICATOR_BATTERY_CRITICAL_INTERVAL_MS) {
            ctx.timers.battery_warn_last = now;
            return DE_INDICATOR_STATE_BATTERY_CRITICAL;
        }
    }

    if (ctx.data.battery_level <= 20) {
        if ((now - ctx.timers.battery_warn_last) > DE_INDICATOR_BATTERY_LOW_INTERVAL_MS) {
            ctx.timers.battery_warn_last = now;
            return DE_INDICATOR_STATE_BATTERY_LOW;
        }
    }

    if (ctx.data.capslock) {
        return DE_INDICATOR_STATE_CAPSLOCK;
    }

    return DE_INDICATOR_STATE_IDLE;
}

static de_indicator_state_t resolve_state(int64_t now) {
    de_indicator_state_t next = resolve_state_raw(now);

    if (next == ctx.state) {
        return ctx.state;
    }

    if ((now - ctx.timers.state_enter_time) < DE_INDICATOR_STATE_MIN_HOLD_MS) {
        return ctx.state;
    }

    LOG_DBG("indicator state: %d -> %d", ctx.state, next);
    ctx.state = next;
    ctx.timers.state_enter_time = now;
    return ctx.state;
}

// =====================
// RENDER
// =====================

static void render_battery_check(int64_t now) {
    uint8_t battery_percent = ctx.data.battery_level;
    int64_t battery_check_start_time = ctx.timers.battery_check_start;
    switch (battery_percent) {
    case 0 ... 20:
        led_flash_window(LED_RED, now, battery_check_start_time, DE_INDICATOR_BATTERY_CHECK_MS);
        break;
    case 21 ... 50:
        led_flash_window(LED_YELLOW, now, battery_check_start_time, DE_INDICATOR_BATTERY_CHECK_MS);
        break;
    case 51 ... 100:
        led_flash_window(LED_GREEN, now, battery_check_start_time, DE_INDICATOR_BATTERY_CHECK_MS);
        break;
    default:
        led_flash_window(LED_MAGENTA, now, battery_check_start_time, DE_INDICATOR_BATTERY_CHECK_MS);
        break;
    }
}

static void render_indicator(de_indicator_state_t state, int64_t now) {

    if (ctx.flags.sleep) {
        led_off_all();
        return;
    }

    switch (state) {
    case DE_INDICATOR_STATE_SLEEP:
        led_off_all();
        break;
    case DE_INDICATOR_STATE_IDLE:
        led_off_all();
        break;

    case DE_INDICATOR_STATE_OUTPUT_CHECK:
        // Use white flash for USB output, blue flash for BLE output
        led_flash_window(ctx.data.output_is_usb ? LED_WHITE : LED_BLUE, now, ctx.timers.output_check_start, DE_INDICATOR_OUTPUT_CHECK_MS);
        break;

    case DE_INDICATOR_STATE_BATTERY_CHECK:
        render_battery_check(now);
        break;

    case DE_INDICATOR_STATE_BLE_SWITCH_EVENT:
        led_flash_double(LED_BLUE, now, ctx.timers.ble_switch_start);
        break;

    case DE_INDICATOR_STATE_BOOT: {
        static const uint8_t boot_colors[7] = {
            LED_RED,
            LED_YELLOW,
            LED_GREEN,
            LED_CYAN,
            LED_BLUE,
            LED_MAGENTA,
            LED_WHITE,
        };
        int64_t delta = now - ctx.timers.boot_start;
        uint8_t index = (delta / 120) % ARRAY_SIZE(boot_colors);
        led_set(boot_colors[index]);
        break;
    }

    case DE_INDICATOR_STATE_LAYER_EVENT:
        led_flash_n(LED_CYAN, now, ctx.timers.layer_start, ctx.data.layer);
        break;

    case DE_INDICATOR_STATE_BLE_PAIRING:
        led_blink_periodic(LED_BLUE, now, DE_INDICATOR_BLINK_FAST_MS);
        break;

    case DE_INDICATOR_STATE_BLE_CONNECTING:
        led_blink_periodic(LED_BLUE, now, DE_INDICATOR_BLINK_SLOW_MS);
        break;

    case DE_INDICATOR_STATE_BLE_CONNECTED:
        led_flash_window(LED_BLUE, now, ctx.timers.ble_connected_start, DE_INDICATOR_BLE_CONNECTED_MS);
        break;

    case DE_INDICATOR_STATE_BATTERY_LOW:
        led_blink_periodic(LED_YELLOW, now, DE_INDICATOR_BLINK_SLOW_MS);
        break;

    case DE_INDICATOR_STATE_BATTERY_CRITICAL:
        led_blink_periodic(LED_RED, now, DE_INDICATOR_BLINK_FAST_MS);
        break;

    case DE_INDICATOR_STATE_CAPSLOCK:
        led_set(LED_WHITE);
        break;

    default:
        led_off_all();
        break;
    }
}

// =====================
// PUBLIC API
// =====================

static void de_indicator_update_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    de_indicator_update();
    (void)k_work_schedule(&de_indicator_update_work, K_MSEC(DE_INDICATOR_UPDATE_INTERVAL_MS));
}

void de_indicator_init(void) {
    memset(&ctx, 0, sizeof(ctx));

    if (!device_is_ready(led_dev)) {
        LOG_ERR("Indicator LED device is not ready");
        return;
    }

    ctx.state = DE_INDICATOR_STATE_BOOT;
    ctx.timers.state_enter_time = k_uptime_get();
    ctx.flags.boot = true;
    ctx.timers.boot_start = ctx.timers.state_enter_time;
    led_off_all();

    if (!de_indicator_runtime_started) {
        k_work_init_delayable(&de_indicator_update_work, de_indicator_update_work_handler);
        (void)k_work_schedule(&de_indicator_update_work, K_MSEC(DE_INDICATOR_UPDATE_INTERVAL_MS));
        de_indicator_runtime_started = true;
    }
}

void de_indicator_update(void) {
    int64_t now = k_uptime_get();
    clear_transient_flags(now);

    if (ctx.flags.sleep) {
        led_off_all();
        return; 
    }

    de_indicator_state_t state = resolve_state(now);
    render_indicator(state, now);
}

void de_indicator_blink_fast(uint8_t color, int64_t now) {
    led_blink_periodic(color, now, DE_INDICATOR_BLINK_FAST_MS);
}

void de_indicator_blink_slow(uint8_t color, int64_t now) {
    led_blink_periodic(color, now, DE_INDICATOR_BLINK_SLOW_MS);
}

void de_indicator_flash(uint8_t color, int64_t now, int duration_ms) {
    if (duration_ms <= 0) {
        led_off_all();
        return;
    }

    if (((now / duration_ms) & 0x1) == 0) {
        led_set(color);
    } else {
        led_off_all();
    }
}

void de_indicator_trigger_battery_check(void) {
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    ctx.data.battery_level = zmk_battery_state_of_charge();
#endif
    ctx.flags.battery_check = true;
    ctx.timers.battery_check_start = k_uptime_get();
}

void de_indicator_trigger_output_check(bool is_usb_output) {
    int64_t now = k_uptime_get();

    ctx.data.output_is_usb = is_usb_output;
    if (!ctx.flags.output_check) {
        ctx.flags.output_check = true;
        ctx.timers.output_check_start = now;
    }
}

void de_indicator_trigger_ble_switch(void) {
    ctx.flags.ble_switch = true;
    ctx.timers.ble_switch_start = k_uptime_get();
}

void de_indicator_trigger_layer_event(uint8_t layer) {
    ctx.flags.layer_event = true;
    ctx.timers.layer_start = k_uptime_get();
    ctx.data.layer = layer;
}

void de_indicator_ble_pairing(bool active) {
    ctx.flags.ble_pairing = active;
    if (active) {
        ctx.flags.ble_connecting = false;
        ctx.flags.ble_connected = false;
        ctx.timers.ble_pairing_start = k_uptime_get();
    }
}

void de_indicator_ble_connecting(bool active) {
    ctx.flags.ble_connecting = active;
    if (active) {
        ctx.flags.ble_pairing = false;
        ctx.flags.ble_connected = false;
        ctx.timers.ble_connecting_start = k_uptime_get();
    }
}

void de_indicator_ble_connected(void) {
    ctx.flags.ble_connected = true;
    ctx.flags.ble_pairing = false;
    ctx.flags.ble_connecting = false;
    ctx.timers.ble_connected_start = k_uptime_get();
}

void de_indicator_set_battery(uint8_t percent) {
    ctx.data.battery_level = percent;
}

void de_indicator_set_capslock(bool status) {
    ctx.data.capslock = status;
}

void de_indicator_set_sleep(bool sleep) {
    ctx.flags.sleep = sleep;

    if (sleep) {
        led_off_all();
    }
}

// =====================
// ZMK EVENT INTEGRATION
// =====================

// Render LED Incdication based on user keycode presses 
static int de_indicator_handle_keycode_user(const struct zmk_keycode_state_changed *event) {
    zmk_key_t key = event->keycode;
    LOG_DBG("key 0x%X state=%d", key, event->state);

    if (!event->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ctx.flags.sleep) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    
    switch (key) {
        case SHUTDOWN_LED_SLEEP_KEYCODE:
            de_indicator_set_sleep(true);
            return ZMK_EV_EVENT_HANDLED;

        case SHOW_LED_BATTERY_KEYCODE:
            de_indicator_trigger_battery_check();
            return ZMK_EV_EVENT_HANDLED;

        case SHOW_LED_OUTPUT_KEYCODE:
            de_indicator_trigger_output_check(ctx.data.output_is_usb);
            return ZMK_EV_EVENT_HANDLED;
        
        case SHOW_LED_BLE_PROFILE_STATUS_KEYCODE:
            de_indicator_trigger_ble_profile_status();
            return ZMK_EV_EVENT_HANDLED;

        case STOP_LED_BLE_PROFILE_STATUS_KEYCODE:
            ctx.flags.ble_pairing = false;
            ctx.flags.ble_connecting = false;
            ctx.flags.ble_connected = false;
            return ZMK_EV_EVENT_HANDLED;

        default:
            return ZMK_EV_EVENT_BUBBLE;
    }
}

static int de_indicator_keycode_user_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *kc_state;

    kc_state = as_zmk_keycode_state_changed(eh);

    if (kc_state != NULL) {
        return de_indicator_handle_keycode_user(kc_state);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(keycode_user, de_indicator_keycode_user_listener);
ZMK_SUBSCRIPTION(keycode_user, zmk_keycode_state_changed);


static int de_indicator_ble_profile_listener(const zmk_event_t *eh) {
    const struct zmk_ble_active_profile_changed *ev = as_zmk_ble_active_profile_changed(eh);
    int64_t now = k_uptime_get();
    static int8_t last_profile_index = -1;

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!debounce_ok(now, &ctx.timers.ble_event_last, DE_INDICATOR_BLE_EVENT_DEBOUNCE_MS)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Compare current active profile index with last known index to detect profile switch
    uint8_t current_profile_index = zmk_ble_active_profile_index();
    if (current_profile_index != (uint8_t)last_profile_index) {
        de_indicator_trigger_ble_switch();
        last_profile_index = (int8_t)current_profile_index;
    }

    if (zmk_ble_active_profile_is_connected()) {
        ctx.flags.ble_connected = true;
        ctx.flags.ble_connecting = false;
        ctx.flags.ble_pairing = false;
        ctx.timers.ble_connected_start = now;
        return ZMK_EV_EVENT_BUBBLE;
    }

    bt_addr_le_t *addr = zmk_ble_active_profile_addr();
    bool ble_profile_paired = (addr != NULL) && !bt_addr_le_eq(addr, BT_ADDR_LE_ANY);

    ctx.flags.ble_connected = false;
    ctx.flags.ble_connecting = ble_profile_paired;
    ctx.flags.ble_pairing = !ble_profile_paired;

    return ZMK_EV_EVENT_BUBBLE;
}

void de_indicator_trigger_ble_profile_status(void) {
    int64_t now = k_uptime_get();
    
    ctx.data.ble_profile_index = zmk_ble_active_profile_index();
    ctx.data.ble_profile_connected = zmk_ble_active_profile_is_connected();

    bt_addr_le_t *addr = zmk_ble_active_profile_addr();
    ctx.data.ble_profile_paired = (addr != NULL) && !bt_addr_le_eq(addr, BT_ADDR_LE_ANY);

    ctx.timers.ble_event_last = now;

    if (ctx.data.ble_profile_connected) {
        de_indicator_ble_connected();
    } else if (ctx.data.ble_profile_paired) {
        de_indicator_ble_connecting(true);
    } else {
        de_indicator_ble_pairing(true);
    }
}

static int de_indicator_endpoint_changed_listener(const zmk_event_t *eh) {
    const struct zmk_endpoint_changed *ev = as_zmk_endpoint_changed(eh);
    int64_t now = k_uptime_get();

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!debounce_ok(now, &ctx.timers.endpoint_event_last,
                     DE_INDICATOR_ENDPOINT_DEBOUNCE_MS)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    bool is_usb = (ev->endpoint.transport == ZMK_TRANSPORT_USB);
    bool prev_is_usb = ctx.data.output_is_usb;

    ctx.data.output_is_usb = is_usb;

    // 
    if (is_usb == prev_is_usb) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // 
    bool ble_switch_recent =
        (now - ctx.timers.ble_event_last) < DE_INDICATOR_OUTPUT_CHECK_SUPPRESS_AFTER_BLE_MS;

    // ===== USB =====
    if (is_usb) {
        // When switching to USB, clear BLE profile status indicators
        ctx.flags.ble_pairing = false;
        ctx.flags.ble_connecting = false;
        ctx.flags.ble_connected = false;

        if (!ble_switch_recent) {
            de_indicator_trigger_output_check(true);
        }
    }

    // ===== BLE =====
    else {
        bool ble_recent_activity = (now - ctx.timers.ble_event_last < 50);
        
        bool no_ble_activity =
            !ctx.flags.ble_pairing &&
            !ctx.flags.ble_connecting &&
            !ctx.flags.ble_connected &&
            !ble_recent_activity;

        if (no_ble_activity && !ctx.flags.output_check) {
            de_indicator_trigger_output_check(false);
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
static int de_indicator_battery_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    de_indicator_set_battery(ev->state_of_charge);
    return ZMK_EV_EVENT_BUBBLE;
}
#endif

static int de_indicator_hid_listener(const zmk_event_t *eh) {

    if (ctx.flags.sleep) {
        return ZMK_EV_EVENT_BUBBLE; 
    }

    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    de_indicator_set_capslock((zmk_hid_indicators_get_current_profile() & CAPSLOCK_BIT) != 0);
    return ZMK_EV_EVENT_BUBBLE;
}

static int de_indicator_layer_listener(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    int64_t now = k_uptime_get();

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!debounce_ok(now, &ctx.timers.layer_event_last, DE_INDICATOR_LAYER_EVENT_DEBOUNCE_MS)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t layer = zmk_keymap_highest_layer_active();
    if (layer > 5) {
        layer = 5;
    }

    de_indicator_trigger_layer_event(layer == 0 ? 1 : layer);
    return ZMK_EV_EVENT_BUBBLE;
}

#if DE_INDICATOR_HAS_ACTIVITY_EVENT
static int de_indicator_activity_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

#ifdef ZMK_ACTIVITY_SLEEP
    if (ev->state == ZMK_ACTIVITY_SLEEP) {
        de_indicator_set_sleep(true);
        led_off_all();
        // set_leds_gpio_off();
        k_busy_wait(1000);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif // ZMK_ACTIVITY_SLEEP

#ifdef ZMK_ACTIVITY_ACTIVE
    if (ev->state == ZMK_ACTIVITY_ACTIVE) {
        // set_leds_gpio_on();
        de_indicator_set_sleep(false);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif // ZMK_ACTIVITY_ACTIVE

    return ZMK_EV_EVENT_BUBBLE;
}
#endif // DE_INDICATOR_HAS_ACTIVITY_EVENT

ZMK_LISTENER(de_indicator_v2_endpoint, de_indicator_endpoint_changed_listener);
ZMK_SUBSCRIPTION(de_indicator_v2_endpoint, zmk_endpoint_changed);

ZMK_LISTENER(de_indicator_v2_ble_profile, de_indicator_ble_profile_listener);
ZMK_SUBSCRIPTION(de_indicator_v2_ble_profile, zmk_ble_active_profile_changed);

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
ZMK_LISTENER(de_indicator_v2_battery, de_indicator_battery_listener);
ZMK_SUBSCRIPTION(de_indicator_v2_battery, zmk_battery_state_changed);
#endif

ZMK_LISTENER(de_indicator_v2_hid, de_indicator_hid_listener);
ZMK_SUBSCRIPTION(de_indicator_v2_hid, zmk_hid_indicators_changed);

ZMK_LISTENER(de_indicator_v2_layer, de_indicator_layer_listener);
ZMK_SUBSCRIPTION(de_indicator_v2_layer, zmk_layer_state_changed);

#if DE_INDICATOR_HAS_ACTIVITY_EVENT
ZMK_LISTENER(de_indicator_v2_activity, de_indicator_activity_listener);
ZMK_SUBSCRIPTION(de_indicator_v2_activity, zmk_activity_state_changed);
#endif

static int de_indicator_v2_sys_init(const struct device *dev) {
    ARG_UNUSED(dev);
    de_indicator_init();
    return 0;
}

SYS_INIT(de_indicator_v2_sys_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
