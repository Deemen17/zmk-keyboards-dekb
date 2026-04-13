#ifndef DE_INDICATOR_V2_H
#define DE_INDICATOR_V2_H

#include <stdbool.h>
#include <stdint.h>

// =====================
// COLOR BIT DEFINES
// =====================

#define LED_R 0b001
#define LED_G 0b010
#define LED_B 0b100

#define LED_RED LED_R
#define LED_GREEN LED_G
#define LED_BLUE LED_B

#define LED_YELLOW (LED_R | LED_G)
#define LED_CYAN (LED_G | LED_B)
#define LED_MAGENTA (LED_R | LED_B)
#define LED_WHITE (LED_R | LED_G | LED_B)
#define LED_OFF 0

// =====================
// STATE
// =====================

typedef enum {
    DE_INDICATOR_STATE_IDLE,
    DE_INDICATOR_STATE_SLEEP,

    DE_INDICATOR_STATE_BATTERY_CHECK,
    DE_INDICATOR_STATE_OUTPUT_CHECK,
    DE_INDICATOR_STATE_BLE_SWITCH_EVENT,
    DE_INDICATOR_STATE_LAYER_EVENT,
    DE_INDICATOR_STATE_BOOT,

    DE_INDICATOR_STATE_BLE_PAIRING,
    DE_INDICATOR_STATE_BLE_CONNECTING,
    DE_INDICATOR_STATE_BLE_CONNECTED,

    DE_INDICATOR_STATE_BATTERY_LOW,
    DE_INDICATOR_STATE_BATTERY_CRITICAL,

    DE_INDICATOR_STATE_CAPSLOCK,
} de_indicator_state_t;

// =====================
// PUBLIC API
// =====================

void de_indicator_init(void);
void de_indicator_update(void);

void de_indicator_set_color(uint8_t color_bits);
void de_indicator_blink_fast(uint8_t color, int64_t now);
void de_indicator_blink_slow(uint8_t color, int64_t now);
void de_indicator_flash(uint8_t color, int64_t now, int duration_ms);

// Triggers
void de_indicator_trigger_battery_check(void);
void de_indicator_trigger_output_check(bool is_usb_output);
void de_indicator_trigger_ble_switch(void);
void de_indicator_trigger_ble_profile_status(void);
void de_indicator_trigger_layer_event(uint8_t layer);

// BLE state updates
void de_indicator_ble_pairing(bool active);
void de_indicator_ble_connecting(bool active);
void de_indicator_ble_connected(void);

// System state updates
void de_indicator_set_battery(uint8_t percent);
void de_indicator_set_capslock(bool on);
void de_indicator_set_sleep(bool sleep);

#endif
