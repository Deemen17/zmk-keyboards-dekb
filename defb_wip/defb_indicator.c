#include "defb_indicator.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(defb_indicator, CONFIG_ZMK_LOG_LEVEL);

// ====================== DRIVER (GIỮ NGUYÊN CỦA BẠN) ======================

extern void led_off_all(void);
extern void led_set(uint8_t color);
extern void led_flash(uint8_t color);
extern void led_blink(uint8_t color);
extern void led_blink_fast(uint8_t color);

// ====================== RENDER ======================

void defb_indicator_render(defb_state_t state) {

    switch (state) {

    case DEFB_STATE_SLEEP:
        led_off_all();
        break;

    case DEFB_STATE_BOOT:
        led_flash(LED_WHITE);
        break;

    case DEFB_STATE_OUTPUT_USB:
        led_flash(LED_WHITE);
        break;

    case DEFB_STATE_OUTPUT_CHECK:
        led_flash(LED_BLUE);
        break;

    case DEFB_STATE_BLE_PAIRING:
        led_blink_fast(LED_BLUE);
        break;

    case DEFB_STATE_BLE_CONNECTING:
        led_blink(LED_BLUE);
        break;

    case DEFB_STATE_BLE_CONNECTED:
        led_set(LED_BLUE);
        break;

    case DEFB_STATE_BATTERY_LOW:
        led_blink(LED_RED);
        break;

    case DEFB_STATE_BATTERY_CRITICAL:
        led_blink_fast(LED_RED);
        break;

    case DEFB_STATE_LAYER:
        led_flash(LED_GREEN);
        break;

    case DEFB_STATE_CAPSLOCK:
        led_set(LED_WHITE);
        break;

    default:
        break;
    }
}