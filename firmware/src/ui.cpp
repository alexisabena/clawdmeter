#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include <time.h>
#include "logo.h"
#include "icons.h"
#include "hal/board_caps.h"
#include "settings.h"
#include "sd_recorder.h"
#include <ArduinoJson.h>
#include "hal/sound_hal.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_mono_32);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Usage screen widgets (single non-splash view) ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
// Clock fed by the daemon: base epoch (local wall-clock seconds) + the lv_tick at
// which it landed, so the title ticks forward locally between 60s payloads.
static long     clock_base_epoch = 0;
static uint32_t clock_base_ms = 0;
static int      clock_fmt = 24;   // 12 or 24, set from the daemon payload
static int      clock_last_min = -1;   // last rendered minute; avoids redrawing the title every tick
static lv_obj_t* usage_group;   // the two usage panels — shown when connected
static lv_obj_t* pair_group;    // pairing hint — shown when disconnected
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* panel_session = nullptr;
static lv_obj_t* panel_weekly = nullptr;
// Enterprise-only widgets inside panel_session
static lv_obj_t* lbl_session_pct_sym = nullptr;  // "%" in smaller font
static lv_obj_t* lbl_spending_desc = nullptr;     // "of your monthly budget"
static lv_obj_t* lbl_spending_status = nullptr;   // "Under pace" / "On pace" / "Over pace"
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle

#define THEME_GEMINI_ACCENT lv_color_hex(0x9b72ff)
static int active_model = 0; // 0 = Claude, 1 = Gemini
static UsageData cached_data = {};
static bool has_cached_data = false;

static lv_obj_t* approval_overlay = nullptr;
static lv_obj_t* lbl_approval_title = nullptr;
static lv_obj_t* lbl_approval_msg = nullptr;
static lv_obj_t* btn_allow_once = nullptr;
static lv_obj_t* btn_always_allow = nullptr;
static lv_obj_t* btn_deny = nullptr;

static lv_obj_t* wifi_modal = nullptr;
static lv_obj_t* lbl_wifi_modal_title = nullptr;
static lv_obj_t* lbl_wifi_modal_msg = nullptr;
static lv_obj_t* btn_wifi_modal_ok = nullptr;
static lv_obj_t* btn_wifi_modal_cancel = nullptr;
static bool wifi_modal_is_home_prompt = true;

static lv_obj_t* settings_shade = nullptr;
static lv_obj_t* lbl_settings_title = nullptr;
static lv_obj_t* lbl_settings_info = nullptr;
static lv_obj_t* btn_settings_home = nullptr;
static lv_obj_t* btn_settings_phone = nullptr;
static lv_obj_t* btn_mute_token = nullptr;
static lv_obj_t* btn_mute_permission = nullptr;
static lv_obj_t* lbl_mute_token = nullptr;
static lv_obj_t* lbl_mute_permission = nullptr;
static lv_obj_t* btn_settings_close = nullptr;

static lv_obj_t* recorder_container = nullptr;
static lv_obj_t* lbl_recorder_title = nullptr;
static lv_obj_t* lbl_sd_status = nullptr;
static lv_obj_t* btn_record = nullptr;
static lv_obj_t* lbl_btn_record = nullptr;
static lv_obj_t* btn_sync = nullptr;
static lv_obj_t* lbl_btn_sync = nullptr;
static lv_obj_t* list_memos = nullptr;
static lv_obj_t* lbl_rec_duration = nullptr;
static lv_timer_t* rec_timer = nullptr;

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Live-data freshness → which usage sub-view to show ----
// usage panels when data is flowing, an idle "Zzz" screen when the host is
// connected but no usage update landed within DATA_FRESH_MS, the pairing hint
// when BLE is down. Re-evaluated every loop in ui_tick_anim().
static lv_obj_t* idle_group;            // the "Zzz" idle screen
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static int       view_state = -1;       // -1 unknown / 0 pair / 1 idle / 2 usage
static const uint32_t DATA_FRESH_MS = 90000;  // usage counts as "live" within this window (daemon sends ~60s)

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);
static void panel_click_cb(lv_event_t* e);
static void logo_click_cb(lv_event_t* e);
static void settings_home_click_cb(lv_event_t* e);
static void settings_phone_click_cb(lv_event_t* e);
static void settings_shade_close_cb(lv_event_t* e);
static void mute_token_click_cb(lv_event_t* e);
static void mute_permission_click_cb(lv_event_t* e);
static void screen_gesture_cb(lv_event_t* e);
static void settings_shade_gesture_cb(lv_event_t* e);
static void build_settings_shade(lv_obj_t* parent);
static void update_settings_buttons_labels(void);

// Recorder forward decls
static void record_click_cb(lv_event_t* e);
static void sync_click_cb(lv_event_t* e);
static void recorder_timer_cb(lv_timer_t* t);
static void refresh_recordings_list(void);
static void build_recorder_container(lv_obj_t* parent);

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 16, 0);
    lv_obj_set_style_pad_right(panel, 16, 0);
    lv_obj_set_style_pad_top(panel, 12, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, 18, 0);
    lv_obj_set_style_pad_right(lbl, 18, 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_obj_set_style_pad_bottom(lbl, 6, 0);
    return lbl;
}

static void init_battery_icons(void) {
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen ========

static lv_obj_t* make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                                  lv_obj_t** out_pct, lv_obj_t** out_pill,
                                  lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_48, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y, L.content_w - 32, 24);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, &font_styrene_28, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);

    return panel;
}

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Wording matches the 3-second release gesture.
static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(pair_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(pair_group);
    lv_label_set_text(l1, "Wi-Fi Connecting");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t* l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "Connecting to network...");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 120);

    lv_obj_t* l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "Check wifi_credentials.h");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, 160);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decides
}

// Idle "Zzz" screen — shown when the host is connected but no usage update has
// landed recently (token expired, daemon down, host asleep…). Full-screen, like
// the pairing hint, so we never render hours-old numbers as if they were live.
static void build_idle_group(lv_obj_t* parent) {
    idle_group = lv_obj_create(parent);
    lv_obj_set_size(idle_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(idle_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(idle_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_group, 0, 0);
    lv_obj_set_style_pad_all(idle_group, 0, 0);
    lv_obj_clear_flag(idle_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    // A shrunk-down sleeping creature (reused claudepix "expression sleep" art)
    // sits between the header and the status line; the animated "Listening…"
    // status line carries the words, so no extra text is needed here.
    lv_obj_t* creature = splash_mini_create(idle_group, "expression sleep", 160);
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);  // update_view_state decides
}

#include "net_server.h"

static void allow_once_click_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        net_set_approval_status(1); // 1 = allow once
        if (approval_overlay) lv_obj_add_flag(approval_overlay, LV_OBJ_FLAG_HIDDEN);
        cached_data.agent_state = 0;
        ui_update(nullptr);
    }
}

static void always_allow_click_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        net_set_approval_status(2); // 2 = always allow
        if (approval_overlay) lv_obj_add_flag(approval_overlay, LV_OBJ_FLAG_HIDDEN);
        cached_data.agent_state = 0;
        ui_update(nullptr);
    }
}

static void deny_click_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        net_set_approval_status(3); // 3 = deny
        if (approval_overlay) lv_obj_add_flag(approval_overlay, LV_OBJ_FLAG_HIDDEN);
        cached_data.agent_state = 0;
        ui_update(nullptr);
    }
}

static void wifi_modal_ok_click_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        if (wifi_modal_is_home_prompt) {
            net_connect_home();
        } else {
            net_connect_phone();
        }
        if (wifi_modal) lv_obj_add_flag(wifi_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

static void wifi_modal_cancel_click_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        if (wifi_modal_is_home_prompt) {
            net_clear_home_detected(); // ignore home detected
        }
        if (wifi_modal) lv_obj_add_flag(wifi_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

static void logo_click_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        lv_event_stop_processing(e);
        if (settings_shade) {
            char info_buf[128];
            const char* net_name = net_is_using_home() ? "Home Wi-Fi" : "Phone Hotspot";
            const char* status_str = "Disconnected";
            net_state_t ns = net_get_state();
            if (ns == NET_STATE_CONNECTED) status_str = "Connected";
            else if (ns == NET_STATE_CONNECTING) status_str = "Connecting";
            
            snprintf(info_buf, sizeof(info_buf), 
                "Active Net: %s\nStatus: %s\nIP: %s", 
                net_name, status_str, net_get_ip_address());
            lv_label_set_text(lbl_settings_info, info_buf);
            update_settings_buttons_labels();
            
            lv_obj_remove_flag(settings_shade, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_y(settings_shade, 0); // Slide down
        }
    }
}

static void settings_home_click_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        net_connect_home();
        if (settings_shade) {
            lv_obj_set_y(settings_shade, -L.scr_h);
            lv_obj_add_flag(settings_shade, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void settings_phone_click_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        net_connect_phone();
        if (settings_shade) {
            lv_obj_set_y(settings_shade, -L.scr_h);
            lv_obj_add_flag(settings_shade, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void settings_shade_close_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        if (settings_shade) {
            lv_obj_set_y(settings_shade, -L.scr_h);
            lv_obj_add_flag(settings_shade, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void mute_token_click_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        settings_set_mute_token(!settings_get_mute_token());
        update_settings_buttons_labels();
    }
}

static void mute_permission_click_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        settings_set_mute_permission(!settings_get_mute_permission());
        update_settings_buttons_labels();
    }
}

static void screen_gesture_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_BOTTOM) {
            if (settings_shade) {
                char info_buf[128];
                const char* net_name = net_is_using_home() ? "Home Wi-Fi" : "Phone Hotspot";
                const char* status_str = "Disconnected";
                net_state_t ns = net_get_state();
                if (ns == NET_STATE_CONNECTED) status_str = "Connected";
                else if (ns == NET_STATE_CONNECTING) status_str = "Connecting";
                
                snprintf(info_buf, sizeof(info_buf), 
                    "Active Net: %s\nStatus: %s\nIP: %s", 
                    net_name, status_str, net_get_ip_address());
                lv_label_set_text(lbl_settings_info, info_buf);
                update_settings_buttons_labels();

                lv_obj_remove_flag(settings_shade, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_y(settings_shade, 0); // Slide down
            }
        }
    }
}

static void settings_shade_gesture_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_TOP) {
            if (settings_shade) {
                lv_obj_set_y(settings_shade, -L.scr_h);
                lv_obj_add_flag(settings_shade, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void update_settings_buttons_labels(void) {
    if (lbl_mute_token) {
        bool mute = settings_get_mute_token();
        lv_label_set_text(lbl_mute_token, mute ? "Token Alert: Silent" : "Token Alert: Chime");
        lv_obj_set_style_bg_color(btn_mute_token, mute ? COL_RED : COL_GREEN, 0);
    }
    if (lbl_mute_permission) {
        bool mute = settings_get_mute_permission();
        lv_label_set_text(lbl_mute_permission, mute ? "Permission: Silent" : "Permission: Chime");
        lv_obj_set_style_bg_color(btn_mute_permission, mute ? COL_RED : COL_GREEN, 0);
    }
}

static void build_settings_shade(lv_obj_t* parent) {
    settings_shade = lv_obj_create(parent);
    lv_obj_set_size(settings_shade, L.scr_w, L.scr_h);
    lv_obj_set_pos(settings_shade, 0, -L.scr_h); // Hidden above top screen
    lv_obj_set_style_bg_color(settings_shade, lv_color_hex(0x1a1a24), 0); // dark grey-blue
    lv_obj_set_style_bg_opa(settings_shade, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(settings_shade, 0, 0);
    lv_obj_set_style_pad_all(settings_shade, 15, 0);
    lv_obj_clear_flag(settings_shade, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(settings_shade, settings_shade_gesture_cb, LV_EVENT_GESTURE, NULL);

    lbl_settings_title = lv_label_create(settings_shade);
    lv_label_set_text(lbl_settings_title, "Settings");
    lv_obj_set_style_text_font(lbl_settings_title, &font_styrene_24, 0);
    lv_obj_set_style_text_color(lbl_settings_title, COL_TEXT, 0);
    lv_obj_align(lbl_settings_title, LV_ALIGN_TOP_MID, 0, 5);

    lbl_settings_info = lv_label_create(settings_shade);
    lv_label_set_text(lbl_settings_info, "");
    lv_label_set_long_mode(lbl_settings_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_settings_info, L.scr_w - 60);
    lv_obj_set_style_text_font(lbl_settings_info, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_settings_info, COL_DIM, 0);
    lv_obj_set_style_text_align(lbl_settings_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_settings_info, LV_ALIGN_TOP_MID, 0, 45);

    // Switch to Home button
    btn_settings_home = lv_button_create(settings_shade);
    lv_obj_set_size(btn_settings_home, L.scr_w - 40, 40);
    lv_obj_align(btn_settings_home, LV_ALIGN_TOP_MID, 0, 115);
    lv_obj_set_style_bg_color(btn_settings_home, lv_color_hex(0x282c34), 0);
    lv_obj_add_event_cb(btn_settings_home, settings_home_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* lbl_h = lv_label_create(btn_settings_home);
    lv_label_set_text(lbl_h, "Switch to Home Wi-Fi");
    lv_obj_set_style_text_font(lbl_h, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_h, COL_TEXT, 0);
    lv_obj_align(lbl_h, LV_ALIGN_CENTER, 0, 0);

    // Switch to Phone button
    btn_settings_phone = lv_button_create(settings_shade);
    lv_obj_set_size(btn_settings_phone, L.scr_w - 40, 40);
    lv_obj_align(btn_settings_phone, LV_ALIGN_TOP_MID, 0, 165);
    lv_obj_set_style_bg_color(btn_settings_phone, lv_color_hex(0x282c34), 0);
    lv_obj_add_event_cb(btn_settings_phone, settings_phone_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* lbl_p = lv_label_create(btn_settings_phone);
    lv_label_set_text(lbl_p, "Switch to Phone Hotspot");
    lv_obj_set_style_text_font(lbl_p, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_p, COL_TEXT, 0);
    lv_obj_align(lbl_p, LV_ALIGN_CENTER, 0, 0);

    // Mute Token Toggle button
    btn_mute_token = lv_button_create(settings_shade);
    lv_obj_set_size(btn_mute_token, L.scr_w - 40, 40);
    lv_obj_align(btn_mute_token, LV_ALIGN_TOP_MID, 0, 215);
    lv_obj_add_event_cb(btn_mute_token, mute_token_click_cb, LV_EVENT_CLICKED, NULL);
    lbl_mute_token = lv_label_create(btn_mute_token);
    lv_obj_set_style_text_font(lbl_mute_token, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_mute_token, COL_TEXT, 0);
    lv_obj_align(lbl_mute_token, LV_ALIGN_CENTER, 0, 0);

    // Mute Permission Toggle button
    btn_mute_permission = lv_button_create(settings_shade);
    lv_obj_set_size(btn_mute_permission, L.scr_w - 40, 40);
    lv_obj_align(btn_mute_permission, LV_ALIGN_TOP_MID, 0, 265);
    lv_obj_add_event_cb(btn_mute_permission, mute_permission_click_cb, LV_EVENT_CLICKED, NULL);
    lbl_mute_permission = lv_label_create(btn_mute_permission);
    lv_obj_set_style_text_font(lbl_mute_permission, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_mute_permission, COL_TEXT, 0);
    lv_obj_align(lbl_mute_permission, LV_ALIGN_CENTER, 0, 0);

    // Close button
    btn_settings_close = lv_button_create(settings_shade);
    lv_obj_set_size(btn_settings_close, L.scr_w - 40, 40);
    lv_obj_align(btn_settings_close, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(btn_settings_close, COL_RED, 0);
    lv_obj_add_event_cb(btn_settings_close, settings_shade_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* lbl_c = lv_label_create(btn_settings_close);
    lv_label_set_text(lbl_c, "Close");
    lv_obj_set_style_text_font(lbl_c, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_c, COL_TEXT, 0);
    lv_obj_align(lbl_c, LV_ALIGN_CENTER, 0, 0);

    update_settings_buttons_labels();
    lv_obj_add_flag(settings_shade, LV_OBJ_FLAG_HIDDEN);
}

static void build_wifi_modal(lv_obj_t* parent) {
    wifi_modal = lv_obj_create(parent);
    lv_obj_set_size(wifi_modal, L.scr_w - 40, L.scr_h / 2);
    lv_obj_align(wifi_modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(wifi_modal, lv_color_hex(0x1a1a24), 0); // dark grey-blue
    lv_obj_set_style_bg_opa(wifi_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_modal, 2, 0);
    lv_obj_set_style_border_color(wifi_modal, COL_ACCENT, 0);
    lv_obj_set_style_pad_all(wifi_modal, 15, 0);
    lv_obj_clear_flag(wifi_modal, LV_OBJ_FLAG_SCROLLABLE);

    lbl_wifi_modal_title = lv_label_create(wifi_modal);
    lv_label_set_text(lbl_wifi_modal_title, "Wi-Fi Alert");
    lv_obj_set_style_text_font(lbl_wifi_modal_title, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_wifi_modal_title, COL_TEXT, 0);
    lv_obj_align(lbl_wifi_modal_title, LV_ALIGN_TOP_MID, 0, 10);

    lbl_wifi_modal_msg = lv_label_create(wifi_modal);
    lv_label_set_text(lbl_wifi_modal_msg, "");
    lv_label_set_long_mode(lbl_wifi_modal_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_wifi_modal_msg, L.scr_w - 80);
    lv_obj_set_style_text_font(lbl_wifi_modal_msg, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_wifi_modal_msg, COL_DIM, 0);
    lv_obj_set_style_text_align(lbl_wifi_modal_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_wifi_modal_msg, LV_ALIGN_CENTER, 0, -10);

    btn_wifi_modal_ok = lv_button_create(wifi_modal);
    lv_obj_set_size(btn_wifi_modal_ok, L.scr_w / 2 - 40, 45);
    lv_obj_align(btn_wifi_modal_ok, LV_ALIGN_BOTTOM_LEFT, 5, -10);
    lv_obj_set_style_bg_color(btn_wifi_modal_ok, COL_GREEN, 0);
    lv_obj_add_event_cb(btn_wifi_modal_ok, wifi_modal_ok_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_ok = lv_label_create(btn_wifi_modal_ok);
    lv_label_set_text(lbl_ok, "Connect");
    lv_obj_set_style_text_font(lbl_ok, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_ok, COL_TEXT, 0);
    lv_obj_align(lbl_ok, LV_ALIGN_CENTER, 0, 0);

    btn_wifi_modal_cancel = lv_button_create(wifi_modal);
    lv_obj_set_size(btn_wifi_modal_cancel, L.scr_w / 2 - 40, 45);
    lv_obj_align(btn_wifi_modal_cancel, LV_ALIGN_BOTTOM_RIGHT, -5, -10);
    lv_obj_set_style_bg_color(btn_wifi_modal_cancel, COL_RED, 0);
    lv_obj_add_event_cb(btn_wifi_modal_cancel, wifi_modal_cancel_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_cancel = lv_label_create(btn_wifi_modal_cancel);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_set_style_text_font(lbl_cancel, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_cancel, COL_TEXT, 0);
    lv_obj_align(lbl_cancel, LV_ALIGN_CENTER, 0, 0);

    lv_obj_add_flag(wifi_modal, LV_OBJ_FLAG_HIDDEN);
}

static void build_approval_overlay(lv_obj_t* parent) {
    approval_overlay = lv_obj_create(parent);
    lv_obj_set_size(approval_overlay, L.scr_w, L.scr_h);
    lv_obj_set_pos(approval_overlay, 0, 0);
    lv_obj_set_style_bg_color(approval_overlay, lv_color_hex(0x1f0f0f), 0); // very dark red
    lv_obj_set_style_bg_opa(approval_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(approval_overlay, 0, 0);
    lv_obj_set_style_pad_all(approval_overlay, 20, 0);
    lv_obj_clear_flag(approval_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lbl_approval_title = lv_label_create(approval_overlay);
    lv_label_set_text(lbl_approval_title, "Permission Request");
    lv_obj_set_style_text_font(lbl_approval_title, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_approval_title, COL_TEXT, 0);
    lv_obj_align(lbl_approval_title, LV_ALIGN_TOP_MID, 0, 40);

    lbl_approval_msg = lv_label_create(approval_overlay);
    lv_label_set_text(lbl_approval_msg, "");
    lv_label_set_long_mode(lbl_approval_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_approval_msg, L.scr_w - 60);
    lv_obj_set_style_text_font(lbl_approval_msg, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_approval_msg, COL_DIM, 0);
    lv_obj_set_style_text_align(lbl_approval_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_approval_msg, LV_ALIGN_CENTER, 0, -20);

    btn_allow_once = lv_button_create(approval_overlay);
    lv_obj_set_size(btn_allow_once, L.scr_w - 40, 48);
    lv_obj_align(btn_allow_once, LV_ALIGN_BOTTOM_MID, 0, -135);
    lv_obj_set_style_bg_color(btn_allow_once, COL_GREEN, 0);
    lv_obj_add_event_cb(btn_allow_once, allow_once_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_btn_app = lv_label_create(btn_allow_once);
    lv_label_set_text(lbl_btn_app, "Allow Once");
    lv_obj_set_style_text_font(lbl_btn_app, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_btn_app, COL_TEXT, 0);
    lv_obj_align(lbl_btn_app, LV_ALIGN_CENTER, 0, 0);

    btn_always_allow = lv_button_create(approval_overlay);
    lv_obj_set_size(btn_always_allow, L.scr_w - 40, 48);
    lv_obj_align(btn_always_allow, LV_ALIGN_BOTTOM_MID, 0, -75);
    lv_obj_set_style_bg_color(btn_always_allow, COL_ACCENT, 0);
    lv_obj_add_event_cb(btn_always_allow, always_allow_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_btn_always = lv_label_create(btn_always_allow);
    lv_label_set_text(lbl_btn_always, "Always Allow");
    lv_obj_set_style_text_font(lbl_btn_always, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_btn_always, COL_TEXT, 0);
    lv_obj_align(lbl_btn_always, LV_ALIGN_CENTER, 0, 0);

    btn_deny = lv_button_create(approval_overlay);
    lv_obj_set_size(btn_deny, L.scr_w - 40, 48);
    lv_obj_align(btn_deny, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_set_style_bg_color(btn_deny, COL_RED, 0);
    lv_obj_add_event_cb(btn_deny, deny_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_btn_deny = lv_label_create(btn_deny);
    lv_label_set_text(lbl_btn_deny, "Deny");
    lv_obj_set_style_text_font(lbl_btn_deny, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_btn_deny, COL_TEXT, 0);
    lv_obj_align(lbl_btn_deny, LV_ALIGN_CENTER, 0, 0);

    lv_obj_add_flag(approval_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void recorder_gesture_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_RIGHT) { // Swipe right to go back to usage
            if (recorder_container && usage_container) {
                lv_obj_add_flag(recorder_container, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void usage_gesture_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_LEFT) { // Swipe left to open recorder
            if (usage_container && recorder_container) {
                lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(recorder_container, LV_OBJ_FLAG_HIDDEN);
                refresh_recordings_list();
            }
        }
    }
}

static void record_click_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        if (!sd_recorder_is_sd_mounted()) return;
        
        if (sd_recorder_is_recording()) {
            sd_recorder_stop();
            // Wait slightly for file to close and WAV header to update
            delay(100);
            uint32_t duration_sec = sd_recorder_get_record_duration_ms() / 1000;
            const char* fn = sd_recorder_get_active_filename();
            sd_recorder_add_history_entry(fn, duration_sec);
            refresh_recordings_list();
        } else {
            char filename[64];
            snprintf(filename, sizeof(filename), "insight_%u.wav", (uint32_t)time(nullptr));
            sd_recorder_start(filename);
        }
    }
}

static void sync_click_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        bool is_home = net_is_using_home() && (net_get_state() == NET_STATE_CONNECTED);
        if (is_home) {
            net_set_sync_requested(true);
            lv_label_set_text(lbl_btn_sync, "Syncing...");
        }
    }
}

static void recorder_timer_cb(lv_timer_t* t) {
    if (!recorder_container || lv_obj_has_flag(recorder_container, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    
    // Update SD status
    if (sd_recorder_is_sd_mounted()) {
        lv_label_set_text(lbl_sd_status, "SD Card: Ready");
        lv_obj_set_style_text_color(lbl_sd_status, COL_GREEN, 0);
    } else {
        lv_label_set_text(lbl_sd_status, "SD Card: Not Found");
        lv_obj_set_style_text_color(lbl_sd_status, COL_RED, 0);
    }

    // Update record state
    if (sd_recorder_is_recording()) {
        uint32_t dur = sd_recorder_get_record_duration_ms() / 1000;
        char dur_buf[32];
        snprintf(dur_buf, sizeof(dur_buf), "Recording... %02u:%02u", dur / 60, dur % 60);
        lv_label_set_text(lbl_rec_duration, dur_buf);
        lv_label_set_text(lbl_btn_record, "Stop");
        lv_obj_set_style_bg_color(btn_record, COL_RED, 0);
    } else {
        lv_label_set_text(lbl_rec_duration, "Idle");
        lv_label_set_text(lbl_btn_record, "Record");
        lv_obj_set_style_bg_color(btn_record, lv_color_hex(0x282c34), 0);
    }

    // Update sync button state
    bool is_home = net_is_using_home() && (net_get_state() == NET_STATE_CONNECTED);
    if (is_home) {
        if (!net_get_sync_requested()) {
            lv_label_set_text(lbl_btn_sync, "Sync to PC");
        }
        lv_obj_remove_state(btn_sync, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(btn_sync, COL_ACCENT, 0);
    } else {
        lv_label_set_text(lbl_btn_sync, "Sync (Home Wi-Fi Only)");
        lv_obj_add_state(btn_sync, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(btn_sync, lv_color_hex(0x404040), 0);
    }
}

static void refresh_recordings_list(void) {
    if (!list_memos) return;
    
    // Clear list children
    uint32_t cnt = lv_obj_get_child_count(list_memos);
    for (int i = (int)cnt - 1; i >= 0; i--) {
        lv_obj_t* child = lv_obj_get_child(list_memos, i);
        lv_obj_delete(child);
    }

    String history_raw = sd_recorder_get_history_json();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, history_raw);
    if (err) return;
    
    if (doc.is<JsonArray>()) {
        JsonArray arr = doc.as<JsonArray>();
        // Read in reverse order so newest is at the top
        for (int i = (int)arr.size() - 1; i >= 0; i--) {
            JsonObject obj = arr[i];
            const char* date_str = obj["date"] | "Unknown";
            int duration = obj["duration"] | 0;
            bool uploaded = obj["uploaded"] | false;
            
            char item_buf[64];
            snprintf(item_buf, sizeof(item_buf), "%s (%ds)", date_str, duration);
            
            lv_obj_t* btn = lv_list_add_button(list_memos, uploaded ? LV_SYMBOL_OK : LV_SYMBOL_AUDIO, item_buf);
            
            if (uploaded) {
                lv_obj_set_style_text_color(btn, COL_DIM, 0);
                lv_obj_set_style_opa(btn, LV_OPA_60, 0);
            } else {
                lv_obj_set_style_text_color(btn, COL_TEXT, 0);
            }
        }
    }
}

static void build_recorder_container(lv_obj_t* parent) {
    recorder_container = lv_obj_create(parent);
    lv_obj_set_size(recorder_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(recorder_container, 0, 0);
    lv_obj_set_style_bg_opa(recorder_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(recorder_container, 0, 0);
    lv_obj_set_style_pad_all(recorder_container, 0, 0);
    lv_obj_clear_flag(recorder_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(recorder_container, recorder_gesture_cb, LV_EVENT_GESTURE, NULL);

    lbl_recorder_title = lv_label_create(recorder_container);
    lv_label_set_text(lbl_recorder_title, "Insights");
    lv_obj_set_style_text_font(lbl_recorder_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_recorder_title, COL_TEXT, 0);
    lv_obj_align(lbl_recorder_title, LV_ALIGN_TOP_MID, 0, L.title_y);

    lbl_sd_status = lv_label_create(recorder_container);
    lv_label_set_text(lbl_sd_status, "SD Card: Ready");
    lv_obj_set_style_text_font(lbl_sd_status, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_sd_status, COL_GREEN, 0);
    lv_obj_align(lbl_sd_status, LV_ALIGN_TOP_MID, 0, L.title_y + 60);

    lbl_rec_duration = lv_label_create(recorder_container);
    lv_label_set_text(lbl_rec_duration, "Idle");
    lv_obj_set_style_text_font(lbl_rec_duration, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_rec_duration, COL_DIM, 0);
    lv_obj_align(lbl_rec_duration, LV_ALIGN_TOP_MID, 0, L.title_y + 85);

    // Record button
    btn_record = lv_button_create(recorder_container);
    lv_obj_set_size(btn_record, L.scr_w / 2 - 25, 45);
    lv_obj_align(btn_record, LV_ALIGN_TOP_LEFT, 20, L.title_y + 115);
    lv_obj_set_style_bg_color(btn_record, lv_color_hex(0x282c34), 0);
    lv_obj_add_event_cb(btn_record, record_click_cb, LV_EVENT_CLICKED, NULL);
    lbl_btn_record = lv_label_create(btn_record);
    lv_label_set_text(lbl_btn_record, "Record");
    lv_obj_set_style_text_font(lbl_btn_record, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_btn_record, COL_TEXT, 0);
    lv_obj_align(lbl_btn_record, LV_ALIGN_CENTER, 0, 0);

    // Sync button
    btn_sync = lv_button_create(recorder_container);
    lv_obj_set_size(btn_sync, L.scr_w / 2 - 25, 45);
    lv_obj_align(btn_sync, LV_ALIGN_TOP_RIGHT, -20, L.title_y + 115);
    lv_obj_set_style_bg_color(btn_sync, COL_ACCENT, 0);
    lv_obj_add_event_cb(btn_sync, sync_click_cb, LV_EVENT_CLICKED, NULL);
    lbl_btn_sync = lv_label_create(btn_sync);
    lv_label_set_text(lbl_btn_sync, "Sync to PC");
    lv_obj_set_style_text_font(lbl_btn_sync, &font_styrene_14, 0);
    lv_obj_set_style_text_color(lbl_btn_sync, COL_TEXT, 0);
    lv_obj_align(lbl_btn_sync, LV_ALIGN_CENTER, 0, 0);

    // List of recordings
    list_memos = lv_list_create(recorder_container);
    lv_obj_set_size(list_memos, L.scr_w - 40, L.scr_h - L.title_y - 200);
    lv_obj_align(list_memos, LV_ALIGN_TOP_MID, 0, L.title_y + 175);
    lv_obj_set_style_bg_color(list_memos, lv_color_hex(0x1a1a24), 0);
    lv_obj_set_style_border_width(list_memos, 1, 0);
    lv_obj_set_style_border_color(list_memos, lv_color_hex(0x2d2d3d), 0);
    lv_obj_set_style_pad_all(list_memos, 5, 0);

    // Start UI update timer
    rec_timer = lv_timer_create(recorder_timer_cb, 200, NULL);

    lv_obj_add_flag(recorder_container, LV_OBJ_FLAG_HIDDEN);
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(usage_container, usage_gesture_cb, LV_EVENT_GESTURE, NULL);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 16, L.title_y);

    // Usage panels (shown when connected) live in a transparent full-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    panel_session = make_usage_panel(usage_group, L.content_y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);

    // Enterprise-only overlays inside panel_session — hidden until enterprise data arrives
    lbl_session_pct_sym = lv_label_create(panel_session);
    lv_label_set_text(lbl_session_pct_sym, "%");
    lv_obj_set_style_text_font(lbl_session_pct_sym, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_session_pct_sym, COL_TEXT, 0);
    lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_desc = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_desc, "of your monthly budget");
    lv_obj_set_style_text_font(lbl_spending_desc, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_spending_desc, COL_DIM, 0);
    lv_obj_set_pos(lbl_spending_desc, 0, L.usage_reset_y);
    lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_status = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_status, "");
    lv_obj_set_style_text_font(lbl_spending_status, &font_styrene_16, 0);
    lv_obj_set_pos(lbl_spending_status, 0, L.usage_reset_y + 20);
    lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);

    panel_weekly = make_usage_panel(usage_group,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);
    // Recolor enabled so enterprise period box can color pace and reset separately
    lv_label_set_recolor(lbl_weekly_reset, true);

    build_pair_group(usage_container);
    build_idle_group(usage_container);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);

    lv_obj_clear_flag(panel_session, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(panel_session, panel_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(panel_weekly, panel_click_cb, LV_EVENT_CLICKED, NULL);

    build_approval_overlay(usage_container);
    build_wifi_modal(usage_container);
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);
    init_battery_icons();

    init_usage_screen(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, L.margin, L.title_y - 10);
    lv_obj_add_flag(logo_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(logo_img, logo_click_cb, LV_EVENT_CLICKED, NULL);

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - 48 - L.margin, L.title_y);

    build_settings_shade(scr);
    build_recorder_container(scr);
    lv_obj_add_event_cb(scr, screen_gesture_cb, LV_EVENT_GESTURE, NULL);
}

void ui_update(const UsageData* data) {
    if (data != nullptr) {
        cached_data = *data;
        has_cached_data = true;
    }
    if (!has_cached_data) return;

    const UsageData* cur_data = &cached_data;
    last_data_ms = lv_tick_get();   // a valid usage update just landed → dot goes green
    data_received = true;

    static int prev_agent_state = 0;
    if (cur_data->agent_state == 2 && prev_agent_state != 2) {
        if (!settings_get_mute_permission()) {
            sound_hal_play_reset();
        }
    }
    prev_agent_state = cur_data->agent_state;

    static bool prev_token_restored = false;
    if (cur_data->token_restored && !prev_token_restored) {
        if (!settings_get_mute_token()) {
            sound_hal_play_reset();
        }

        // Show a custom premium forest green toast
        lv_obj_t* toast = lv_obj_create(lv_screen_active());
        lv_obj_set_size(toast, L.scr_w - 60, 60);
        lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, 80);
        lv_obj_set_style_bg_color(toast, lv_color_hex(0x112b11), 0); // dark forest green
        lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(toast, lv_color_hex(0x2a6b2a), 0); // green border
        lv_obj_set_style_border_width(toast, 2, 0);
        lv_obj_set_style_radius(toast, 12, 0);
        lv_obj_clear_flag(toast, LV_OBJ_FLAG_SCROLLABLE);
        
        lv_obj_t* lbl = lv_label_create(toast);
        lv_label_set_text(lbl, "Claude Token Restored!");
        lv_obj_set_style_text_font(lbl, &font_styrene_16, 0);
        lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        
        // Auto-delete timer
        lv_timer_create([](lv_timer_t* t){
            lv_obj_t* target = (lv_obj_t*)lv_timer_get_user_data(t);
            if (lv_obj_is_valid(target)) {
                lv_obj_delete(target);
            }
            lv_timer_delete(t);
        }, 3500, toast);
    }
    prev_token_restored = cur_data->token_restored;

    if (approval_overlay) {
        if (cur_data->agent_state == 2) {
            lv_label_set_text(lbl_approval_msg, cur_data->agent_msg);
            lv_obj_remove_flag(approval_overlay, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(approval_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (cur_data->clock_epoch > 0) {    // daemon supplied wall-clock time → drive the title clock
        clock_base_epoch = cur_data->clock_epoch;
        clock_base_ms = last_data_ms;
        clock_fmt = cur_data->clock_fmt;
    } else if (clock_base_epoch != 0) {   // clock turned off daemon-side → revert title to "Usage"
        clock_base_epoch = 0;
        clock_last_min = -1;
        lv_label_set_text(lbl_title, active_model == 0 ? "Claude" : "Gemini");
    }

    if (clock_base_epoch == 0) {
        lv_label_set_text(lbl_title, active_model == 0 ? "Claude" : "Gemini");
    }

    float session_pct = 0.0f;
    int session_reset_mins = -1;
    float weekly_pct = 0.0f;
    int weekly_reset_mins = -1;

    if (active_model == 0) {
        // Claude
        session_pct = cur_data->claude.session_pct;
        session_reset_mins = cur_data->claude.session_reset_mins;
        weekly_pct = cur_data->claude.weekly_pct;
        weekly_reset_mins = cur_data->claude.weekly_reset_mins;
    } else {
        // Gemini
        session_pct = cur_data->gemini.session_pct;
        session_reset_mins = cur_data->gemini.session_reset_mins;
        weekly_pct = cur_data->gemini.weekly_pct;
        weekly_reset_mins = cur_data->gemini.weekly_reset_mins;
    }

    int s_pct = (int)(session_pct + 0.5f);
    bool is_enterprise = (active_model == 0 && cur_data->enterprise);

    if (is_enterprise) {
        // Spending box: big number-only label + small "%" symbol + desc + pace
        lv_obj_set_style_text_font(lbl_session_pct, &font_tiempos_56, 0);
        lv_label_set_text(lbl_session_label, "Spending");
        lv_obj_add_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status,   LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_font(lbl_session_pct, &font_styrene_48, 0);
        lv_label_set_text(lbl_session_label, active_model == 0 ? "Claude" : "Gemini");
        lv_obj_clear_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[48];

    // Pace vars used in both enterprise blocks below
    const char* pace_text = "Under pace";
    lv_color_t  pace_color = COL_GREEN;
    const char* pace_hex   = "788c5d";   // matches THEME_GREEN
    if (session_pct > (float)cur_data->time_pct + 15.0f) {
        pace_text = "Over pace";  pace_color = COL_RED;   pace_hex = "c0392b";
    } else if (session_pct > (float)cur_data->time_pct - 15.0f) {
        pace_text = "On pace";    pace_color = COL_AMBER; pace_hex = "d97757";
    }

    if (is_enterprise) {
        lv_label_set_text_fmt(lbl_session_pct, "%d", s_pct);
        lv_obj_align_to(lbl_session_pct_sym, lbl_session_pct,
                        LV_ALIGN_OUT_RIGHT_TOP, 4, 12);
    } else {
        lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
        format_reset_time(session_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_session_reset, buf);
    }

    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(session_pct), LV_PART_INDICATOR);

    if (is_enterprise) {
        // Period box: time % + dynamic pace color + "Resets <date>" label
        lv_label_set_text(lbl_weekly_label, "Period");
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", cur_data->time_pct);
        lv_bar_set_value(bar_weekly, cur_data->time_pct, LV_ANIM_ON);
        lv_color_t bar_pace = (session_pct <= (float)cur_data->time_pct) ? COL_GREEN :
                              (session_pct <= (float)cur_data->time_pct + 15.0f) ? COL_AMBER :
                              COL_RED;
        lv_obj_set_style_bg_color(bar_weekly, bar_pace, LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "#%s %s# - #faf9f5 Resets %s#",
                 pace_hex, pace_text, cur_data->reset_date);
        lv_label_set_text(lbl_weekly_reset, buf);
    } else {
        int w_pct = (int)(weekly_pct + 0.5f);
        lv_label_set_text(lbl_weekly_label, active_model == 0 ? "Weekly" : "Daily");
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
        lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_weekly, pct_color(weekly_pct), LV_PART_INDICATOR);
        format_reset_time(weekly_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_weekly_reset, buf);
    }

    // Set animation spinner color to match active model
    lv_obj_set_style_text_color(lbl_anim, active_model == 0 ? COL_ACCENT : THEME_GEMINI_ACCENT, 0);
}

// Pick the usage-view sub-screen: pairing hint (BLE down), the idle "Zzz" screen
// (connected but data has gone stale), or the live usage panels. Only re-lays-out
// on an actual change. The animated status line stays visible everywhere — it
// reads "Listening…" on the idle screen, keeping it alive rather than frozen.
static void update_view_state(void) {
    if (!usage_group || !pair_group || !idle_group) return;
    int v;
    if (!s_ble_connected) {
        v = 0;  // pairing hint
    } else if (data_received && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
        v = 2;  // live usage
    } else {
        v = 1;  // idle / Zzz
    }
    if (v == view_state) return;
    view_state = v;
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v == 0 ? pair_group : v == 1 ? idle_group : usage_group,
                      LV_OBJ_FLAG_HIDDEN);
}

void ui_tick_anim(void) {
    if (current_screen != SCREEN_USAGE) return;
    update_view_state();
    if (view_state == 1) splash_mini_tick();   // animate the sleeping creature on the idle screen

    uint32_t now = lv_tick_get();

    // Title clock: once the daemon has sent wall-clock time, replace "Usage" with
    // the live time, advanced locally so it ticks every minute between payloads.
    if (clock_base_epoch > 0) {
        time_t cur = (time_t)(clock_base_epoch + (now - clock_base_ms) / 1000);
        struct tm tmv;
        gmtime_r(&cur, &tmv);   // epoch is already local wall-clock → gmtime keeps it as-is
        if (tmv.tm_min != clock_last_min) {   // only rewrite the title when the minute changes
            clock_last_min = tmv.tm_min;
            char tbuf[12];
            if (clock_fmt == 12) {
                int h12 = tmv.tm_hour % 12;
                if (h12 == 0) h12 = 12;
                snprintf(tbuf, sizeof(tbuf), "%d:%02d %s", h12, tmv.tm_min,
                         tmv.tm_hour < 12 ? "AM" : "PM");
            } else {
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            }
            lv_label_set_text(lbl_title, tbuf);
        }
    }

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    // Status text by priority. Whimsical messages only when connected & settled.
    const char* text;
    lv_color_t spinner_color = active_model == 0 ? COL_ACCENT : THEME_GEMINI_ACCENT;

    if (!s_ble_connected) {
        text = "Waiting";              // advertising / waiting for a host connection
    } else if (view_state == 1) {      // idle — alternate so it reads as alive AND data-less
        text = (anim_msg_idx & 1) ? "No data" : "Listening";
    } else if (has_cached_data && cached_data.agent_state == 1) {
        text = "Working";
        spinner_color = COL_RED;
    } else if (has_cached_data && cached_data.agent_state == 2) {
        text = "Needs permit";
        spinner_color = COL_AMBER;
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else {
        text = anim_messages[anim_msg_idx];
    }

    // All states share the whimsical style: "<glyph> <Title-case word>…"
    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(lbl_anim, buf);
    lv_obj_set_style_text_color(lbl_anim, spinner_color, 0);

    // Check Wi-Fi state for modals
    static net_state_t prev_net_state = NET_STATE_INIT;
    net_state_t ns = net_get_state();
    
    if (wifi_modal && lv_obj_has_flag(wifi_modal, LV_OBJ_FLAG_HIDDEN)) {
        if (ns == NET_STATE_DISCONNECTED && prev_net_state == NET_STATE_CONNECTED && net_is_using_home()) {
            wifi_modal_is_home_prompt = false;
            lv_label_set_text(lbl_wifi_modal_title, "Connection Lost");
            lv_label_set_text(lbl_wifi_modal_msg, "Home Wi-Fi lost.\nConnect to Phone Hotspot?");
            lv_obj_remove_flag(wifi_modal, LV_OBJ_FLAG_HIDDEN);
        }
        else if (net_is_home_detected() && !net_is_using_home()) {
            wifi_modal_is_home_prompt = true;
            lv_label_set_text(lbl_wifi_modal_title, "Home Detected");
            lv_label_set_text(lbl_wifi_modal_msg, "Home Wi-Fi detected.\nSwitch to Home Wi-Fi?");
            lv_obj_remove_flag(wifi_modal, LV_OBJ_FLAG_HIDDEN);
        }
    }
    prev_net_state = ns;
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

int ui_get_active_model(void) {
    return active_model;
}

#include "usage_rate.h"

void ui_cycle_model(void) {
    active_model = 1 - active_model;
    
    // When switching models, re-sample rate based on the new active model's usage
    if (has_cached_data) {
        float active_session_pct = (active_model == 0) ? cached_data.claude.session_pct : cached_data.gemini.session_pct;
        usage_rate_sample(active_session_pct);
        if (splash_is_active()) {
            splash_pick_for_current_rate();
        }
    }
    
    ui_update(nullptr);
}

static void panel_click_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        lv_event_stop_processing(e);
        ui_cycle_model();
    }
}


static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:  splash_show(); break;
    case SCREEN_USAGE:   lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    if (logo_img) {
        if (screen == SCREEN_SPLASH) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else                          lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_net_status(net_state_t state, const char* name, const char* ip) {
    (void)name; (void)ip;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == NET_STATE_CONNECTED);

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    // pair / idle / usage — picked from connection + data freshness.
    update_view_state();
}

void ui_update_battery(int percent, bool charging) {
    int idx;
    if (charging) {
        idx = 4;
    } else if (percent < 0) {
        idx = 0;
    } else if (percent <= 10) {
        idx = 0;
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}
