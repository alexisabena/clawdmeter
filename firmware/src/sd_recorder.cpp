#include "sd_recorder.h"
#ifdef BOARD_AMOLED_216
#include <FS.h>
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include <ESP_I2S.h>
#include "es8311.h"
#include "boards/waveshare_amoled_216/board.h"
#include "hal/sound_hal.h"

#define SD_CLK 2
#define SD_CMD 1
#define SD_D0  3
#define SD_CS  41

static bool sd_mounted = false;
static bool recording_active = false;
static uint32_t record_start_ms = 0;
static uint32_t record_duration_ms = 0;
static File record_file;
static I2SClass i2s_rec;
static String active_filename = "";

struct wav_header_t {
    char chunk_id[4];        // "RIFF"
    uint32_t chunk_size;     // 36 + subchunk2_size
    char format[4];          // "WAVE"
    char subchunk1_id[4];    // "fmt "
    uint32_t subchunk1_size; // 16
    uint16_t audio_format;   // 1 (PCM)
    uint16_t num_channels;   // 1 (mono)
    uint32_t sample_rate;    // 16000
    uint32_t byte_rate;      // sample_rate * num_channels * bits_per_sample / 8
    uint16_t block_align;    // num_channels * bits_per_sample / 8
    uint16_t bits_per_sample;// 16
    char subchunk2_id[4];    // "data"
    uint32_t subchunk2_size; // size of audio data
};

void sd_recorder_init(void) {
    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
    // CS is GPIO 41
    if (SD_MMC.begin("/sdcard", true)) {
        sd_mounted = true;
        Serial.println("SD Card: Mounted successfully via SD_MMC (1-bit)");
        
        // Ensure /history.json exists
        if (!SD_MMC.exists("/history.json")) {
            File f = SD_MMC.open("/history.json", FILE_WRITE);
            if (f) {
                f.print("[]");
                f.close();
            }
        }
    } else {
        sd_mounted = false;
        Serial.println("SD Card: Mount failed");
    }
}

bool sd_recorder_is_sd_mounted(void) {
    return sd_mounted;
}

bool sd_recorder_is_recording(void) {
    return recording_active;
}

uint32_t sd_recorder_get_record_duration_ms(void) {
    if (recording_active) {
        return millis() - record_start_ms;
    }
    return record_duration_ms;
}

// Asynchronous recording task
static void record_task(void* arg) {
    char* filepath = (char*)arg;
    record_file = SD_MMC.open(filepath, FILE_WRITE);
    free(filepath);
    
    if (!record_file) {
        Serial.println("SD Card: Failed to open record file");
        recording_active = false;
        vTaskDelete(NULL);
        return;
    }

    // 1. Write dummy WAV header to reserve space
    wav_header_t dummy_header;
    memset(&dummy_header, 0, sizeof(dummy_header));
    record_file.write((uint8_t*)&dummy_header, sizeof(dummy_header));

    // 2. Configure ES8311 for Microphone input (analog, 33dB gain)
    es8311_handle_t es = es8311_create(0, SND_ES8311_ADDR);
    if (es) {
        es8311_microphone_config(es, false); // analog input
        es8311_microphone_gain_set(es, ES8311_MIC_GAIN_30DB);
        es8311_delete(es);
    }

    // 3. Configure and start I2S RX
    i2s_rec.setPins(SND_I2S_BCLK, SND_I2S_WS, -1, SND_I2S_DIN, SND_I2S_MCLK);
    if (!i2s_rec.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
        Serial.println("SD Card: Failed to start I2S RX");
        record_file.close();
        recording_active = false;
        vTaskDelete(NULL);
        return;
    }

    Serial.println("SD Card: Recording started...");
    
    int16_t buffer[256];
    uint32_t total_data_bytes = 0;
    
    while (recording_active) {
        size_t read_bytes = i2s_rec.readBytes((char*)buffer, sizeof(buffer));
        if (read_bytes > 0) {
            record_file.write((uint8_t*)buffer, read_bytes);
            total_data_bytes += read_bytes;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Stop I2S
    i2s_rec.end();
    
    // 4. Write final WAV header
    wav_header_t header;
    memcpy(header.chunk_id, "RIFF", 4);
    header.chunk_size = 36 + total_data_bytes;
    memcpy(header.format, "WAVE", 4);
    memcpy(header.subchunk1_id, "fmt ", 4);
    header.subchunk1_size = 16;
    header.audio_format = 1; // PCM
    header.num_channels = 1; // Mono
    header.sample_rate = 16000;
    header.bits_per_sample = 16;
    header.byte_rate = 16000 * 1 * 16 / 8;
    header.block_align = 1 * 16 / 8;
    memcpy(header.subchunk2_id, "data", 4);
    header.subchunk2_size = total_data_bytes;

    record_file.seek(0);
    record_file.write((uint8_t*)&header, sizeof(header));
    record_file.close();

    Serial.printf("SD Card: Recording stopped, saved %u bytes\n", total_data_bytes);
    
    // Restore Chime output
    sound_hal_init();
    
    vTaskDelete(NULL);
}

bool sd_recorder_start(const char* filename) {
    if (!sd_mounted || recording_active) return false;
    
    // Shutdown chime I2S so we can rebind pins for RX
    #include "chime.h"
    chime_deinit();

    recording_active = true;
    record_start_ms = millis();
    active_filename = filename;
    
    char* path = (char*)malloc(64);
    snprintf(path, 64, "/%s", filename);

    if (xTaskCreatePinnedToCore(record_task, "record", 4096, path, 2, NULL, 1) != pdPASS) {
        free(path);
        recording_active = false;
        sound_hal_init();
        return false;
    }
    return true;
}

const char* sd_recorder_get_active_filename(void) {
    return active_filename.c_str();
}

void sd_recorder_stop(void) {
    if (!recording_active) return;
    record_duration_ms = millis() - record_start_ms;
    recording_active = false;
    // record_task task loop will exit and restore Chime I2S itself
}

String sd_recorder_get_history_json(void) {
    if (!sd_mounted) return "[]";
    File f = SD_MMC.open("/history.json", FILE_READ);
    if (!f) return "[]";
    String res = f.readString();
    f.close();
    return res;
}

void sd_recorder_add_history_entry(const char* filename, int duration_sec) {
    if (!sd_mounted) return;
    
    // Parse current history
    JsonDocument doc;
    File f = SD_MMC.open("/history.json", FILE_READ);
    if (f) {
        deserializeJson(doc, f);
        f.close();
    }

    if (!doc.is<JsonArray>()) {
        doc.to<JsonArray>();
    }
    JsonArray arr = doc.as<JsonArray>();

    // Build time string
    time_t now = time(nullptr);
    char time_str[32];
    struct tm* tm_info = localtime(&now);
    if (tm_info && tm_info->tm_year > 120) { // Valid time sync from PC
        strftime(time_str, sizeof(time_str), "%b %d, %H:%M", tm_info);
    } else {
        snprintf(time_str, sizeof(time_str), "Memo #%d", (int)arr.size() + 1);
    }

    // Add entry
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = filename;
    obj["date"] = time_str;
    obj["duration"] = duration_sec;
    obj["uploaded"] = false;
    obj["timestamp"] = (long)now;

    // Prune entries older than 30 days (if we have valid timestamps)
    // Or limit to max 50 entries to prevent history file from growing indefinitely
    while (arr.size() > 50) {
        arr.remove(0);
    }

    // Save back to SD card
    File f_out = SD_MMC.open("/history.json", FILE_WRITE);
    if (f_out) {
        serializeJson(doc, f_out);
        f_out.close();
    }
}

void sd_recorder_set_uploaded(const char* filename) {
    if (!sd_mounted) return;
    
    JsonDocument doc;
    File f = SD_MMC.open("/history.json", FILE_READ);
    if (f) {
        deserializeJson(doc, f);
        f.close();
    }

    if (doc.is<JsonArray>()) {
        JsonArray arr = doc.as<JsonArray>();
        for (JsonObject obj : arr) {
            if (strcmp(obj["name"] | "", filename) == 0) {
                obj["uploaded"] = true;
                break;
            }
        }
        
        File f_out = SD_MMC.open("/history.json", FILE_WRITE);
        if (f_out) {
            serializeJson(doc, f_out);
            f_out.close();
        }
    }
}

void sd_recorder_delete_file(const char* filename) {
    if (!sd_mounted) return;
    char filepath[64];
    snprintf(filepath, sizeof(filepath), "/%s", filename);
    if (SD_MMC.exists(filepath)) {
        SD_MMC.remove(filepath);
    }
}
#endif
