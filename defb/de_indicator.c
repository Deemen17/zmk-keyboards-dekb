#include "de_indicator.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/sys/reboot.h>

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
#include <zmk/usb.h>


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

// =====================
// CONFIG
// =====================

/*
 * Indicator timing
 *
 * These values control user-visible timing and state lifetime.
 * Keep visual duration and state lifetime separate where possible.
 */

#define DE_INDICATOR_PRE_SLEEP_MS 200
#define DE_INDICATOR_BLINK_FAST_MS 120
#define DE_INDICATOR_BLINK_SLOW_MS 500

/* Duration of the manual battery status display. */
#define DE_INDICATOR_BATTERY_CHECK_MS 1500

/*
 * Lifetime of the output-check state.
 * This is intentionally longer than some visual patterns so the state
 * remains stable while related BLE/USB events settle.
 */
#define DE_INDICATOR_OUTPUT_CHECK_STATE_MS 3000

/*
 * BLE indicator timing
 *
 * BLE events can arrive in bursts and may race with endpoint changes.
 * Debounce prevents duplicate visual events.
 * Visual grace allows a BLE event to remain visible briefly even when
 * USB is currently the active HID endpoint.
 */
#define DE_INDICATOR_BLE_SWITCH_MS 250

/* Show successful BLE connection feedback for 3 seconds. */
#define DE_INDICATOR_BLE_CONNECTED_TIMEOUT_MS 3000

/* Stop showing pairing/connecting animation if no new state arrives. */
#define DE_INDICATOR_BLE_PAIRING_TIMEOUT_MS 30000
#define DE_INDICATOR_BLE_CONNECTING_TIMEOUT_MS 30000

/* Ignore duplicate BLE profile events arriving within this window. */
#define DE_INDICATOR_BLE_EVENT_DEBOUNCE_MS 120

/*
 * After meaningful BLE activity, allow BLE feedback to remain visible
 * briefly even if the USB endpoint is active.
 * AKA BLE visual disapper after 10s when plug in USB
 */
#define DE_INDICATOR_BLE_VISUAL_GRACE_MS 10000

#define DE_INDICATOR_LAYER_EVENT_MS 3000

#define DE_INDICATOR_BOOT_MS 840

#define DE_INDICATOR_BATTERY_DEADLY     10
#define DE_INDICATOR_BATTERY_CRITICAL   20
#define DE_INDICATOR_BATTERY_LOW        30

#define DE_INDICATOR_BATTERY_DEADLY_REMIND_INTERVAL_MS     30000
#define DE_INDICATOR_BATTERY_CRITICAL_REMIND_INTERVAL_MS   30000
#define DE_INDICATOR_BATTERY_LOW_REMIND_INTERVAL_MS        (5 * 60 * 1000)

#define DE_INDICATOR_BATTERY_DEADLY_DURATION_MS       10000
#define DE_INDICATOR_BATTERY_CRITICAL_DURATION_MS     10000
#define DE_INDICATOR_BATTERY_LOW_DURATION_MS          5000

#define DE_INDICATOR_BATTERY_FLASH_PERIOD_LOW_MS      500       
#define DE_INDICATOR_BATTERY_FLASH_PERIOD_CRITICAL_MS 300       
#define DE_INDICATOR_BATTERY_FLASH_PERIOD_DEADLY_MS   200  

#define DE_INDICATOR_STATE_MIN_HOLD_MS 120

#define DE_INDICATOR_ENDPOINT_DEBOUNCE_MS 120

#define DE_INDICATOR_OUTPUT_CHECK_SUPPRESS_AFTER_BLE_MS 1000
#define DE_INDICATOR_LAYER_EVENT_DEBOUNCE_MS 80
#define DE_INDICATOR_UPDATE_INTERVAL_MS 50

#define DE_INDICATOR_BATTERY_WARNING_GRACE_MS (5 * 60 * 1000)

// =====================
// DE CUSTOM KEYCODES
// =====================

#define SHUTDOWN_LED_SLEEP_KEYCODE 0xDE01
#define SHOW_LED_BATTERY_KEYCODE 0xDE02
#define SHOW_LED_OUTPUT_KEYCODE 0xDE03
#define SHOW_LED_OUTPUT_USB_KEYCODE 0xDE04
#define SHOW_LED_BLE_PROFILE_STATUS_KEYCODE 0xDE05

// =====================
// INTERNAL CONTEXT
// =====================

typedef struct {
    bool is_sleeping;
    bool is_booting;

    bool evt_battery_check;
    bool evt_output_check;
    bool evt_output_usb;
    bool evt_layer;
    bool evt_ble_switch;
    
    bool ble_is_pairing;
    bool ble_is_connecting;
    bool ble_is_connected;

} indicator_flags_t;

typedef struct {
    int64_t state_enter_time;
    int64_t boot_start_ts;
    int64_t last_activity_time;

    int64_t endpoint_event_last_ts;
    int64_t output_check_start_ts;
    int64_t output_usb_start_ts;

    int64_t battery_check_start_ts;
    int64_t battery_warn_last_ts;

    int64_t layer_start_ts;
    int64_t layer_event_last_ts;
    
    int64_t ble_switch_start_ts;
    int64_t ble_connected_start_ts;
    int64_t ble_pairing_start_ts;
    int64_t ble_connecting_start_ts;

    /* Last accepted BLE event, used only for debounce. */
    int64_t ble_event_last_ts;

    /*
    * Last meaningful BLE activity used by the visual resolver.
    * This is intentionally separate from the debounce timestamp.
    */
    int64_t ble_visual_activity_ts;
    
} indicator_timers_t;

typedef struct {
    uint8_t battery_percent;
    bool battery_valid;
    uint8_t active_layer;
    uint8_t ble_profile_index;

    bool capslock_on;
    
    bool is_usb_output;
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

static void de_indicator_ble_disabled(void);
static void de_indicator_clear_battery_warning(void);

// =====================
// LOW LEVEL LED CONTROL
// =====================

static void set_indicator_color(uint8_t bits) {
    static uint8_t last_bits = 0xFF;

    if (bits == last_bits) {
        return;
    }

    for (uint8_t pos = 0; pos < ARRAY_SIZE(led_idx); pos++) {
        if (bits & BIT(pos)) {
            led_on(led_dev, led_idx[pos]);
        } else {
            led_off(led_dev, led_idx[pos]);
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

    if (count == 0) {
        led_off_all();
        return;
    }

    const int32_t start_delay_ms = 360;

    int64_t elapsed = now - start;

    if (elapsed < start_delay_ms) {
        led_off_all();
        return;
    }

    elapsed -= start_delay_ms;

    int32_t on_time_ms  = 350;
    int32_t off_time_ms = 100;

    if (count > 2) {
        on_time_ms  = 250;
        off_time_ms = 100;
    }

    const int32_t cycle_ms = on_time_ms + off_time_ms;
    const int32_t total_duration = count * cycle_ms;

    if (elapsed >= total_duration) {
        led_off_all();
        return;
    }

    int32_t position = elapsed % cycle_ms;

    if (position < on_time_ms) {
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

static inline void led_rainbow_cycle(int64_t elapsed, int32_t cycle_ms) {
    static const uint8_t colors[] = {
        LED_RED,
        LED_YELLOW,
        LED_GREEN,
        LED_CYAN,
        LED_BLUE,
        LED_MAGENTA,
    };

    int64_t segment_ms = cycle_ms / ARRAY_SIZE(colors);
    uint8_t index = elapsed / segment_ms;

    if (index >= ARRAY_SIZE(colors)) {
        index = ARRAY_SIZE(colors) - 1;
    }

    led_set(colors[index]);
}

// =====================
// TRANSIENT LIFECYCLE
// =====================

static void clear_transient_flags(int64_t now) {
    if (ctx.flags.evt_output_check && (now - ctx.timers.output_check_start_ts >= DE_INDICATOR_OUTPUT_CHECK_STATE_MS)) {
        ctx.flags.evt_output_check = false;
    }

    if (ctx.flags.evt_output_usb && (now - ctx.timers.output_usb_start_ts >= DE_INDICATOR_OUTPUT_CHECK_STATE_MS)) {
        ctx.flags.evt_output_usb = false;
    }

    if (ctx.flags.evt_battery_check &&
        (now - ctx.timers.battery_check_start_ts >= DE_INDICATOR_BATTERY_CHECK_MS)) {
        ctx.flags.evt_battery_check = false;
    }

    if (ctx.flags.evt_ble_switch && (now - ctx.timers.ble_switch_start_ts >= DE_INDICATOR_BLE_SWITCH_MS)) {
        ctx.flags.evt_ble_switch = false;
    }

    if (ctx.flags.is_booting && (now - ctx.timers.boot_start_ts >= DE_INDICATOR_BOOT_MS)) {
        ctx.flags.is_booting = false;
    }

    if (ctx.flags.evt_layer && (now - ctx.timers.layer_start_ts >= DE_INDICATOR_LAYER_EVENT_MS)) {
        ctx.flags.evt_layer = false;
    }

    if (ctx.flags.ble_is_connected &&
        (now - ctx.timers.ble_connected_start_ts >= DE_INDICATOR_BLE_CONNECTED_TIMEOUT_MS)) {
        ctx.flags.ble_is_connected = false;
    }

    // Auto-clear BLE pairing if no state change after timeout
    if (ctx.flags.ble_is_pairing &&
        (now - ctx.timers.ble_pairing_start_ts >= DE_INDICATOR_BLE_PAIRING_TIMEOUT_MS)) {
        ctx.flags.ble_is_pairing = false;
    }

    // Auto-clear BLE connecting if no state change after timeout
    if (ctx.flags.ble_is_connecting &&
        (now - ctx.timers.ble_connecting_start_ts >= DE_INDICATOR_BLE_CONNECTING_TIMEOUT_MS)) {
        ctx.flags.ble_is_connecting = false;
    }

}

// =====================
// STATE RESOLVER
// =====================

/*
 * State priority
 *
 * Higher-priority states temporarily override lower-priority states.
 *
 * 1. Sleep
 * 2. Boot
 * 3. Output transition / manual output status
 * 4. Manual battery check
 * 5. BLE transition/status
 * 6. Battery warning
 * 7. Persistent Caps Lock status
 * 8. Idle
 *
 * Persistent states such as Caps Lock are intentionally placed near
 * the bottom so temporary notifications can override them and then
 * naturally return to the previous status.
 */

static de_indicator_state_t resolve_state_raw(int64_t now) {

    if (ctx.flags.is_booting) {
        return DE_INDICATOR_STATE_BOOT;
    }

    if (ctx.flags.evt_output_check) {
        return DE_INDICATOR_STATE_OUTPUT_CHECK;
    }

    if (ctx.flags.evt_output_usb) {
        return DE_INDICATOR_STATE_OUTPUT_USB;
    }

    if (ctx.flags.evt_battery_check) {
        return DE_INDICATOR_STATE_BATTERY_CHECK;
    }

    if (ctx.flags.evt_ble_switch) {
        return DE_INDICATOR_STATE_BLE_SWITCH_EVENT;
    }

    // Allow BLE visual feedback when:
    // - Not in USB mode
    // - OR recently switched BLE profile
    // - OR within grace window after BLE activity
    bool ble_visual_allowed = 
        !ctx.data.is_usb_output  ||
        ctx.flags.evt_ble_switch ||
        ((now - ctx.timers.ble_visual_activity_ts) < DE_INDICATOR_BLE_VISUAL_GRACE_MS);

    // Only show BLE states if visual feedback is allowed
    if (ble_visual_allowed) {
        if (ctx.flags.ble_is_pairing) {
            return DE_INDICATOR_STATE_BLE_PAIRING;
        }

        if (ctx.flags.ble_is_connecting) {
            return DE_INDICATOR_STATE_BLE_CONNECTING;
        }

        if (ctx.flags.ble_is_connected) {
            return DE_INDICATOR_STATE_BLE_CONNECTED;
        }
    }
    
    /*
    * Battery warnings are suppressed while USB power is present.
    *
    * DE60 BLE REV1 does not expose charger status to the MCU, so USB power
    * cannot be interpreted as confirmed charging or full-charge state.
    */
    if (ctx.data.battery_valid && (zmk_usb_is_powered() == 0)) {
        int64_t time_since_last_warn = now - ctx.timers.battery_warn_last_ts;

        if (ctx.data.battery_percent <= DE_INDICATOR_BATTERY_DEADLY) {
            if (time_since_last_warn >= DE_INDICATOR_BATTERY_DEADLY_REMIND_INTERVAL_MS) {
                ctx.timers.battery_warn_last_ts = now;
                return DE_INDICATOR_STATE_BATTERY_DEADLY;
            }
            else if (ctx.state == DE_INDICATOR_STATE_BATTERY_DEADLY) {
                return DE_INDICATOR_STATE_BATTERY_DEADLY;
            }
        }
        else if (ctx.data.battery_percent <= DE_INDICATOR_BATTERY_CRITICAL) {
            if (time_since_last_warn >= DE_INDICATOR_BATTERY_CRITICAL_REMIND_INTERVAL_MS) {
                ctx.timers.battery_warn_last_ts = now;
                return DE_INDICATOR_STATE_BATTERY_CRITICAL;
            }
            else if (ctx.state == DE_INDICATOR_STATE_BATTERY_CRITICAL) {
                return DE_INDICATOR_STATE_BATTERY_CRITICAL;
            }
        }
        else if (ctx.data.battery_percent <= DE_INDICATOR_BATTERY_LOW) {
            if (time_since_last_warn >= DE_INDICATOR_BATTERY_LOW_REMIND_INTERVAL_MS) {
                ctx.timers.battery_warn_last_ts = now;
                return DE_INDICATOR_STATE_BATTERY_LOW;
            }
            else if (ctx.state == DE_INDICATOR_STATE_BATTERY_LOW) {
                return DE_INDICATOR_STATE_BATTERY_LOW;
            }
        }
    }

    if (ctx.data.capslock_on) {
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
    uint8_t battery_percent = ctx.data.battery_percent;
    int64_t start = ctx.timers.battery_check_start_ts;

    // 10
    if (battery_percent <= DE_INDICATOR_BATTERY_DEADLY) {
        led_blink_duration(LED_RED, now, start, DE_INDICATOR_BATTERY_FLASH_PERIOD_DEADLY_MS, DE_INDICATOR_BATTERY_CHECK_MS);

    // 20
    } else if (battery_percent <= DE_INDICATOR_BATTERY_CRITICAL) {
        led_flash_window(LED_RED, now, start, DE_INDICATOR_BATTERY_CHECK_MS);

    // 30
    } else if (battery_percent <= DE_INDICATOR_BATTERY_LOW) {
        led_flash_window(LED_YELLOW, now, start, DE_INDICATOR_BATTERY_CHECK_MS);

    // 50
    } else if (battery_percent <= 50) {
        led_flash_n(LED_GREEN, now, start, 2);

    // 50-100 
    } else if (battery_percent > 50 && battery_percent <= 100) {
        led_flash_window(LED_GREEN, now, start, DE_INDICATOR_BATTERY_CHECK_MS);

    // Can not read percent
    } else {
        led_flash_window(LED_MAGENTA, now, start, DE_INDICATOR_BATTERY_CHECK_MS);
    }
}

static void render_battery_warning(int64_t now, de_indicator_state_t state) {
    int64_t start = ctx.timers.battery_warn_last_ts;

    switch (state) {

        // baterry <= 10% 
        case DE_INDICATOR_STATE_BATTERY_DEADLY:
            led_blink_duration(LED_RED, now, start, DE_INDICATOR_BATTERY_FLASH_PERIOD_DEADLY_MS, DE_INDICATOR_BATTERY_DEADLY_DURATION_MS);
            break;

        // battery <= 20%
        case DE_INDICATOR_STATE_BATTERY_CRITICAL:
            led_blink_duration(LED_RED, now, start, DE_INDICATOR_BATTERY_FLASH_PERIOD_CRITICAL_MS, DE_INDICATOR_BATTERY_CRITICAL_DURATION_MS);
            break;

        // battery <= 30%,
        case DE_INDICATOR_STATE_BATTERY_LOW:
            led_blink_duration(LED_YELLOW, now, start, DE_INDICATOR_BATTERY_FLASH_PERIOD_LOW_MS, DE_INDICATOR_BATTERY_LOW_DURATION_MS);
            break;

        default:
            led_off_all();
            break;
    }
}

static void render_indicator_state(de_indicator_state_t state, int64_t now) {

    switch (state) {
    case DE_INDICATOR_STATE_SLEEP:
    case DE_INDICATOR_STATE_IDLE:
        led_off_all();
        break;

    case DE_INDICATOR_STATE_OUTPUT_CHECK:
        if (ctx.data.is_usb_output) {
            led_flash_window(LED_WHITE, now, ctx.timers.output_check_start_ts, 1000);
        } else {
            led_flash_n(LED_BLUE, now, ctx.timers.output_check_start_ts, ctx.data.ble_profile_index + 1);
        }
        break;
    
    case DE_INDICATOR_STATE_OUTPUT_USB:
        led_flash_window(LED_WHITE, now, ctx.timers.output_usb_start_ts, 1000);
        break;

    case DE_INDICATOR_STATE_BATTERY_CHECK:
        render_battery_check(now);
        break;

    case DE_INDICATOR_STATE_BLE_SWITCH_EVENT:
        led_flash_double(LED_BLUE, now, ctx.timers.ble_switch_start_ts);
        break;

    case DE_INDICATOR_STATE_BOOT: 
        led_rainbow_cycle(now - ctx.timers.boot_start_ts, DE_INDICATOR_BOOT_MS);
        break;

    case DE_INDICATOR_STATE_LAYER_EVENT:
        // led_flash_n(LED_CYAN, now, ctx.timers.layer_start_ts, ctx.data.active_layer);
        break;

    case DE_INDICATOR_STATE_BLE_PAIRING:
        led_blink_periodic(LED_BLUE, now, DE_INDICATOR_BLINK_FAST_MS);
        break;

    case DE_INDICATOR_STATE_BLE_CONNECTING:
        led_blink_periodic(LED_BLUE, now, DE_INDICATOR_BLINK_SLOW_MS);
        break;

    case DE_INDICATOR_STATE_BLE_CONNECTED:
        led_flash_window(LED_BLUE, now, ctx.timers.ble_connected_start_ts, DE_INDICATOR_BLE_CONNECTED_TIMEOUT_MS);
        break;

    case DE_INDICATOR_STATE_BATTERY_DEADLY:
    case DE_INDICATOR_STATE_BATTERY_LOW:
    case DE_INDICATOR_STATE_BATTERY_CRITICAL:
        render_battery_warning(now, state);
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

void de_indicator_trigger_battery_check(void) {
    #if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    ctx.data.battery_percent = zmk_battery_state_of_charge();
    #endif 
    ctx.flags.evt_battery_check = true;
    ctx.timers.battery_check_start_ts = k_uptime_get();
}

void de_indicator_trigger_output_check(bool is_usb_output) {
    int64_t now = k_uptime_get();

    ctx.data.is_usb_output = is_usb_output;
    if (!ctx.flags.evt_output_check) {
        ctx.flags.evt_output_check = true;
        ctx.timers.output_check_start_ts = now;

        if (!is_usb_output) {
            ctx.data.ble_profile_index = zmk_ble_active_profile_index();   
        }
    }
}

void de_indicator_trigger_show_led_output_usb(void) {
    if (!ctx.flags.evt_output_usb && ctx.data.is_usb_output) {
        ctx.flags.evt_output_usb = true;
        de_indicator_ble_disabled(); // Disable BLE visual feedback when USB output is active
        ctx.timers.output_usb_start_ts = k_uptime_get();
    }
}

void de_indicator_trigger_ble_switch(void) {
    ctx.flags.evt_ble_switch = true;
    ctx.timers.ble_switch_start_ts = k_uptime_get();
}

void de_indicator_trigger_layer_event(uint8_t active_layer) {
    ctx.flags.evt_layer = true;
    ctx.timers.layer_start_ts = k_uptime_get();
    ctx.data.active_layer = active_layer;
}

void de_indicator_set_ble_state(bool pairing, bool connecting, bool connected) {
    ctx.flags.ble_is_pairing = pairing;
    ctx.flags.ble_is_connecting = connecting;
    ctx.flags.ble_is_connected = connected;
    if (pairing) {
        ctx.timers.ble_pairing_start_ts = k_uptime_get();
    } else if (connecting) {
        ctx.timers.ble_connecting_start_ts = k_uptime_get();
    } else if (connected) {
        ctx.timers.ble_connected_start_ts = k_uptime_get();
    }
}

void de_indicator_ble_pairing(void) {
    de_indicator_set_ble_state(true, false, false);
}

void de_indicator_ble_connecting(void) {
    de_indicator_set_ble_state(false, true, false);
}

void de_indicator_ble_connected(void) {
    de_indicator_set_ble_state(false, false, true);
}

static void de_indicator_ble_disabled(void) {
    de_indicator_set_ble_state(false, false, false);
}

void de_indicator_set_battery(uint8_t percent) {
    ctx.data.battery_percent = percent;
    // Suppress low-battery warning while USB power is present.
    if (zmk_usb_is_powered()) {
        de_indicator_clear_battery_warning();
    }
}

void de_indicator_set_sleep(bool is_sleeping) {
    ctx.flags.is_sleeping = is_sleeping;

    if (is_sleeping) {
        led_off_all();
    }
}

static void de_indicator_clear_battery_warning(void) {
    int64_t now = k_uptime_get();

    /*
    * Delay the next battery warning after USB power is detected.
    *
    * This prevents a low-battery warning from immediately returning when
    * the keyboard transitions to USB power.
    */
    ctx.timers.battery_warn_last_ts =
        now + DE_INDICATOR_BATTERY_WARNING_GRACE_MS;

    if (ctx.state == DE_INDICATOR_STATE_BATTERY_LOW ||
        ctx.state == DE_INDICATOR_STATE_BATTERY_CRITICAL ||
        ctx.state == DE_INDICATOR_STATE_BATTERY_DEADLY) {
        ctx.state = DE_INDICATOR_STATE_IDLE;
        ctx.timers.state_enter_time = now;
    }
}

// =====================
// ZMK EVENT INTEGRATION
// =====================

// Handle DE-specific indicator commands exposed as custom keycodes.
static int de_indicator_handle_keycode_user(const struct zmk_keycode_state_changed *event) {
    zmk_key_t key = event->keycode;
    LOG_DBG("key 0x%X state=%d", key, event->state);

    if (!event->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ctx.flags.is_sleeping) {
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
            de_indicator_trigger_output_check(ctx.data.is_usb_output);
            return ZMK_EV_EVENT_HANDLED;

        case SHOW_LED_OUTPUT_USB_KEYCODE:
            de_indicator_trigger_show_led_output_usb();
            return ZMK_EV_EVENT_HANDLED;
        
        case SHOW_LED_BLE_PROFILE_STATUS_KEYCODE:
            de_indicator_trigger_ble_profile_status();
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

    if (!debounce_ok(now, &ctx.timers.ble_event_last_ts, DE_INDICATOR_BLE_EVENT_DEBOUNCE_MS)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    ctx.timers.ble_visual_activity_ts = now;

    // Compare current active profile index with last known index to detect profile switch
    uint8_t current_profile_index = zmk_ble_active_profile_index();
    if (current_profile_index != (uint8_t)last_profile_index) {
        de_indicator_trigger_ble_switch();
        last_profile_index = (int8_t)current_profile_index;
    }

    if (zmk_ble_active_profile_is_connected()) {
        de_indicator_ble_connected();
        return ZMK_EV_EVENT_BUBBLE;
    }

    bt_addr_le_t *addr = zmk_ble_active_profile_addr();
    bool ble_profile_paired = (addr != NULL) && !bt_addr_le_eq(addr, BT_ADDR_LE_ANY);

    if (ble_profile_paired) {
        de_indicator_ble_connecting();
        return ZMK_EV_EVENT_BUBBLE;
    } else {
        de_indicator_ble_pairing();
        return ZMK_EV_EVENT_BUBBLE;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(de_indicator_v2_ble_profile, de_indicator_ble_profile_listener);
ZMK_SUBSCRIPTION(de_indicator_v2_ble_profile, zmk_ble_active_profile_changed);

void de_indicator_trigger_ble_profile_status(void) {
    int64_t now = k_uptime_get();
    
    ctx.data.ble_profile_index = zmk_ble_active_profile_index();
    ctx.data.ble_profile_connected = zmk_ble_active_profile_is_connected();

    bt_addr_le_t *addr = zmk_ble_active_profile_addr();
    ctx.data.ble_profile_paired = (addr != NULL) && !bt_addr_le_eq(addr, BT_ADDR_LE_ANY);

    ctx.timers.ble_visual_activity_ts = now;

    if (ctx.data.ble_profile_connected) {
        de_indicator_ble_connected();
    } else if (ctx.data.ble_profile_paired) {
        de_indicator_ble_connecting();
    } else {
        de_indicator_ble_pairing();
    }
}

static int de_indicator_endpoint_changed_listener(const zmk_event_t *eh) {
    const struct zmk_endpoint_changed *ev = as_zmk_endpoint_changed(eh);
    int64_t now = k_uptime_get();

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!debounce_ok(now, &ctx.timers.endpoint_event_last_ts,
                     DE_INDICATOR_ENDPOINT_DEBOUNCE_MS)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    bool is_usb = (ev->endpoint.transport == ZMK_TRANSPORT_USB);
    bool is_ble = (ev->endpoint.transport == ZMK_TRANSPORT_BLE);
    bool prev_is_usb = ctx.data.is_usb_output;

    ctx.data.is_usb_output = is_usb;

    if (is_usb == prev_is_usb) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // If switched to USB, suppress BLE output check if there was recent BLE activity to avoid false trigger
    bool ble_switch_recent = (now - ctx.timers.ble_visual_activity_ts) < DE_INDICATOR_OUTPUT_CHECK_SUPPRESS_AFTER_BLE_MS;

    // ===== USB =====
    if (is_usb) {
        de_indicator_ble_disabled();
        de_indicator_clear_battery_warning();

        if (!ble_switch_recent) {
            de_indicator_trigger_output_check(true);
        }
    }

    // ===== BLE =====
    if (is_ble) {
        // Short window to avoid false output check trigger after BLE event
        bool ble_recent_activity = (now - ctx.timers.ble_visual_activity_ts < 50);
        
        bool no_ble_activity =
            !ctx.flags.ble_is_pairing &&
            !ctx.flags.ble_is_connecting &&
            !ctx.flags.ble_is_connected &&
            !ble_recent_activity;

        if (no_ble_activity && !ctx.flags.evt_output_check) {
            // Indicate BLE profile
            de_indicator_trigger_output_check(false);
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(de_indicator_v2_endpoint, de_indicator_endpoint_changed_listener);
ZMK_SUBSCRIPTION(de_indicator_v2_endpoint, zmk_endpoint_changed);

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
static int de_indicator_battery_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    de_indicator_set_battery(ev->state_of_charge);
    // Mark battery as valid to indicate that we have received a valid battery state
    ctx.data.battery_valid = true;

    return ZMK_EV_EVENT_BUBBLE;
}
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
ZMK_LISTENER(de_indicator_v2_battery, de_indicator_battery_listener);       
ZMK_SUBSCRIPTION(de_indicator_v2_battery, zmk_battery_state_changed);
#endif

static int de_indicator_hid_listener(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);

    if (!ev) return ZMK_EV_EVENT_BUBBLE;
    
    bool caps = ((zmk_hid_indicators_get_current_profile() & CAPSLOCK_BIT) != 0);
    ctx.data.capslock_on = caps;

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(de_indicator_v2_hid, de_indicator_hid_listener);
ZMK_SUBSCRIPTION(de_indicator_v2_hid, zmk_hid_indicators_changed);

static int de_indicator_layer_listener(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    int64_t now = k_uptime_get();

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!debounce_ok(now, &ctx.timers.layer_event_last_ts, DE_INDICATOR_LAYER_EVENT_DEBOUNCE_MS)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t active_layer = zmk_keymap_highest_layer_active();
    if (active_layer > 5) {
        active_layer = 5;
    }

    de_indicator_trigger_layer_event(active_layer == 0 ? 1 : active_layer);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(de_indicator_v2_layer, de_indicator_layer_listener);
ZMK_SUBSCRIPTION(de_indicator_v2_layer, zmk_layer_state_changed);

#if DE_INDICATOR_HAS_ACTIVITY_EVENT
static int de_indicator_activity_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ctx.flags.is_sleeping) {
        ctx.flags.is_sleeping = false;
    }

#ifdef ZMK_ACTIVITY_SLEEP
    if (ev->state == ZMK_ACTIVITY_SLEEP) {
        de_indicator_set_sleep(true);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif // ZMK_ACTIVITY_SLEEP

#ifdef ZMK_ACTIVITY_ACTIVE
    if (ev->state == ZMK_ACTIVITY_ACTIVE) {
        de_indicator_set_sleep(false);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif // ZMK_ACTIVITY_ACTIVE

    return ZMK_EV_EVENT_BUBBLE;
}
#endif // DE_INDICATOR_HAS_ACTIVITY_EVENT

#if DE_INDICATOR_HAS_ACTIVITY_EVENT
ZMK_LISTENER(de_indicator_v2_activity, de_indicator_activity_listener);
ZMK_SUBSCRIPTION(de_indicator_v2_activity, zmk_activity_state_changed);
#endif

static int de_indicator_activity_real_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // chỉ khi key DOWN
    if (ev->state) {
        ctx.timers.last_activity_time = k_uptime_get();

        if (ctx.flags.is_sleeping) {
            ctx.flags.is_sleeping = false;
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(de_indicator_activity_real, de_indicator_activity_real_listener);
ZMK_SUBSCRIPTION(de_indicator_activity_real, zmk_position_state_changed);

// =====================
//      INIT SYSTEM
// =====================

void de_indicator_update(void) {
    int64_t now = k_uptime_get();
    clear_transient_flags(now);

#if IS_ENABLED(CONFIG_ZMK_SLEEP)
    int64_t idle_time = now - ctx.timers.last_activity_time;

    if (!ctx.flags.is_sleeping && 
        !ctx.data.is_usb_output &&
        ctx.timers.last_activity_time != 0 &&
        idle_time > (CONFIG_ZMK_IDLE_SLEEP_TIMEOUT - DE_INDICATOR_PRE_SLEEP_MS)) {
        ctx.flags.is_sleeping = true;
    }
    
    if (ctx.flags.is_sleeping) {
        led_off_all();
        return;
    }
#endif // CONFIG_ZMK_SLEEP
    
    de_indicator_state_t state = resolve_state(now);
    render_indicator_state(state, now);
}

static void de_indicator_update_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    de_indicator_update();
    
    // Dynamic Polling 
    int32_t next_delay = DE_INDICATOR_UPDATE_INTERVAL_MS; // Default 50ms
    
    (void)k_work_schedule(&de_indicator_update_work, K_MSEC(next_delay));
}

void de_indicator_init(void) {
    memset(&ctx, 0, sizeof(ctx));
    int64_t now = k_uptime_get();
    
    if (!device_is_ready(led_dev)) {
        LOG_ERR("Indicator LED device is not ready");
        return;
    }
    
    ctx.state = DE_INDICATOR_STATE_BOOT;
    ctx.timers.state_enter_time = now;
    ctx.flags.is_booting = true;
    ctx.timers.boot_start_ts = ctx.timers.state_enter_time;
    ctx.data.battery_valid = false;
    
    led_off_all();

    // Initialize battery warning timer to allow immediate warning if battery is low on startup
    ctx.timers.battery_warn_last_ts = now - DE_INDICATOR_BATTERY_LOW_REMIND_INTERVAL_MS;
    
    if (!de_indicator_runtime_started) {
        k_work_init_delayable(&de_indicator_update_work, de_indicator_update_work_handler);
        (void)k_work_schedule(&de_indicator_update_work, K_MSEC(DE_INDICATOR_UPDATE_INTERVAL_MS));
        de_indicator_runtime_started = true;
    }
}

static int de_indicator_v2_sys_init(void) {
    de_indicator_init();
    return 0;
}

SYS_INIT(de_indicator_v2_sys_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
