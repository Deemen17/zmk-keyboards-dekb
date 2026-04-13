#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Public API for RGB LED indicator widget */

/**
 * @brief Turn off the LED indicator
 */
void de_indicator_off(void);

/**
 * @brief Enable the LED indicator
 */
void de_indicator_on(void);

/**
 * @brief Check if indicator is enabled
 * @return true if enabled, false otherwise
 */
bool de_indicator_is_enabled(void);

/**
 * @brief Force display battery level on demand
 * Triggers battery level indicator display for a short duration
 * 
 * Battery indicator:
 * - Magenta blink    : Battery unavailable (CONFIG_ZMK_BATTERY_REPORTING disabled or not initialized)
 * - Red fast blink   : 0-20% (critical)
 * - Yellow (Red+Green) : 20-40% (low)
 * - Yellow (Red+Green) : 40-60% (mid)
 * - Yellow/Green blink : 60-80% (high)
 * - Green            : 80-100% (full)
 */
void de_indicator_show_battery_on_demand(void);

/**
 * @brief Force display current layer on demand
 * Triggers layer indicator display for a short duration
 * 
 * Layer colors:
 * - Red  : Layer 0
 * - Green: Layer 1
 * - Blue : Layer 2
 * - Cyan : Layer 3
 */
void de_indicator_show_layer_on_demand(void);
