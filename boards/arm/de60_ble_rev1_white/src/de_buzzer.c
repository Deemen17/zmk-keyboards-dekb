#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <string.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define BUZZER_LOG_INF(...) LOG_INF("buzzer: " __VA_ARGS__)
#define BUZZER_LOG_DBG(...) LOG_DBG("buzzer: " __VA_ARGS__)
#define BUZZER_LOG_WRN(...) LOG_WRN("buzzer: " __VA_ARGS__)
#define BUZZER_LOG_ERR(...) LOG_ERR("buzzer: " __VA_ARGS__)

static inline bool buzzer_is_ready(void);
static inline bool buzzer_is_enabled(void);

#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <dt-bindings/zmk/hid_usage.h>
#include <zmk/hid.h>

#include "de_buzzer.h"

#define BUZZER_NODE DT_ALIAS(buzzer)

#if DT_NODE_HAS_STATUS(BUZZER_NODE, okay)

// Optimized buzzer configuration
#define BUZZER_THREAD_STACK_SIZE 1024
#define BUZZER_THREAD_PRIORITY K_PRIO_COOP(8)
#define BLE_MONITOR_INTERVAL_MS 3000
#define MAX_BLE_PROFILES 5

// HID keycode used to toggle the buzzer in firmware. Map this to any key in the keymap to
// enable/disable the buzzer without reflashing. Default is a vendor-reserved code to avoid
// conflicts with standard keys.
#ifndef DE_BUZZER_TOGGLE_KEYCODE
/* So sánh với mã HID đầy đủ (usage page + usage id), trùng với &kp F24 */
#define DE_BUZZER_TOGGLE_KEYCODE HID_USAGE_KEY_KEYBOARD_F24
#endif

// Musical note periods in nanoseconds (optimized for memory)
typedef struct {
    uint32_t period_ns;
    uint8_t duration_ms;
} buzzer_note_t;

// Pre-computed note frequencies for better performance
enum musical_notes {
    NOTE_C5 = 1908396, // 523.25Hz
    NOTE_D5 = 1700680, // 587.33Hz
    NOTE_E5 = 1515152, // 659.25Hz
    NOTE_F5 = 1431818, // 698.46Hz
    NOTE_G5 = 1275510, // 783.99Hz
    NOTE_A5 = 1136364, // 880.00Hz
    NOTE_B5 = 1013776, // 987.77Hz
    NOTE_C6 = 954198,  // 1046.50Hz
    NOTE_FS5 = 851064, // 739.99Hz (F#5)
    NOTE_GS5 = 758374, // 830.61Hz (G#5)
    NOTE_SILENT = 0    // Silence
};

// Buzzer state management with anti-spam protection
typedef struct {
    bool is_playing;
    bool hw_ready;
    bool enabled;
    uint8_t current_profile;
    bool connection_states[MAX_BLE_PROFILES];
    struct k_work_q work_queue;
    K_THREAD_STACK_MEMBER(work_stack, BUZZER_THREAD_STACK_SIZE);

    // Anti-spam protection
    uint32_t last_profile_change;  // Timestamp of last profile change
    uint32_t last_endpoint_change; // Timestamp of last endpoint change
    uint32_t last_sound_played;    // Timestamp of last sound played
    uint8_t spam_counter;          // Count rapid events
    bool spam_mode;                // In spam protection mode
} buzzer_state_t;

static buzzer_state_t buzzer_state = {0};
static const struct pwm_dt_spec pwm = PWM_DT_SPEC_GET(BUZZER_NODE);

#if IS_ENABLED(CONFIG_SETTINGS)
#define BUZZER_SETTINGS_PATH "dekb/buzzer"
#define BUZZER_SETTINGS_KEY_ENABLED "enabled"

static int buzzer_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                               void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, BUZZER_SETTINGS_KEY_ENABLED, &next) && !next) {
        uint8_t enabled = 1U;

        if (len != sizeof(enabled)) {
            return -EINVAL;
        }

        int rc = read_cb(cb_arg, &enabled, sizeof(enabled));
        if (rc < 0) {
            return rc;
        }

        buzzer_state.enabled = (enabled != 0U);
        BUZZER_LOG_INF("Restored buzzer state from settings: enabled=%d", buzzer_state.enabled);
        return 0;
    }

    return -ENOENT;
}

static int buzzer_settings_commit(void) {
    if (buzzer_state.hw_ready && !buzzer_state.enabled) {
        pwm_set_dt(&pwm, 0, 0);
        buzzer_state.is_playing = false;
    }

    BUZZER_LOG_INF("Applied buzzer settings after settings_load: enabled=%d", buzzer_state.enabled);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(dekb_buzzer, BUZZER_SETTINGS_PATH, NULL, buzzer_settings_set,
                               buzzer_settings_commit, NULL);

static int buzzer_save_enabled_state(void) {
    uint8_t enabled = buzzer_state.enabled ? 1U : 0U;
    int ret = settings_save_one(BUZZER_SETTINGS_PATH "/" BUZZER_SETTINGS_KEY_ENABLED, &enabled,
                                sizeof(enabled));
    if (ret < 0) {
        BUZZER_LOG_WRN("Failed to save buzzer state (%d)", ret);
    }
    return ret;
}
#else
static inline int buzzer_save_enabled_state(void) { return 0; }
#endif

static inline bool buzzer_is_ready(void) {
    return buzzer_state.hw_ready;
}

static inline bool buzzer_is_enabled(void) {
    return buzzer_state.enabled;
}

static inline bool buzzer_can_play(void) {
    return buzzer_is_ready() && buzzer_is_enabled();
}

static inline void buzzer_log_state(void) {
    BUZZER_LOG_DBG("state ready=%d enabled=%d spam_mode=%d", buzzer_state.hw_ready,
                   buzzer_state.enabled, buzzer_state.spam_mode);
}

// Anti-spam configuration
#define MIN_PROFILE_INTERVAL_MS 300  // Minimum 300ms between profile sounds
#define MIN_ENDPOINT_INTERVAL_MS 500 // Minimum 500ms between endpoint sounds
#define MIN_SOUND_INTERVAL_MS 100    // Minimum 100ms between any sounds
#define SPAM_THRESHOLD 5             // 5 rapid events = spam detected
#define SPAM_COOLDOWN_MS 2000        // 2 second cooldown in spam mode

// Anti-spam protection functions
static bool is_spam_detected(uint32_t now, uint32_t last_event, uint32_t min_interval) {
    if (now - last_event < min_interval) {
        buzzer_state.spam_counter++;
        if (buzzer_state.spam_counter >= SPAM_THRESHOLD) {
            buzzer_state.spam_mode = true;
            BUZZER_LOG_WRN("Spam detected! Activating protection mode");
            return true;
        }
    } else {
        buzzer_state.spam_counter = 0;
    }
    return false;
}

static bool should_block_sound(uint32_t now, uint32_t last_event, uint32_t min_interval) {
    // Block if in spam mode and cooldown not finished
    if (buzzer_state.spam_mode && (now - buzzer_state.last_sound_played < SPAM_COOLDOWN_MS)) {
        return true;
    }

    // Block if too frequent
    if (now - last_event < min_interval) {
        return true;
    }

    // Exit spam mode after cooldown
    if (buzzer_state.spam_mode && (now - buzzer_state.last_sound_played >= SPAM_COOLDOWN_MS)) {
        buzzer_state.spam_mode = false;
        buzzer_state.spam_counter = 0;
        BUZZER_LOG_INF("Exiting spam protection mode");
    }

    return false;
}

static void update_sound_timestamp(void) { buzzer_state.last_sound_played = k_uptime_get_32(); }

// Optimized tone playing with non-blocking approach
static inline void play_tone_async(uint32_t period_ns, uint8_t duration_ms) {
    if (!buzzer_can_play() || buzzer_state.is_playing || period_ns == NOTE_SILENT) {
        return;
    }

    buzzer_state.is_playing = true;
    pwm_set_dt(&pwm, period_ns, period_ns / 2U);
    k_msleep(duration_ms);
    pwm_set_dt(&pwm, 0, 0);
    buzzer_state.is_playing = false;
}

// Optimized sequence player with anti-spam protection
static void play_melody(const buzzer_note_t *melody, size_t note_count) {
    uint32_t now = k_uptime_get_32();

    if (!buzzer_can_play()) {
        BUZZER_LOG_DBG("play_melody blocked, buzzer_can_play=false");
        buzzer_log_state();
        return;
    }

    // Check if sound should be blocked due to spam
    if (should_block_sound(now, buzzer_state.last_sound_played, MIN_SOUND_INTERVAL_MS)) {
        BUZZER_LOG_DBG("Sound blocked due to anti-spam protection");
        return;
    }

    // Play melody if not blocked
    for (size_t i = 0; i < note_count && buzzer_state.hw_ready; i++) {
        if (buzzer_state.spam_mode) {
            // In spam mode, play shorter/quieter sounds
            uint8_t reduced_duration = melody[i].duration_ms / 2;
            play_tone_async(melody[i].period_ns, reduced_duration);
        } else {
            play_tone_async(melody[i].period_ns, melody[i].duration_ms);
        }
        k_msleep(10); // Shorter gap in spam mode
    }

    update_sound_timestamp();
} // Optimized profile sounds using structured melodies
static const buzzer_note_t profile_melodies[][5] = {
    // Profile 1 - Single note
    {{NOTE_C5, 100}, {NOTE_SILENT, 0}},

    // Profile 2 - Two harmonious notes
    {{NOTE_C5, 80}, {NOTE_E5, 80}, {NOTE_SILENT, 0}},

    // Profile 3 - Three note chord
    {{NOTE_C5, 70}, {NOTE_E5, 70}, {NOTE_G5, 70}, {NOTE_SILENT, 0}},

    // Profile 4 - Four note arpeggio
    {{NOTE_C5, 60}, {NOTE_E5, 60}, {NOTE_G5, 60}, {NOTE_C6, 60}, {NOTE_SILENT, 0}},

    // Profile 5 - Five note scale
    {{NOTE_C5, 50}, {NOTE_D5, 50}, {NOTE_E5, 50}, {NOTE_F5, 50}, {NOTE_G5, 50}}};

static inline size_t get_profile_melody_len(uint8_t profile_idx) {
    if (profile_idx >= ARRAY_SIZE(profile_melodies)) {
        return 0;
    }
    size_t len = 0;
    for (size_t i = 0; i < ARRAY_SIZE(profile_melodies[profile_idx]); i++) {
        if (profile_melodies[profile_idx][i].period_ns == NOTE_SILENT) {
            break;
        }
        len++;
    }
    return len;
}

static inline const buzzer_note_t *get_profile_melody(uint8_t profile_idx) {
    if (profile_idx >= ARRAY_SIZE(profile_melodies)) {
        return NULL;
    }
    return profile_melodies[profile_idx];
}

static const buzzer_note_t startup_melody[] = {
    {NOTE_C5, 120}, {NOTE_E5, 120}, {NOTE_G5, 120}, {NOTE_C6, 150}};

static const buzzer_note_t usb_melody[] = {{NOTE_FS5, 60}, {NOTE_GS5, 60}};

static const buzzer_note_t ble_melody[] = {{NOTE_G5, 90}, {NOTE_A5, 90}, {NOTE_B5, 90}};

static const buzzer_note_t ble_connected_melody[] = {
    {NOTE_C5, 70}, {NOTE_E5, 70}, {NOTE_G5, 70}, {NOTE_A5, 70}};

static const buzzer_note_t toggle_on_melody[] = {{NOTE_A5, 70}};
static const buzzer_note_t toggle_off_melody[] = {{NOTE_D5, 70}};

// Work queue functions for non-blocking audio
static void profile_sound_work(struct k_work *work);
static void system_sound_work(struct k_work *work);
static void connection_sound_work(struct k_work *work);

K_WORK_DEFINE(profile_work, profile_sound_work);
K_WORK_DEFINE(system_work, system_sound_work);
K_WORK_DEFINE(connection_work, connection_sound_work);

static uint8_t pending_profile = 0;
static uint8_t pending_system_sound = 0; // 1=startup, 2=usb, 3=ble

// Work queue implementations for non-blocking audio
static void profile_sound_work(struct k_work *work) {
    BUZZER_LOG_INF("profile_sound_work executing for pending_profile=%d", pending_profile);
    if (pending_profile < 5) {
        size_t melody_len = 0;
        // Calculate melody length
        for (int i = 0; i < 5; i++) {
            if (profile_melodies[pending_profile][i].period_ns == NOTE_SILENT) {
                break;
            }
            melody_len++;
        }
        BUZZER_LOG_INF("Playing profile melody %d with %d notes", pending_profile, melody_len);
        play_melody(profile_melodies[pending_profile], melody_len);
        BUZZER_LOG_INF("Profile %d sound played", pending_profile + 1);
    } else {
        BUZZER_LOG_WRN("Invalid pending_profile: %d", pending_profile);
    }
}

static void system_sound_work(struct k_work *work) {
    switch (pending_system_sound) {
    case 1: // Startup
        play_melody(startup_melody, ARRAY_SIZE(startup_melody));
        BUZZER_LOG_INF("Startup sound played");
        break;
    case 2: // USB
        play_melody(usb_melody, ARRAY_SIZE(usb_melody));
        BUZZER_LOG_INF("USB sound played");
        break;
    case 3: // BLE
        play_melody(ble_melody, ARRAY_SIZE(ble_melody));
        BUZZER_LOG_INF("BLE sound played");
        break;
    }
    pending_system_sound = 0;
}

static void connection_sound_work(struct k_work *work) {
    play_melody(ble_connected_melody, ARRAY_SIZE(ble_connected_melody));
    BUZZER_LOG_INF("BLE connected sound played");
}

// Anti-spam protected public interface functions
static inline void play_profile_sound(uint8_t profile_idx) {
    uint32_t now = k_uptime_get_32();

    BUZZER_LOG_INF("play_profile_sound called: profile_idx=%d, hw_ready=%d, enabled=%d", profile_idx, buzzer_is_ready(), buzzer_is_enabled());

    if (profile_idx >= ARRAY_SIZE(profile_melodies) || !buzzer_can_play()) {
        BUZZER_LOG_WRN("Profile sound blocked: profile_idx=%d, ready=%d, enabled=%d", profile_idx, buzzer_is_ready(), buzzer_is_enabled());
        return;
    }

    // Check for profile spam
    if (is_spam_detected(now, buzzer_state.last_profile_change, MIN_PROFILE_INTERVAL_MS)) {
        BUZZER_LOG_WRN("Profile change spam detected");
        return;
    }

    if (should_block_sound(now, buzzer_state.last_profile_change, MIN_PROFILE_INTERVAL_MS)) {
        BUZZER_LOG_WRN("Profile sound blocked - too frequent");
        return;
    }

    buzzer_state.last_profile_change = now;
    size_t melody_len = get_profile_melody_len(profile_idx);
    const buzzer_note_t *melody = get_profile_melody(profile_idx);
    if (!melody || melody_len == 0) {
        BUZZER_LOG_WRN("Profile %d has no melody", profile_idx);
        return;
    }

    BUZZER_LOG_INF("Playing profile melody %d with %d notes directly", profile_idx, melody_len);
    play_melody(melody, melody_len);
    BUZZER_LOG_INF("Profile %d sound played directly", profile_idx + 1);
}

static inline void play_startup_sound(void) {
    if (buzzer_state.hw_ready && buzzer_state.enabled) {
        pending_system_sound = 1;
        k_work_submit_to_queue(&buzzer_state.work_queue, &system_work);
    }
}

static inline void play_usb_sound(void) {
    uint32_t now = k_uptime_get_32();

    if (!buzzer_state.hw_ready || !buzzer_state.enabled) {
        return;
    }

    if (should_block_sound(now, buzzer_state.last_endpoint_change, MIN_ENDPOINT_INTERVAL_MS)) {
        BUZZER_LOG_DBG("USB sound blocked - too frequent");
        return;
    }

    buzzer_state.last_endpoint_change = now;
    pending_system_sound = 2;
    k_work_submit_to_queue(&buzzer_state.work_queue, &system_work);
}

static inline void play_ble_sound(void) {
    uint32_t now = k_uptime_get_32();

    if (!buzzer_state.hw_ready || !buzzer_state.enabled) {
        return;
    }

    if (should_block_sound(now, buzzer_state.last_endpoint_change, MIN_ENDPOINT_INTERVAL_MS)) {
        BUZZER_LOG_DBG("BLE sound blocked - too frequent");
        return;
    }

    buzzer_state.last_endpoint_change = now;
    pending_system_sound = 3;
    k_work_submit_to_queue(&buzzer_state.work_queue, &system_work);
}

static inline void play_ble_connected_sound(void) {
    uint32_t now = k_uptime_get_32();

    if (!buzzer_state.hw_ready || !buzzer_state.enabled) {
        return;
    }

    // Connection sounds are less restricted but still have basic protection
    if (should_block_sound(now, buzzer_state.last_sound_played, MIN_SOUND_INTERVAL_MS)) {
        BUZZER_LOG_DBG("BLE connected sound blocked - too frequent");
        return;
    }

    k_work_submit_to_queue(&buzzer_state.work_queue, &connection_work);
}

// Optimized event listeners
static int buzzer_listener(const zmk_event_t *eh) {
    const struct zmk_ble_active_profile_changed *profile_ev = as_zmk_ble_active_profile_changed(eh);
    if (!profile_ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    BUZZER_LOG_INF("buzzer_listener profile %d", profile_ev->index);

    switch (profile_ev->index) {
        case 0:
            play_profile_sound(0);
            break;
        case 1:
            play_profile_sound(1);
            break;
        case 2:
            play_profile_sound(2);
            break;
        case 3:
            play_profile_sound(3);
            break;
        case 4:
            play_profile_sound(4);
            break;
        default:
            break;
    }

    return ZMK_EV_EVENT_HANDLED;
}

static int endpoint_listener(const zmk_event_t *eh) {
    const struct zmk_endpoint_changed *endpoint_ev = as_zmk_endpoint_changed(eh);
    if (!endpoint_ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Check endpoint type using transport field
    if (endpoint_ev->endpoint.transport == ZMK_TRANSPORT_USB) {
        play_usb_sound();
        BUZZER_LOG_DBG("Endpoint: USB");
    } else if (endpoint_ev->endpoint.transport == ZMK_TRANSPORT_BLE) {
        play_ble_sound();
        BUZZER_LOG_DBG("Endpoint: BLE");
    }

    return ZMK_EV_EVENT_BUBBLE;
}

// Keycode listener to flip the software buzzer switch
static int buzzer_toggle_key_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *kc_ev = as_zmk_keycode_state_changed(eh);
    if (!kc_ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    BUZZER_LOG_INF("buzzer_toggle_key_listener: keycode=0x%08x state=%d", kc_ev->keycode, kc_ev->state);

    if (!kc_ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (kc_ev->keycode == DE_BUZZER_TOGGLE_KEYCODE) {
        de_buzzer_toggle();
        BUZZER_LOG_INF("de_buzzer_toggle called, new state=%d", buzzer_state.enabled);
        return ZMK_EV_EVENT_HANDLED;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

static void buzzer_play_toggle_feedback(bool enable) {
    if (!buzzer_state.hw_ready) {
        return;
    }

    /* Temporarily ignore enabled flag so we can play the acknowledgement tone even when turning off */
    bool previous_enabled = buzzer_state.enabled;
    buzzer_state.enabled = true;

    const buzzer_note_t *melody = enable ? toggle_on_melody : toggle_off_melody;
    BUZZER_LOG_INF("Playing toggle feedback sound, enable=%d", enable);
    play_melody(melody, 1);

    buzzer_state.enabled = previous_enabled;
}

void de_buzzer_set_enabled(bool enable) {
    if (!buzzer_state.hw_ready) {
        buzzer_state.enabled = enable;
        (void)buzzer_save_enabled_state();
        return;
    }

    if (buzzer_state.enabled == enable) {
        return;
    }

    if (!enable) {
        /* Stop any ongoing tone */
        pwm_set_dt(&pwm, 0, 0);
        buzzer_state.is_playing = false;
    }

    buzzer_play_toggle_feedback(enable);
    buzzer_state.enabled = enable;
    BUZZER_LOG_INF("Buzzer %s via software toggle", enable ? "enabled" : "disabled");
    (void)buzzer_save_enabled_state();
}

void de_buzzer_toggle(void) { de_buzzer_set_enabled(!buzzer_state.enabled); }

bool de_buzzer_is_enabled(void) { return buzzer_state.enabled; }

void de_buzzer_test_set_hw_ready(bool ready) { buzzer_state.hw_ready = ready; }

void de_buzzer_test_reset_state(void) {
    buzzer_state.is_playing = false;
    buzzer_state.hw_ready = false;
    buzzer_state.enabled = false;
    buzzer_state.current_profile = 0;
    memset(buzzer_state.connection_states, 0, sizeof(buzzer_state.connection_states));
    buzzer_state.last_profile_change = 0;
    buzzer_state.last_endpoint_change = 0;
    buzzer_state.last_sound_played = 0;
    buzzer_state.spam_counter = 0;
    buzzer_state.spam_mode = false;
}

// Optimized BLE connection monitoring
static void ble_connection_monitor(struct k_work *work) {
    bool state_changed = false;

    // Check only the active profile for connection state
    uint8_t active_profile = zmk_ble_active_profile_index();
    bool current_state = zmk_ble_active_profile_is_connected();

    // Update current profile tracking
    buzzer_state.current_profile = active_profile;

    // Check for new connection on active profile
    if (current_state && !buzzer_state.connection_states[active_profile]) {
        play_ble_connected_sound();
        BUZZER_LOG_INF("BLE profile %d connected", active_profile);
        state_changed = true;
    }

    // Update connection state for active profile
    buzzer_state.connection_states[active_profile] = current_state;

    if (state_changed) {
        BUZZER_LOG_DBG("BLE connection states updated");
    }
}

K_WORK_DEFINE(ble_monitor_work, ble_connection_monitor);

static void ble_monitor_timer_handler(struct k_timer *timer) {
    k_work_submit_to_queue(&buzzer_state.work_queue, &ble_monitor_work);
}

K_TIMER_DEFINE(ble_monitor_timer, ble_monitor_timer_handler, NULL);

// Optimized buzzer initialization
static int buzzer_init(void) {
    // Hardware check
    if (!device_is_ready(pwm.dev)) {
        BUZZER_LOG_ERR("PWM device %s not ready", pwm.dev->name);
        return -ENODEV;
    }

    buzzer_state.hw_ready = true;
    buzzer_state.enabled = true;

    // Initialize work queue with dedicated thread
    k_work_queue_init(&buzzer_state.work_queue);
    k_work_queue_start(&buzzer_state.work_queue, buzzer_state.work_stack,
                       K_THREAD_STACK_SIZEOF(buzzer_state.work_stack), BUZZER_THREAD_PRIORITY,
                       NULL);

    // Initialize BLE connection state tracking
    buzzer_state.current_profile = zmk_ble_active_profile_index();

    // Initialize connection states - only track active profile
    for (int i = 0; i < MAX_BLE_PROFILES; i++) {
        buzzer_state.connection_states[i] = false; // Initialize to false
    }

    // Set current active profile connection state
    buzzer_state.connection_states[buzzer_state.current_profile] =
        zmk_ble_active_profile_is_connected();

    // Delay for system stability
    k_msleep(300);

    // With CONFIG_SETTINGS, buzzer enabled state is applied after global settings_load() in main().
    // Avoid startup beeps here to prevent playing sound when persisted state is OFF.
#if !IS_ENABLED(CONFIG_SETTINGS)
    if (buzzer_state.enabled) {
        play_startup_sound();
        BUZZER_LOG_INF("Buzzer startup sound triggered");

        play_melody(toggle_on_melody, 1);
        k_msleep(100);
        play_melody(toggle_off_melody, 1);
        BUZZER_LOG_INF("Buzzer self-test tone sequence done");
    } else {
        pwm_set_dt(&pwm, 0, 0);
        buzzer_state.is_playing = false;
        BUZZER_LOG_INF("Buzzer startup muted (restored disabled state)");
    }
#else
    BUZZER_LOG_INF("Buzzer startup tones deferred until settings are applied");
#endif

    // Start BLE monitoring with optimized interval
    k_timer_start(&ble_monitor_timer, K_MSEC(BLE_MONITOR_INTERVAL_MS),
                  K_MSEC(BLE_MONITOR_INTERVAL_MS));

    BUZZER_LOG_INF("Buzzer system initialized (thread priority: %d)", BUZZER_THREAD_PRIORITY);
    return 0;
}

ZMK_LISTENER(buzzer_output_status, buzzer_listener)
ZMK_LISTENER(buzzer_endpoint_status, endpoint_listener)
ZMK_LISTENER(buzzer_toggle_key, buzzer_toggle_key_listener)

#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(buzzer_output_status, zmk_ble_active_profile_changed);
#endif

#if defined(CONFIG_ZMK_USB) || defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(buzzer_endpoint_status, zmk_endpoint_changed);
#endif

ZMK_SUBSCRIPTION(buzzer_toggle_key, zmk_keycode_state_changed);

// Initialize buzzer after system startup
SYS_INIT(buzzer_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif
