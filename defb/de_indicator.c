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

#define DE_INDICATOR_PRE_SLEEP_MS 200
#define DE_INDICATOR_BLINK_FAST_MS 120
#define DE_INDICATOR_BLINK_SLOW_MS 500
#define DE_INDICATOR_BATTERY_CHECK_MS 1500
#define DE_INDICATOR_OUTPUT_CHECK_MS 800

#define DE_INDICATOR_BLE_SWITCH_MS 250
#define DE_INDICATOR_BLE_CONNECTED_TIMEOUT_MS 3000
#define DE_INDICATOR_BLE_PAIRING_TIMEOUT_MS 30000
#define DE_INDICATOR_BLE_CONNECTING_TIMEOUT_MS 30000
#define DE_INDICATOR_BLE_EVENT_DEBOUNCE_MS 120
#define DE_INDICATOR_BLE_VISUAL_GRACE_MS 10000

#define DE_INDICATOR_LAYER_EVENT_MS 3000

#define DE_INDICATOR_BOOT_MS 840

#define DE_INDICATOR_BATTERY_DEADLY_INTERVAL_MS 10000
#define DE_INDICATOR_BATTERY_CRITICAL_INTERVAL_MS 30000
#define DE_INDICATOR_BATTERY_LOW_INTERVAL_MS 45000

#define DE_INDICATOR_BATTERY_LOW_DURATION_MS          10000     
#define DE_INDICATOR_BATTERY_CRITICAL_DURATION_MS     10000     
#define DE_INDICATOR_BATTERY_DEADLY_DURATION_MS       0    

#define DE_INDICATOR_BATTERY_FLASH_PERIOD_LOW_MS      500       
#define DE_INDICATOR_BATTERY_FLASH_PERIOD_CRITICAL_MS 300       
#define DE_INDICATOR_BATTERY_FLASH_PERIOD_DEADLY_MS   100  

#define DE_INDICATOR_STATE_MIN_HOLD_MS 120

#define DE_INDICATOR_ENDPOINT_DEBOUNCE_MS 120

#define DE_INDICATOR_OUTPUT_CHECK_SUPPRESS_AFTER_BLE_MS 1000
#define DE_INDICATOR_LAYER_EVENT_DEBOUNCE_MS 80
#define DE_INDICATOR_UPDATE_INTERVAL_MS 50


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

    bool battery_is_warming;

    bool capslock_valid;
} indicator_flags_t;

typedef struct {
    int64_t state_enter_time;
    int64_t boot_start_ts;
    int64_t last_activity_time;

    int64_t endpoint_event_last_ts;
    int64_t output_check_start_ts;
    int64_t output_usb_start_ts;

    int64_t battery_check_start_ts;
    int64_t battery_warm_start_ts;
    int64_t battery_warn_last_ts;

    int64_t layer_start_ts;
    int64_t layer_event_last_ts;
    
    int64_t ble_switch_start_ts;
    int64_t ble_connected_start_ts;
    int64_t ble_pairing_start_ts;
    int64_t ble_connecting_start_ts;
    int64_t ble_last_activity_ts;
    
    int64_t capslock_last_update_ts;
} indicator_timers_t;

typedef struct {
    uint8_t battery_percent;
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

    const int32_t on_time_ms  = 120;  
    const int32_t off_time_ms = 120;   
    const int32_t cycle_ms    = on_time_ms + off_time_ms;  

    int64_t elapsed = now - start;
    int32_t total_duration = (int32_t)count * cycle_ms;

    if (elapsed >= total_duration) {
        led_off_all();
        return;
    }

    int32_t position = (int32_t)(elapsed % cycle_ms);

    if (position < on_time_ms) {
        led_set(color);        // Pha sáng
    } else {
        led_off_all();         // Pha tắt
    }
}

static inline bool debounce_ok(int64_t now, int64_t *last, int32_t interval_ms) {
    if ((now - *last) < interval_ms) {
        return false;
    }

    *last = now;
    return true;
}

static inline void led_rainbow_cycle(int64_t now, int32_t cycle_ms) {
    static const uint8_t colors[] = {
        LED_RED,
        LED_YELLOW,
        LED_GREEN,
        LED_CYAN,
        LED_BLUE,
        LED_MAGENTA,
    };
    int64_t t = now % cycle_ms;
    int64_t segment_ms = cycle_ms / ARRAY_SIZE(colors);
    uint8_t index = t / segment_ms;
    led_set(colors[index]);
}

// =====================
// TRANSIENT LIFECYCLE
// =====================

static void clear_transient_flags(int64_t now) {
    if (ctx.flags.evt_output_check && (now - ctx.timers.output_check_start_ts >= DE_INDICATOR_OUTPUT_CHECK_MS)) {
        ctx.flags.evt_output_check = false;
    }

    if (ctx.flags.evt_output_usb && (now - ctx.timers.output_usb_start_ts >= DE_INDICATOR_OUTPUT_CHECK_MS)) {
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

    if (ctx.flags.capslock_valid && (now - ctx.timers.capslock_last_update_ts > 5000)) {
        ctx.flags.capslock_valid = false;
    }

}

// =====================
// STATE RESOLVER
// =====================

static de_indicator_state_t resolve_state_raw(int64_t now) {
    // Allow BLE visual feedback when:
    // - Not in USB mode
    // - OR recently switched BLE profile
    // - OR within grace window after BLE activity
    bool ble_visual_allowed = !ctx.data.is_usb_output ||
                              ctx.flags.evt_ble_switch ||
                              ((now - ctx.timers.ble_last_activity_ts) < DE_INDICATOR_BLE_VISUAL_GRACE_MS);

    if (ctx.flags.is_sleeping) {
        return DE_INDICATOR_STATE_SLEEP;
    }

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

    // if (ctx.flags.evt_layer) {
    //     return DE_INDICATOR_STATE_LAYER_EVENT;
    // }

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

    if (ctx.data.capslock_on) {
        return DE_INDICATOR_STATE_CAPSLOCK;
    }

    // Battery warnings
    int64_t time_since_last_warn = now - ctx.timers.battery_warn_last_ts;

    if (ctx.data.battery_percent <= 5) {
        // Chỉ bắt đầu chu kỳ Deadly mới khi đã đủ thời gian lặp
        if (time_since_last_warn >= DE_INDICATOR_BATTERY_DEADLY_INTERVAL_MS) {
            ctx.timers.battery_warn_last_ts = now;     // Bắt đầu chu kỳ mới
            return DE_INDICATOR_STATE_BATTERY_DEADLY;
        }
        // Nếu đang trong chu kỳ Deadly thì giữ state
        else if (ctx.state == DE_INDICATOR_STATE_BATTERY_DEADLY) {
            return DE_INDICATOR_STATE_BATTERY_DEADLY;
        }
    }
    else if (ctx.data.battery_percent <= 10) {
        if (time_since_last_warn >= DE_INDICATOR_BATTERY_CRITICAL_INTERVAL_MS) {
            ctx.timers.battery_warn_last_ts = now;
            return DE_INDICATOR_STATE_BATTERY_CRITICAL;
        }
        else if (ctx.state == DE_INDICATOR_STATE_BATTERY_CRITICAL) {
            return DE_INDICATOR_STATE_BATTERY_CRITICAL;
        }
    }
    else if (ctx.data.battery_percent <= 30) {
        if (time_since_last_warn >= DE_INDICATOR_BATTERY_LOW_INTERVAL_MS) {
            ctx.timers.battery_warn_last_ts = now;
            return DE_INDICATOR_STATE_BATTERY_LOW;
        }
        else if (ctx.state == DE_INDICATOR_STATE_BATTERY_LOW) {
            return DE_INDICATOR_STATE_BATTERY_LOW;
        }
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
    int64_t battery_check_start_ts_time = ctx.timers.battery_check_start_ts;
    switch (battery_percent) {
        case 0 ... 10:
            led_flash_window(LED_RED, now, battery_check_start_ts_time, DE_INDICATOR_BATTERY_CHECK_MS);
            break;
        case 11 ... 50:
            led_flash_window(LED_YELLOW, now, battery_check_start_ts_time, DE_INDICATOR_BATTERY_CHECK_MS);
            break;
        case 51 ... 100:
            led_flash_window(LED_GREEN, now, battery_check_start_ts_time, DE_INDICATOR_BATTERY_CHECK_MS);
            break;
        default:
            led_flash_window(LED_MAGENTA, now, battery_check_start_ts_time, DE_INDICATOR_BATTERY_CHECK_MS);
            break;
    }
}

static void render_battery_warning(int64_t now, de_indicator_state_t state) {
    int64_t start_time = ctx.timers.battery_warn_last_ts;
    int64_t elapsed = now - start_time;

    uint32_t flash_duration_ms = 0;
    uint32_t full_period_ms = 0;        // Chu kỳ đầy đủ (on + off)
    uint8_t  color = LED_RED;

    // Chọn thông số theo state
    switch (state) {
        case DE_INDICATOR_STATE_BATTERY_LOW:
            flash_duration_ms = DE_INDICATOR_BATTERY_LOW_DURATION_MS;      // 10000
            full_period_ms    = DE_INDICATOR_BATTERY_FLASH_PERIOD_LOW_MS;  // 800
            color = LED_YELLOW;
            break;

        case DE_INDICATOR_STATE_BATTERY_CRITICAL:
            flash_duration_ms = DE_INDICATOR_BATTERY_CRITICAL_DURATION_MS; // 10000
            full_period_ms    = DE_INDICATOR_BATTERY_FLASH_PERIOD_CRITICAL_MS; // 400
            color = LED_RED;
            break;

        case DE_INDICATOR_STATE_BATTERY_DEADLY:
            flash_duration_ms = DE_INDICATOR_BATTERY_DEADLY_DURATION_MS;   // liên tục
            full_period_ms    = DE_INDICATOR_BATTERY_FLASH_PERIOD_DEADLY_MS; // 200
            color = LED_RED;
            break;

        default:
            led_off_all();
            return;
    }

    // Nếu là Deadly hoặc chưa hết thời gian flash 10 giây
    if (flash_duration_ms > 0 && elapsed >= flash_duration_ms) {
        led_off_all();
        return;
    }

    // Cách tính ổn định và chính xác hơn
    uint32_t position_in_cycle = (uint32_t)(elapsed % full_period_ms);

    if (position_in_cycle < (full_period_ms / 2)) {
        led_set(color);        // Pha sáng (nửa đầu chu kỳ)
    } else {
        led_off_all();         // Pha tắt (nửa sau chu kỳ)
    }
}

static void render_indicator_state(de_indicator_state_t state, int64_t now) {

    if (ctx.flags.is_sleeping) {
        led_off_all();
        return;
    }

    switch (state) {
    case DE_INDICATOR_STATE_SLEEP:
    case DE_INDICATOR_STATE_IDLE:
        led_off_all();
        break;

    case DE_INDICATOR_STATE_OUTPUT_CHECK:
        if (ctx.data.is_usb_output) led_flash_window(LED_WHITE, now, ctx.timers.output_check_start_ts, DE_INDICATOR_OUTPUT_CHECK_MS);
        else led_flash_n(LED_BLUE, now, ctx.timers.output_check_start_ts, ctx.data.ble_profile_index + 1);
        break;
    
    case DE_INDICATOR_STATE_OUTPUT_USB:
        led_flash_window(LED_WHITE, now, ctx.timers.output_usb_start_ts, DE_INDICATOR_OUTPUT_CHECK_MS);
        break;

    case DE_INDICATOR_STATE_BATTERY_CHECK:
        render_battery_check(now);
        break;

    case DE_INDICATOR_STATE_BLE_SWITCH_EVENT:
        led_flash_double(LED_BLUE, now, ctx.timers.ble_switch_start_ts);
        break;

    case DE_INDICATOR_STATE_BOOT: 
        led_rainbow_cycle(now, DE_INDICATOR_BOOT_MS);
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

    // case DE_INDICATOR_STATE_BATTERY_DEADLY:
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

void de_indicator_process(void) {
    int64_t now = k_uptime_get();

    if (ctx.timers.boot_start_ts == 0) {
        ctx.flags.is_booting = true;
        ctx.timers.boot_start_ts = now;
        ctx.timers.state_enter_time = now;
        ctx.state = DE_INDICATOR_STATE_BOOT;
    }

    int64_t idle_time = now - ctx.timers.last_activity_time;
    clear_transient_flags(now);
    
    // Turn off LEDs before is_sleeping timeout
    #if IS_ENABLED(CONFIG_ZMK_SLEEP)
    if (!ctx.flags.is_sleeping &&
        !ctx.data.is_usb_output &&
        ctx.timers.last_activity_time != 0 &&
        idle_time > (CONFIG_ZMK_IDLE_SLEEP_TIMEOUT - DE_INDICATOR_PRE_SLEEP_MS)) {
            ctx.flags.is_sleeping = true;
            led_off_all();
        }
    #endif // IS_ENABLED(CONFIG_ZMK_SLEEP) 
        
    de_indicator_state_t state = resolve_state(now);
    render_indicator_state(state, now);
}

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
        de_indicator_ble_disabled();
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
    // Assume battery is chagring via USB
    if (ctx.data.is_usb_output) {
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
    // Push the last warn ts into the future to delay next warning cycle, giving user a grace period after they check the battery
    ctx.timers.battery_warn_last_ts = k_uptime_get() + 1800000ULL; 
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
            // de_indicator_ble_disabled();
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

    if (!debounce_ok(now, &ctx.timers.ble_last_activity_ts, DE_INDICATOR_BLE_EVENT_DEBOUNCE_MS)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

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

    ctx.timers.ble_last_activity_ts = now;

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
    bool ble_switch_recent = (now - ctx.timers.ble_last_activity_ts) < DE_INDICATOR_OUTPUT_CHECK_SUPPRESS_AFTER_BLE_MS;

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
        bool ble_recent_activity = (now - ctx.timers.ble_last_activity_ts < 50);
        
        bool no_ble_activity =
            !ctx.flags.ble_is_pairing &&
            !ctx.flags.ble_is_connecting &&
            !ctx.flags.ble_is_connected &&
            !ble_recent_activity;

        if (no_ble_activity && !ctx.flags.evt_output_check) {
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
    ctx.timers.capslock_last_update_ts = k_uptime_get();

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

    if (ctx.flags.is_sleeping) {
        led_off_all();
        return; 
    }

    de_indicator_state_t state = resolve_state(now);
    render_indicator_state(state, now);
}

static void de_indicator_update_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    de_indicator_update();
    
    // Dynamic Polling 
    int32_t next_delay = DE_INDICATOR_UPDATE_INTERVAL_MS; // Default 50ms
    
    // if (ctx.state == DE_INDICATOR_STATE_IDLE || 
    //     ctx.state == DE_INDICATOR_STATE_CAPSLOCK ||
    //     ctx.flags.is_sleeping) {
    //     next_delay = 500; 
    // }
    
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
    
    led_off_all();

    ctx.timers.battery_warn_last_ts = now;
    
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
