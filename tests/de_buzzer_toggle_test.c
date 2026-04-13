#include <zephyr/ztest.h>

#include "de_buzzer.h"

static void reset_and_disable_hw(void) {
    de_buzzer_test_reset_state();
    de_buzzer_test_set_hw_ready(false);
}

ZTEST(de_buzzer, toggle_updates_state_without_hw_ready) {
    reset_and_disable_hw();
    /* hw_ready is false so no PWM access is attempted */
    de_buzzer_set_enabled(true);
    zassert_true(de_buzzer_is_enabled(), "Expected enabled after explicit set");

    de_buzzer_toggle();
    zassert_false(de_buzzer_is_enabled(), "Toggle should disable");

    de_buzzer_toggle();
    zassert_true(de_buzzer_is_enabled(), "Toggle should enable again");
}

ZTEST(de_buzzer, reset_state_clears_flags) {
    reset_and_disable_hw();
    zassert_false(de_buzzer_is_enabled(), "Enabled flag should start false after reset");
}

ZTEST_SUITE(de_buzzer, NULL, NULL, NULL, NULL, NULL);
