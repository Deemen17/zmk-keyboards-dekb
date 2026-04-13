#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/hid_indicators.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/split/bluetooth/peripheral.h>

#include <math.h>

#define NUMLOCK_BIT BIT(0)
#define CAPSLOCK_BIT BIT(1)
#define SCROLLLOCK_BIT BIT(2)

// Layer colors (RGB bits: bit0=Red, bit1=Green, bit2=Blue)
#define LAYER_0_COLOR 0b001  // Red
#define LAYER_1_COLOR 0b010  // Green
#define LAYER_2_COLOR 0b100  // Blue
#define LAYER_3_COLOR 0b110  // Cyan

// Battery level colors
#define BATTERY_CRITICAL_COLOR 0b001  // Red (0-20%)
#define BATTERY_LOW_COLOR 0b011       // Yellow (20-40%)
#define BATTERY_MID_COLOR 0b011       // Yellow (40-60%)
#define BATTERY_HIGH_COLOR 0b010      // Green (60-100%)
#define BATTERY_UNAVAILABLE_COLOR 0b101  // Magenta (unavailable)

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LED_GPIO_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(indicator_r)),
             "An alias for a red LED is not found for RGBLED_WIDGET");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(indicator_g)),
             "An alias for a green LED is not found for RGBLED_WIDGET");
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(indicator_b)),
             "An alias for a blue LED is not found for RGBLED_WIDGET");

// GPIO-based LED device and indices of red/green/blue LEDs inside its DT node
static const struct device *led_dev = DEVICE_DT_GET(LED_GPIO_NODE_ID);
static const uint8_t led_idx[] = {DT_NODE_CHILD_IDX(DT_ALIAS(indicator_r)),
                                  DT_NODE_CHILD_IDX(DT_ALIAS(indicator_g)),
                                  DT_NODE_CHILD_IDX(DT_ALIAS(indicator_b))};

struct indicator_state_t {
    uint8_t keylock;
    uint8_t connection;
    uint8_t active_device;
    uint8_t battery;
    uint8_t battery_level_index;  // 0-4 for 5 battery levels
    bool battery_available;       // true when battery reading is valid
    uint8_t layer;
    uint8_t flash_times;
    bool show_battery_on_demand;
    bool show_layer_on_demand;
    uint32_t show_battery_until_ms;
    uint32_t show_layer_until_ms;
} indicator_state;

static void set_indicator_color(uint8_t bits) {
    static uint8_t last_bits = 0;
    if (bits != last_bits) {
        for (uint8_t pos = 0; pos < 3; pos++) {
            if (bits & (1 << pos)) {
                led_on(led_dev, led_idx[pos]);
            } else {
                led_off(led_dev, led_idx[pos]);
            }
        }
        last_bits = bits;
    }
}

static uint8_t get_battery_level_index(uint8_t battery_percent) {
    if (battery_percent < 20)
        return 0;  // 0-20%
    else if (battery_percent < 40)
        return 1;  // 20-40%
    else if (battery_percent < 60)
        return 2;  // 40-60%
    else if (battery_percent < 80)
        return 3;  // 60-80%
    else
        return 4;  // 80-100%
}

static uint8_t get_layer_color(uint8_t layer) {
    static const uint8_t layer_colors[] = {LAYER_0_COLOR, LAYER_1_COLOR, LAYER_2_COLOR, LAYER_3_COLOR};
    if (layer >= 4)
        return LAYER_0_COLOR;
    return layer_colors[layer];
}

static void get_lock_indicators(void) {
    uint8_t state = zmk_hid_indicators_get_current_profile();
    LOG_DBG("LOCK LEDS: %d", state);
    indicator_state.keylock = state;
}

static int hid_indicators_status_update_cb(const zmk_event_t *eh) { get_lock_indicators(); return ZMK_EV_EVENT_BUBBLE; }

ZMK_LISTENER(widget_hid_indicators_status, hid_indicators_status_update_cb);
ZMK_SUBSCRIPTION(widget_hid_indicators_status, zmk_hid_indicators_changed);

struct blink_item {
    uint16_t duration_ms;
    uint16_t sleep_ms;
    uint8_t count;
};

K_MSGQ_DEFINE(led_msgq, sizeof(struct blink_item), 16, 1);

static bool indicator_enabled = true;

void de_indicator_off(void) {
    indicator_enabled = false;
    set_indicator_color(0);
}

void de_indicator_on(void) {
    indicator_enabled = true;
}

bool de_indicator_is_enabled(void) {
    return indicator_enabled;
}

void de_indicator_show_battery_on_demand(void) {
    if (indicator_enabled) {
        indicator_state.show_battery_on_demand = true;
        indicator_state.show_battery_until_ms = k_uptime_get() + 500;  
    }
}

void de_indicator_show_layer_on_demand(void) {
    if (indicator_enabled) {
        indicator_state.show_layer_on_demand = true;
        indicator_state.show_layer_until_ms = k_uptime_get() + 500;  
    }
}

static void ble_active_profile_update(void) {
    uint8_t profile_index = zmk_ble_active_profile_index();
    if (profile_index > 3)
        return;
    indicator_state.active_device = profile_index;
    if (zmk_ble_active_profile_is_connected()) {
        indicator_state.connection = 2;
        indicator_state.flash_times = 3 * 4;
        //} else if (zmk_ble_active_profile_is_open()) {
    } else {
        indicator_state.connection = 1;
        indicator_state.flash_times = 15 * 4;
    }
    LOG_DBG("Device_BT%d, Connection State: %d", indicator_state.active_device + 1,
            indicator_state.connection);
    return;
}

static int ble_active_profile_update_cb(const zmk_event_t *eh) { ble_active_profile_update(); return ZMK_EV_EVENT_BUBBLE; }

ZMK_LISTENER(ble_active_profile_listener, ble_active_profile_update_cb);
ZMK_SUBSCRIPTION(ble_active_profile_listener, zmk_ble_active_profile_changed);

static void layer_state_update(void) {
    uint8_t highest_layer = zmk_keymap_highest_layer_active();
    if (highest_layer >= 4) {
        highest_layer = 3;
    }
    indicator_state.layer = highest_layer;
    LOG_DBG("Layer changed to: %d", highest_layer);
}

static int layer_state_changed_cb(const zmk_event_t *eh) {
    layer_state_update();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(layer_state_listener, layer_state_changed_cb);
ZMK_SUBSCRIPTION(layer_state_listener, zmk_layer_state_changed);

#include <zmk/events/keycode_state_changed.h>
static int zmk_handle_keycode_user(struct zmk_keycode_state_changed *event) {
    zmk_key_t key = event->keycode;
    LOG_DBG("key 0x%X", key);
    if (key == 0xAB) {
        ble_active_profile_update();
        return ZMK_EV_EVENT_HANDLED;
    }
    
    if (key == 0xAC) {
        de_indicator_off();
        return ZMK_EV_EVENT_HANDLED;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

static int keycode_user_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *kc_state;

    kc_state = as_zmk_keycode_state_changed(eh);

    if (kc_state != NULL) {
        return zmk_handle_keycode_user(kc_state);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(keycode_user, keycode_user_listener);
ZMK_SUBSCRIPTION(keycode_user, zmk_keycode_state_changed);

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
static int led_battery_listener_cb(const zmk_event_t *eh) {
    uint8_t battery_level = as_zmk_battery_state_changed(eh)->state_of_charge;
    indicator_state.battery = battery_level;
    indicator_state.battery_level_index = get_battery_level_index(battery_level);
    indicator_state.battery_available = true;  // Mark battery as available
    LOG_DBG("Battery level: %d%% (index: %d)", battery_level, indicator_state.battery_level_index);
    return 0;
}

ZMK_LISTENER(led_battery_listener, led_battery_listener_cb);
ZMK_SUBSCRIPTION(led_battery_listener, zmk_battery_state_changed);
#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

void led_process_thread(void *p1, void *p2, void *p3) {
    while (true) {
        k_sleep(K_MSEC(20));
        static uint16_t led_timer_steps = 0;
        led_timer_steps++;

        if (!indicator_enabled) {
            set_indicator_color(0);
            continue;
        }

        // Priority 1: Show connection status
        if (indicator_state.connection > 0) {
            static uint8_t profile_color_bits[3] = {0b011, 0b110, 0b101};
            if (indicator_state.active_device >= 3) {
                continue;
            }

            if ((led_timer_steps & 0xf) == 0xf) {
                indicator_state.flash_times--;
                uint8_t color_bits = profile_color_bits[indicator_state.active_device];
                switch ((led_timer_steps >> 4) & 0x3) {
                case 0:
                    set_indicator_color(0);
                    break;
                case 1:
                    set_indicator_color(color_bits);
                    break;
                case 2:
                    if (indicator_state.connection != 2)
                        set_indicator_color(0);
                    break;
                case 3:
                    if (indicator_state.connection != 2) {
                        bt_addr_le_t *addr = zmk_ble_active_profile_addr();
                        if (bt_addr_le_eq(addr, BT_ADDR_LE_ANY))
                            set_indicator_color(0b001); // red color
                        else
                            set_indicator_color(0b100); // blue color
                    }
                    break;
                }
                if (indicator_state.flash_times == 0)
                    indicator_state.connection = 0;
            }
        }
        // Priority 2: Show battery level (5 levels) or unavailable indicator
        else if (!indicator_state.battery_available) {
            // Magenta blink if battery is unavailable
            if ((led_timer_steps & 0x0f) < 0x08) {
                set_indicator_color(BATTERY_UNAVAILABLE_COLOR);
            } else {
                set_indicator_color(0);
            }
        }
        // Priority 2b: Show battery levels if available
        else if (indicator_state.battery_level_index < 5) {
            // Battery critical (0-20%): Fast red blink
            if (indicator_state.battery_level_index == 0) {
                if ((led_timer_steps & 0x0f) < 0x05) {
                    set_indicator_color(BATTERY_CRITICAL_COLOR);
                } else {
                    set_indicator_color(0);
                }
            }
            // Battery low (20-40%): Red solid
            else if (indicator_state.battery_level_index == 1) {
                set_indicator_color(BATTERY_LOW_COLOR);
            }
            // Battery mid (40-60%): Yellow solid
            else if (indicator_state.battery_level_index == 2) {
                set_indicator_color(BATTERY_MID_COLOR);
            }
            // Battery high (60-80%): Yellow-green blink
            else if (indicator_state.battery_level_index == 3) {
                if ((led_timer_steps & 0x1f) < 0x10) {
                    set_indicator_color(BATTERY_MID_COLOR);
                } else {
                    set_indicator_color(BATTERY_HIGH_COLOR);
                }
            }
            // Battery full (80-100%): Green solid
            else if (indicator_state.battery_level_index == 4) {
                set_indicator_color(BATTERY_HIGH_COLOR);
            }
        }
        // Priority 3: Show layer color
        else if (indicator_state.layer < 4) {
            set_indicator_color(get_layer_color(indicator_state.layer));
        }
        // Priority 4: Show CapsLock
        else if (indicator_state.keylock & CAPSLOCK_BIT) {
            set_indicator_color(0b111); // white color
        }
        else {
            set_indicator_color(0);
        }
    }
}

// Thread IDs and stacks
static k_tid_t led_process_tid;
static struct k_thread led_process_thread_data;
static k_thread_stack_t led_process_stack[1024];

static k_tid_t de_indicator_init_tid;
static struct k_thread de_indicator_init_thread_data;
static k_thread_stack_t de_indicator_init_stack[1024];

void de_indicator_init_thread(void) {
    indicator_state.connection = 1;
    indicator_state.battery = 0;
    indicator_state.battery_available = false;  // Battery unavailable until first event
    
    // Initialize layer state
    indicator_state.layer = zmk_keymap_highest_layer_active();
    if (indicator_state.layer >= 4) {
        indicator_state.layer = 0;
    }
    
    // Initialize battery level index
    indicator_state.battery_level_index = 0;
    
    LOG_DBG("Indicator initialized - Layer: %d, Battery available: %d", 
            indicator_state.layer, indicator_state.battery_available);
    
    // Start the LED processing thread
    led_process_tid = k_thread_create(&led_process_thread_data, led_process_stack,
                                      sizeof(led_process_stack),
                                      led_process_thread, NULL, NULL, NULL,
                                      K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
}

static int de_indicator_init(const struct device *dev) {
    ARG_UNUSED(dev);
    de_indicator_init_thread();
    return 0;
}

SYS_INIT(de_indicator_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);