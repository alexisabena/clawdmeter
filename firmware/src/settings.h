#pragma once
#include <Arduino.h>

void settings_init(void);
bool settings_get_mute_token(void);
void settings_set_mute_token(bool mute);
bool settings_get_mute_permission(void);
void settings_set_mute_permission(bool mute);
