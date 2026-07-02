#pragma once
#include <Arduino.h>

#ifdef BOARD_AMOLED_216

void sd_recorder_init(void);
bool sd_recorder_is_sd_mounted(void);
bool sd_recorder_is_recording(void);

// Audio controls
bool sd_recorder_start(const char* filename);
const char* sd_recorder_get_active_filename(void);
void sd_recorder_stop(void);
uint32_t sd_recorder_get_record_duration_ms(void);

// History management (one month log)
String sd_recorder_get_history_json(void);
void sd_recorder_add_history_entry(const char* filename, int duration_sec);
void sd_recorder_set_uploaded(const char* filename);
void sd_recorder_delete_file(const char* filename);

#else

inline void sd_recorder_init(void) {}
inline bool sd_recorder_is_sd_mounted(void) { return false; }
inline bool sd_recorder_is_recording(void) { return false; }
inline bool sd_recorder_start(const char* filename) { return false; }
inline const char* sd_recorder_get_active_filename(void) { return ""; }
inline void sd_recorder_stop(void) {}
inline uint32_t sd_recorder_get_record_duration_ms(void) { return 0; }
inline String sd_recorder_get_history_json(void) { return "[]"; }
inline void sd_recorder_add_history_entry(const char* filename, int duration_sec) {}
inline void sd_recorder_set_uploaded(const char* filename) {}
inline void sd_recorder_delete_file(const char* filename) {}

#endif
