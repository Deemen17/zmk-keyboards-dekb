#pragma once

#include <stdbool.h>

/* Public helpers for integrating buzzer toggle with keymap behaviors */
void de_buzzer_set_enabled(bool enable);
void de_buzzer_toggle(void);
bool de_buzzer_is_enabled(void);
void de_buzzer_test_set_hw_ready(bool ready);
void de_buzzer_test_reset_state(void);
