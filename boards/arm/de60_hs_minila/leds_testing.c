#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/services/bas.h>

#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/hid_indicators.h>

#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/hid_indicators_changed.h>

#include <zmk/events/battery_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>

#include <zmk/keymap.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/split/bluetooth/central.h>
#include <zmk/events/button_event.h>

#include <zephyr/logging/log.h>

// LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);
LOG_MODULE_REGISTER(led_indicators, CONFIG_ZMK_LOG_LEVEL);

#define LED_GPIO_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)

// GPIO-based LED device
static const struct device *led_dev = DEVICE_DT_GET(LED_GPIO_NODE_ID);

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
};

// Timer and status for BLE advertising LED
static struct k_timer ble_adv_timer;
static bool ble_adv_led_on = false;
static int ble_adv_led_index = -1;

// Timer callback function to blink LED
static void ble_adv_timer_handler(struct k_timer *timer_id) {
    if (ble_adv_led_index >= 0) {
        if (ble_adv_led_on) {
            led_off(led_dev, ble_adv_led_index);
        } else {
            led_on(led_dev, ble_adv_led_index);
        }
        ble_adv_led_on = !ble_adv_led_on; // Toggle LED state
    }
}

// Caps Lock Indicator
static int update_caps_lock_led(const zmk_event_t *eh) {
    zmk_hid_indicators_t flags = zmk_hid_indicators_get_current_profile();
    unsigned int capsBit = 1 << (HID_USAGE_LED_CAPS_LOCK - 1);

    if (flags & capsBit) {
        led_on(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_caps)));
    } else {
        led_off(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_caps)));
    }

    return 0;
}

// Output Selection Indicators

// Set the status LED based on the current state
static void set_led_for_endpoint(struct output_status_state state) {
    // Turn off all LEDs before turning on the corresponding LED
    led_off(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_usb)));
    led_off(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_0)));
    led_off(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_1)));
    led_off(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_2)));

    switch (state->selected_endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        // Turn on USB LED
        led_on(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_usb)));
        break;
    case ZMK_TRANSPORT_BLE:
        if (state->active_profile_bonded) {
            if (state->active_profile_connected) {
                // Turn on connected BLE LED
                switch (state->selected_endpoint.ble.profile_index) {
                case 0:
                    led_on(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_0)));
                    break;
                case 1:
                    led_on(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_1)));
                    break;
                case 2:
                    led_on(led_dev, DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_2)));
                    break;
                }
            } else {
                // BLE not connected - could leave empty or add logic
            }
        } else {
            switch (state->selected_endpoint.ble.profile_index) {
            case 0:
                ble_adv_led_index = DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_0));
                break;
            case 1:
                ble_adv_led_index = DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_1));
                break;
            case 2:
                ble_adv_led_index = DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_2));
                break;
            }

            // Nếu profile chưa kết nối, nháy LED
            if (!zmk_ble_active_profile_is_connected()) {
                k_timer_stop(&ble_adv_timer);
                k_timer_start(&ble_adv_timer, K_MSEC(500), K_MSEC(500)); // Nháy LED
            } else {
                // Nếu đã kết nối, sáng LED 5 giây rồi tắt
                led_on(ble_adv_led_index);
                k_timer_start(&ble_adv_timer, K_MSEC(5000), K_MSEC(0)); // Sáng 5 giây
            }
        }
        break;
    }
}

// Hàm cập nhật trạng thái LED khi có sự thay đổi endpoint
static int endpoint_changed_listener(const zmk_event_t *eh) {
    struct zmk_endpoint_instance selected_endpoint = zmk_endpoints_selected();
    set_led_for_endpoint(selected_endpoint);
    return 0;
}

// Lắng nghe sự kiện thay đổi Caps Lock
static int led_keylock_listener_cb(const zmk_event_t *eh) {
    update_caps_lock_led();
    return 0;
}

// Lắng nghe sự kiện khi người dùng nhấn nút BT_SEL
static int bt_sel_listener(const zmk_event_t *eh) {
    struct zmk_button_event *event = (struct zmk_button_event *)eh;
    if (event->action == ZMK_BUTTON_PRESS) {
        int profile = event->params.button_id; // Lấy profile từ nút nhấn
        if (profile >= 0 && profile <= 2) {
            // Bật LED tương ứng với profile được chọn trong 5 giây
            switch (profile) {
            case 0:
                ble_adv_led_index = DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_0));
                break;
            case 1:
                ble_adv_led_index = DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_1));
                break;
            case 2:
                ble_adv_led_index = DT_NODE_CHILD_IDX(DT_ALIAS(led_ble_2));
                break;
            default:
                return 0;
            }
            led_on(ble_adv_led_index);
            k_timer_start(&ble_adv_timer, K_MSEC(5000), K_MSEC(0)); // Sáng 5 giây rồi tắt
        }
    }
    return 0;
}

// Lắng nghe sự kiện chuyển chế độ OUT_TOG hoặc OUT_BLE
static int mode_change_listener(const zmk_event_t *eh) {
    struct zmk_button_event *event = (struct zmk_button_event *)eh;
    if (event->action == ZMK_BUTTON_PRESS) {
        if (event->params.button_id == ZMK_BUTTON_ID_OUT_TOG ||
            event->params.button_id == ZMK_BUTTON_ID_OUT_BLE) {
            // Tắt LED USB khi chuyển sang chế độ BLE
            led_off(LED_USB_PIN);
        }
    }
    return 0;
}

// Đăng ký listeners
ZMK_LISTENER(led_keylock_listener, led_keylock_listener_cb);
ZMK_LISTENER(endpoint_changed_listener, endpoint_changed_listener);
ZMK_LISTENER(bt_sel_listener, bt_sel_listener);
ZMK_LISTENER(mode_change_listener, mode_change_listener);

// Đăng ký các sự kiện
ZMK_SUBSCRIPTION(led_keylock_listener, zmk_hid_indicators_changed);
ZMK_SUBSCRIPTION(endpoint_changed_listener, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(bt_sel_listener, zmk_button_event);
ZMK_SUBSCRIPTION(mode_change_listener, zmk_button_event);

// LED initialization
static int leds_init(const struct device *device) {
    if (!device_is_ready(led_dev)) {
        return -ENODEV;
    }

    return 0;
}

// Run leds_init on boot
SYS_INIT(leds_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static struct k_timer ble_adv_timer = K_TIMER_INIT(ble_adv_timer_handler, NULL, NULL);
