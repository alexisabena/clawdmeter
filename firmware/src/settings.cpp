#include "settings.h"
#include <Preferences.h>

static bool mute_token = false;
static bool mute_permission = false;

void settings_init(void) {
    Preferences prefs;
    prefs.begin("clawd_settings", true);
    mute_token = prefs.getBool("mute_token", false);
    mute_permission = prefs.getBool("mute_perm", false);
    prefs.end();
    Serial.printf("Settings init: mute_token=%s, mute_permission=%s\n", 
                  mute_token ? "true" : "false", 
                  mute_permission ? "true" : "false");
}

bool settings_get_mute_token(void) {
    return mute_token;
}

void settings_set_mute_token(bool mute) {
    mute_token = mute;
    Preferences prefs;
    prefs.begin("clawd_settings", false);
    prefs.putBool("mute_token", mute);
    prefs.end();
}

bool settings_get_mute_permission(void) {
    return mute_permission;
}

void settings_set_mute_permission(bool mute) {
    mute_permission = mute;
    Preferences prefs;
    prefs.begin("clawd_settings", false);
    prefs.putBool("mute_perm", mute);
    prefs.end();
}
