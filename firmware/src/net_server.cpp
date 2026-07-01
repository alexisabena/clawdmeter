#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "net_server.h"
#include "wifi_credentials.h"

static WebServer server(80);
static net_state_t state = NET_STATE_INIT;
static char ip_str[32] = "0.0.0.0";
static char daemon_url[128] = "";

#define NET_BUF_SIZE 512
static char rx_buf[NET_BUF_SIZE];
static volatile bool data_ready = false;
static int approval_status = 0; // 0 = pending, 1 = allow_once, 2 = always_allow, 3 = deny

static void handle_approval() {
    String json_resp;
    switch (approval_status) {
        case 1:  json_resp = "{\"status\":\"allow_once\"}"; break;
        case 2:  json_resp = "{\"status\":\"always_allow\"}"; break;
        case 3:  json_resp = "{\"status\":\"deny\"}"; break;
        default: json_resp = "{\"status\":\"pending\"}"; break;
    }
    server.send(200, "application/json", json_resp);
}

static void handle_payload() {
    if (server.method() != HTTP_POST) {
        server.send(405, "application/json", "{\"error\":\"Method Not Allowed\"}");
        return;
    }
    
    String body = server.arg("plain");
    size_t len = std::min(body.length(), (size_t)(NET_BUF_SIZE - 1));
    memcpy(rx_buf, body.c_str(), len);
    rx_buf[len] = '\0';
    data_ready = true;
    
    // Extract daemon_url if provided
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (!err) {
        if (doc.containsKey("d_url")) {
            strlcpy(daemon_url, doc["d_url"] | "", sizeof(daemon_url));
        }
    }
    
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

static bool using_home = true;
static bool home_detected = false;
static uint32_t last_scan_time = 0;
static bool scan_in_progress = false;

void net_connect_home(void) {
    using_home = true;
    home_detected = false;
    WiFi.disconnect();
    server.stop();
    MDNS.end();
    state = NET_STATE_CONNECTING;
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_HOME_SSID, WIFI_HOME_PASS);
    Serial.println("Wi-Fi: Connecting to Home Wi-Fi...");
}

void net_connect_phone(void) {
    using_home = false;
    WiFi.disconnect();
    server.stop();
    MDNS.end();
    state = NET_STATE_CONNECTING;
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_PHONE_SSID, WIFI_PHONE_PASS);
    Serial.println("Wi-Fi: Connecting to Phone Hotspot...");
}

bool net_is_using_home(void) {
    return using_home;
}

bool net_is_home_detected(void) {
    return home_detected;
}

void net_clear_home_detected(void) {
    home_detected = false;
}

static bool server_started = false;

void net_init(void) {
    state = NET_STATE_CONNECTING;
    using_home = true;
    WiFi.persistent(false);   // do NOT save credentials to flash
    WiFi.disconnect(true);    // erase any previously stored credentials
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    Serial.println("Wi-Fi: connecting to Home Wi-Fi...");
    WiFi.begin(WIFI_HOME_SSID, WIFI_HOME_PASS);
}

void net_tick(void) {
    uint32_t now = millis();

    if (state == NET_STATE_CONNECTING) {
        // Timeout: if stuck connecting > 20s, force a clean retry
        static uint32_t connect_start_time = 0;
        if (connect_start_time == 0) connect_start_time = now;
        if (now - connect_start_time >= 20000) {
            connect_start_time = 0;
            Serial.println("Wi-Fi: connection timeout — retrying with clean state...");
            WiFi.disconnect(true);
            delay(100);
            WiFi.mode(WIFI_STA);
            if (using_home) {
                WiFi.begin(WIFI_HOME_SSID, WIFI_HOME_PASS);
            } else {
                WiFi.begin(WIFI_PHONE_SSID, WIFI_PHONE_PASS);
            }
        }
        // Print WiFi status every 2s so we can diagnose failures over serial
        static uint32_t last_dbg_time = 0;
        static uint32_t last_scan_time_dbg = 0;
        if (now - last_dbg_time >= 2000) {
            last_dbg_time = now;
            int ws = WiFi.status();
            const char* status_str = "UNKNOWN";
            switch(ws) {
                case WL_IDLE_STATUS:     status_str = "WL_IDLE_STATUS";     break;
                case WL_NO_SSID_AVAIL:   status_str = "WL_NO_SSID_AVAIL";   break;
                case WL_SCAN_COMPLETED:  status_str = "WL_SCAN_COMPLETED";  break;
                case WL_CONNECTED:       status_str = "WL_CONNECTED";       break;
                case WL_CONNECT_FAILED:  status_str = "WL_CONNECT_FAILED";  break;
                case WL_CONNECTION_LOST: status_str = "WL_CONNECTION_LOST"; break;
                case WL_DISCONNECTED:    status_str = "WL_DISCONNECTED";    break;
            }
            // Use label not raw SSID to avoid non-ASCII crash
            const char* net_label = using_home ? "Home" : "Phone";
            Serial.printf("Wi-Fi status: %s (%d), network: %s\n",
                status_str, ws, net_label);
        }
        // Repeat network scan every 30s so monitor can catch it even after reconnect
        if (now - last_scan_time_dbg >= 30000) {
            last_scan_time_dbg = now;
            Serial.println("Wi-Fi: rescanning networks...");
            int n = WiFi.scanNetworks();
            for (int i = 0; i < n; ++i) {
                Serial.printf("  [%d] SSID: \"%s\" RSSI: %d Auth: %d\n",
                    i + 1,
                    WiFi.SSID(i).c_str(),
                    WiFi.RSSI(i),
                    (int)WiFi.encryptionType(i));
            }
            WiFi.scanDelete();
        }
        if (WiFi.status() == WL_CONNECTED) {
            state = NET_STATE_CONNECTED;
            snprintf(ip_str, sizeof(ip_str), "%s", WiFi.localIP().toString().c_str());
            Serial.printf("Wi-Fi: connected, IP=%s\n", ip_str);
            
            // Start mDNS
            if (MDNS.begin("clawdmeter")) {
                Serial.println("mDNS: responder started (http://clawdmeter.local)");
            }
            
            // Set up web server endpoints (re-register on every connect)
            if (!server_started) {
                server.on("/api/payload", handle_payload);
                server.on("/api/approval", handle_approval);
                server_started = true;
            }
            server.begin();
            Serial.println("HTTP: server started on port 80");
        }
    } else if (state == NET_STATE_CONNECTED) {
        if (WiFi.status() != WL_CONNECTED) {
            state = NET_STATE_DISCONNECTED;
            Serial.println("Wi-Fi: connection lost");
        } else {
            server.handleClient();

            // If connected to Phone, periodically scan for Home network in background
            if (!using_home) {
                if (!scan_in_progress && (now - last_scan_time >= 30000)) {
                    WiFi.scanNetworks(true); // true = asynchronous scan
                    scan_in_progress = true;
                    last_scan_time = now;
                }
                
                if (scan_in_progress) {
                    int16_t n = WiFi.scanComplete();
                    if (n >= 0) { // scan finished (n < 0 means scanning or error)
                        scan_in_progress = false;
                        for (int i = 0; i < n; ++i) {
                            if (WiFi.SSID(i) == WIFI_HOME_SSID) {
                                home_detected = true;
                                Serial.println("Wi-Fi: Home SSID detected in scan!");
                                break;
                            }
                        }
                        WiFi.scanDelete();
                    }
                }
            }
        }
    } else if (state == NET_STATE_DISCONNECTED) {
        static uint32_t last_reconnect_time = 0;
        if (now - last_reconnect_time >= 5000) {
            last_reconnect_time = now;
            state = NET_STATE_CONNECTING;
            WiFi.disconnect(true);
            WiFi.mode(WIFI_STA);
            if (using_home) {
                WiFi.begin(WIFI_HOME_SSID, WIFI_HOME_PASS);
                Serial.println("Wi-Fi: Retrying Home Wi-Fi connection...");
            } else {
                WiFi.begin(WIFI_PHONE_SSID, WIFI_PHONE_PASS);
                Serial.println("Wi-Fi: Retrying Phone Hotspot connection...");
            }
        }
    }
}

net_state_t net_get_state(void) {
    return state;
}

const char* net_get_device_name(void) {
    return using_home ? "Clawdmeter-Home" : "Clawdmeter-Phone";
}

const char* net_get_ip_address(void) {
    return ip_str;
}

bool net_has_bonds(void) { return false; }
void net_clear_bonds(void) {}

bool net_has_data(void) {
    return data_ready;
}

const char* net_get_data(void) {
    data_ready = false;
    return rx_buf;
}

void net_send_ack(void) {}
void net_send_nack(void) {}

void net_send_response(const char* response) {
    if (state == NET_STATE_CONNECTED && strlen(daemon_url) > 0) {
        WiFiClient client;
        HTTPClient http;
        http.begin(client, daemon_url);
        http.addHeader("Content-Type", "application/json");
        int httpResponseCode = http.POST(response);
        Serial.printf("Wi-Fi: sent response to daemon, code=%d\n", httpResponseCode);
        http.end();
    }
}

void net_set_approval_status(int status) {
    approval_status = status;
    Serial.printf("Approval status changed: %d\n", status);
}

int net_get_approval_status(void) {
    return approval_status;
}

void net_request_refresh(void) {}
