/*
 * ============================================================================
 *  esp32_iot.ino  —  ESP32 Smart Home Controller  (Offline-Capable)
 * ============================================================================
 *
 *  Target board : ESP32 Dev Module (ESP32-WROOM-32)
 *  Framework    : Arduino
 *
 *  THREE FULLY INDEPENDENT LAYERS — each works without the next:
 *  ─────────────────────────────────────────────────────────────
 *  LAYER 1 · PHYSICAL SWITCHES (always ON, 50 ms scan, no WiFi needed)
 *    Each wall switch immediately drives its paired relay regardless of
 *    WiFi, internet, or Supabase state.
 *
 *  LAYER 2 · LOCAL WEB SERVER (needs WiFi to home network, not internet)
 *    A beautiful Arabic dashboard served from the ESP32 itself.
 *    Open it at http://smarthome.local  OR  http://<IP>
 *    Works perfectly even when Supabase is unreachable.
 *    All relay toggles from this page drive the relays instantly and are
 *    queued/synced to Supabase as soon as internet comes back.
 *
 *  LAYER 3 · SUPABASE SYNC (needs internet, graceful degradation)
 *    When online: polls every 1 s and pushes switch/relay changes.
 *    When offline: silently retries every 5 minutes. On reconnect it
 *    first PUSHES the current local state to Supabase (local wins), then
 *    resumes normal polling so Supabase stays accurate.
 *
 *  WIRING (unchanged — see full table in previous sketch)
 *  ─────────────────────────────────────────────────────
 *   #  Room              Relay GPIO   Switch GPIO
 *   1  غرفة أبي         25           32
 *   2  غرفة جدتي        26           12
 *   3  المجلس            22           33
 *   4  الصالة            14           4
 *   5  المطبخ            27           19
 *   6  حمام الديوان      21           18
 *   7  غرفة منيب         13           5
 *   8  الحمام العائلي    23           36  ← NOT GPIO13 (relay conflict)
 *
 *  REQUIRED LIBRARIES (Arduino IDE → Manage Libraries)
 *    • ArduinoJson  by Benoit Blanchon
 *    ESPmDNS, WebServer, WiFi, WiFiClientSecure, HTTPClient are all
 *    included in the esp32 board package — no extra install needed.
 * ============================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ============================================================================
//  CONFIGURATION
// ============================================================================

const char* WIFI_SSID = "Al Rajhi 1";
const char* WIFI_PASS = "m772220802mm";

const char* MDNS_NAME = "smarthome";   // -> http://smarthome.local

// Supabase
const char* SB_ANON  = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InhoaXJmemViZ3R2aW9vYXVjbGF5Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODMwNzY5MzIsImV4cCI6MjA5ODY1MjkzMn0.thTirFPUT8RAt4XXQf1GrX7zYPgHvutbWjTQf8PQhkY";
const char* SB_GET   = "https://xhirfzebgtviooauclay.supabase.co/rest/v1/devices?id=eq.1&select=relay1,relay2,relay3,relay4,relay5,relay6,relay7,relay8,switch1,switch2,switch3,switch4,switch5,switch6,switch7,switch8";
const char* SB_PATCH = "https://xhirfzebgtviooauclay.supabase.co/rest/v1/devices?id=eq.1";

// AP fallback (new): local control when router/internet is unavailable
const char* AP_SSID = "SmartHome";
const char* AP_PASS = "12345678";
IPAddress AP_IP(192, 168, 4, 1);
IPAddress AP_GW(192, 168, 4, 1);
IPAddress AP_MASK(255, 255, 255, 0);

// Timing
const unsigned long SWITCH_SCAN_MS           =     50UL;   // switch debounce interval
const unsigned long SB_POLL_MS               =   1000UL;   // Supabase poll when online
const unsigned long SB_RECONNECT_MS          = 300000UL;   // 5-minute retry when offline
const unsigned long WIFI_RETRY_MS            =  30000UL;   // WiFi reconnect interval
const unsigned long LOCAL_RECOVERY_CHECK_MS  = 300000UL;   // AP-mode STA/internet recovery check

// Relay active level — flip both if your module is active-LOW
const int RELAY_ON  = HIGH;
const int RELAY_OFF = LOW;

// ============================================================================
//  HARDWARE MAPS
// ============================================================================

const int RELAY_PINS[8]  = { 25, 26, 22, 14, 27, 21, 13, 23 };
const int SWITCH_PINS[8] = { 32, 12, 33,  4, 19, 18,  5, 36 };

// Room names stored in flash (PROGMEM) to save RAM
const char RM0[] PROGMEM = "\xD8\xBA\xD8\xB1\xD9\x81\xD8\xA9 \xD8\xA3\xD8\xA8\xD9\x8A";        // غرفة أبي
const char RM1[] PROGMEM = "\xD8\xBA\xD8\xB1\xD9\x81\xD8\xA9 \xD8\xAC\xD8\xAF\xD8\xAA\xD9\x8A"; // غرفة جدتي
const char RM2[] PROGMEM = "\xD8\xA7\xD9\x84\xD9\x85\xD8\xAC\xD9\x84\xD8\xB3";                   // المجلس
const char RM3[] PROGMEM = "\xD8\xA7\xD9\x84\xD8\xB5\xD8\xA7\xD9\x84\xD8\xA9";                   // الصالة
const char RM4[] PROGMEM = "\xD8\xA7\xD9\x84\xD9\x85\xD8\xB7\xD8\xA8\xD8\xAE";                   // المطبخ
const char RM5[] PROGMEM = "\xD8\xAD\xD9\x85\xD8\xA7\xD9\x85 \xD8\xA7\xD9\x84\xD8\xAF\xD9\x8A\xD9\x88\xD8\xA7\xD9\x86"; // حمام الديوان
const char RM6[] PROGMEM = "\xD8\xBA\xD8\xB1\xD9\x81\xD8\xA9 \xD9\x85\xD9\x86\xD9\x8A\xD8\xA8"; // غرفة منيب
const char RM7[] PROGMEM = "\xD8\xA7\xD9\x84\xD8\xAD\xD9\x85\xD8\xA7\xD9\x85 \xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD8\xA6\xD9\x84\xD9\x8A"; // الحمام العائلي

const char* const ROOM_NAMES_P[8] PROGMEM = { RM0,RM1,RM2,RM3,RM4,RM5,RM6,RM7 };

// Helper: copy room name from PROGMEM to a local buffer
void getRoomName(int i, char* buf, size_t len) {
  strncpy_P(buf, (char*)pgm_read_ptr(&ROOM_NAMES_P[i]), len - 1);
  buf[len - 1] = '\0';
}

// ============================================================================
//  STATE
// ============================================================================

bool relayState[8]         = {};
bool lastSwitchState[8]    = {};
bool switchStateUploaded[8]= {};
bool firstSwitchRead       = true;
bool supabaseOnline        = false;
bool pendingSupabaseSync   = false;   // local changes waiting to be pushed
bool fallbackApActive      = false;   // new: true while SmartHome AP fallback is available
bool webServerStarted      = false;

unsigned long lastSwitchScan   = 0;
unsigned long lastSbPoll       = 0;
unsigned long lastReconnect    = 0;
unsigned long lastWifiRetry    = 0;
unsigned long lastRecoveryCheck= 0;

WebServer server(80);

// ============================================================================
//  FORWARD DECLARATIONS
// ============================================================================
void setupPins();
void connectWiFi();
void startFallbackAP();
void stopFallbackAPIfOnline();
void beginLocalServices();
void setupWebServer();
void setupMDNS();
String buildStatusJson();
void handleRoot();
void handleStatus();
void handleToggle();
void handleSetRelay();
void handleAll();
void scanSwitches();
void supabasePoll();
bool supabasePatch(const String& body);
bool supabasePushLocalState();
bool checkSupabaseReachable();
String localDashboardHTML();

// ============================================================================
//  SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n========================================"));
  Serial.println(F("  ESP32 Smart Home  |  Offline-Capable"));
  Serial.println(F("========================================"));

  setupPins();
  beginLocalServices();
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    setupMDNS();

    // Check if Supabase is reachable right now
    supabaseOnline = checkSupabaseReachable();
    if (supabaseOnline) {
      stopFallbackAPIfOnline();
      Serial.println(F("[SB] Supabase reachable - online mode."));
    } else {
      startFallbackAP();
      Serial.println(F("[SB] Supabase not reachable - local AP mode. Will retry in 5 min."));
    }
  } else {
    startFallbackAP();
  }
}

// ============================================================================
//  LOOP
// ============================================================================
void loop() {
  // ── Layer 1: Physical switches (always, no internet needed) ────────────
  if (millis() - lastSwitchScan >= SWITCH_SCAN_MS) {
    lastSwitchScan = millis();
    scanSwitches();
  }

  // ── Layer 2: Local web server (always, only needs WiFi) ────────────────
  server.handleClient();

  // ── WiFi/AP fallback recovery (new) ─────────────────────────────────────
  if (WiFi.status() != WL_CONNECTED) {
    startFallbackAP();
    if (millis() - lastWifiRetry >= WIFI_RETRY_MS) {
      lastWifiRetry = millis();
      Serial.println(F("[WiFi] STA disconnected - retrying in AP+STA mode..."));
      WiFi.mode(WIFI_AP_STA);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    return; // no Supabase work without STA WiFi
  }

  if (fallbackApActive && millis() - lastRecoveryCheck >= LOCAL_RECOVERY_CHECK_MS) {
    lastRecoveryCheck = millis();
    Serial.println(F("[WiFi] AP-mode 5-min internet recovery check..."));
    supabaseOnline = checkSupabaseReachable();
    if (supabaseOnline) {
      Serial.println(F("[SB] Internet recovered. Pushing local state and returning online."));
      if (supabasePushLocalState()) {
        pendingSupabaseSync = false;
        setupMDNS();
        stopFallbackAPIfOnline();
      } else {
        supabaseOnline = false;
        pendingSupabaseSync = true;
        Serial.println(F("[SB] Recovery push failed. Staying in local AP mode."));
      }
    }
  }

  // ── Layer 3: Supabase sync ─────────────────────────────────────────────
  if (supabaseOnline) {
    if (millis() - lastSbPoll >= SB_POLL_MS) {
      lastSbPoll = millis();
      // If we have pending local changes, push them first before pulling
      if (pendingSupabaseSync) {
        if (supabasePushLocalState()) {
          pendingSupabaseSync = false;
        } else {
          supabaseOnline = false;
          lastReconnect = millis();
          startFallbackAP();
        }
      } else {
        supabasePoll();
      }
    }
  } else {
    // Retry Supabase every 5 minutes
    if (millis() - lastReconnect >= SB_RECONNECT_MS) {
      lastReconnect = millis();
      Serial.println(F("[SB] 5-min reconnect attempt..."));
      supabaseOnline = checkSupabaseReachable();
      if (supabaseOnline) {
        Serial.println(F("[SB] Back online! Pushing local state to Supabase..."));
        // Push local relay state so Supabase matches what happened offline
        if (supabasePushLocalState()) {
          pendingSupabaseSync = false;
          stopFallbackAPIfOnline();
          Serial.println(F("[SB] Local state synced. Resuming normal polling."));
        }
      } else {
        startFallbackAP();
        Serial.println(F("[SB] Still offline. Next retry in 5 min."));
      }
    }
  }
}

// ============================================================================
//  PIN SETUP
// ============================================================================
void setupPins() {
  for (int i = 0; i < 8; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], RELAY_OFF);
    relayState[i] = false;
  }
  for (int i = 0; i < 8; i++) {
    // GPIO 34-39 are input-only, no internal pull-up available
    pinMode(SWITCH_PINS[i], SWITCH_PINS[i] >= 34 ? INPUT : INPUT_PULLUP);
  }
  Serial.println(F("[HW] Pins initialised. All relays OFF."));
}

// ============================================================================
//  WiFi
// ============================================================================
void connectWiFi() {
  Serial.print(F("[WiFi] Connecting to ")); Serial.println(WIFI_SSID);
  WiFi.mode(fallbackApActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 10000UL) {
    // New: keep local controls responsive even during startup/reconnect attempts.
    server.handleClient();
    if (millis() - lastSwitchScan >= SWITCH_SCAN_MS) {
      lastSwitchScan = millis();
      scanSwitches();
    }
    delay(10);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("[WiFi] Connected. STA IP: ")); Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("[WiFi] Failed. Local AP fallback remains active."));
  }
}

// ============================================================================
//  AP FALLBACK + LOCAL SERVICES (new)
// ============================================================================
void startFallbackAP() {
  if (fallbackApActive) return;

  Serial.println(F("[AP] Starting fallback AP SmartHome at 192.168.4.1..."));
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_GW, AP_MASK);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  fallbackApActive = true;
  supabaseOnline = false;
  lastRecoveryCheck = millis();

  Serial.print(F("[AP] SSID: ")); Serial.println(AP_SSID);
  Serial.print(F("[AP] IP: ")); Serial.println(WiFi.softAPIP());
}

void stopFallbackAPIfOnline() {
  if (!fallbackApActive || WiFi.status() != WL_CONNECTED || !supabaseOnline) return;

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  fallbackApActive = false;
  Serial.println(F("[AP] Fallback AP stopped. Normal STA/Supabase mode active."));
}

void beginLocalServices() {
  if (webServerStarted) return;

  setupWebServer();
  server.begin();
  webServerStarted = true;
  Serial.println(F("[Web] Local server started on port 80."));
}

// ============================================================================
//  mDNS
// ============================================================================
void setupMDNS() {
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mDNS] http://%s.local\n", MDNS_NAME);
  }
}

// ============================================================================
//  SWITCH SCANNER  (Layer 1 — always runs)
// ============================================================================
void scanSwitches() {
  bool current[8];
  for (int i = 0; i < 8; i++) {
    current[i] = (digitalRead(SWITCH_PINS[i]) == LOW); // LOW = pressed (pull-up)
  }

  if (firstSwitchRead) {
    for (int i = 0; i < 8; i++) {
      lastSwitchState[i]    = current[i];
      switchStateUploaded[i]= current[i];
    }
    firstSwitchRead = false;
    return;
  }

  bool anyChanged = false;
  for (int i = 0; i < 8; i++) {
    if (current[i] != lastSwitchState[i]) {
      anyChanged = true;

      // ── IMMEDIATE relay drive — no internet needed ──
      relayState[i] = current[i];
      digitalWrite(RELAY_PINS[i], current[i] ? RELAY_ON : RELAY_OFF);
      lastSwitchState[i] = current[i];

      char room[32];
      getRoomName(i, room, sizeof(room));
      Serial.printf("[SW] Switch %d (%s) -> %s => Relay %s (instant)\n",
                    i+1, room, current[i]?"ON":"OFF", current[i]?"ON":"OFF");
    }
  }

  if (anyChanged) {
    // Try to sync to Supabase immediately if online
    if (supabaseOnline) {
      // Build patch for only changed fields
      JsonDocument doc;
      for (int i = 0; i < 8; i++) {
        if (current[i] != switchStateUploaded[i]) {
          doc[String("switch") + (i+1)] = current[i];
          doc[String("relay")  + (i+1)] = current[i];
        }
      }
      String body;
      serializeJson(doc, body);
      if (supabasePatch(body)) {
        for (int i = 0; i < 8; i++) switchStateUploaded[i] = current[i];
      } else {
        // Supabase failed - mark offline, queue full sync, and expose AP fallback.
        supabaseOnline     = false;
        pendingSupabaseSync= true;
        lastReconnect      = millis();
        startFallbackAP();
        Serial.println(F("[SB] Patch failed - entering local AP mode."));
      }
    } else {
      // Offline: just mark that local state needs syncing when we're back
      pendingSupabaseSync = true;
    }
  }
}

// ============================================================================
//  SUPABASE POLL (online mode — pull cloud state, apply to relays)
// ============================================================================
void supabasePoll() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, SB_GET)) return;
  http.addHeader("apikey",        SB_ANON);
  http.addHeader("Authorization", String("Bearer ") + SB_ANON);
  http.setTimeout(4000);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[SB] Poll failed HTTP %d\n", code);
    http.end();
    // Count as failure - go offline and expose local AP fallback.
    supabaseOnline     = false;
    pendingSupabaseSync= true;
    lastReconnect      = millis();
    startFallbackAP();
    Serial.println(F("[SB] Going offline."));
    return;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload) || doc.as<JsonArray>().size() < 1) return;

  JsonObject row = doc.as<JsonArray>()[0];
  for (int i = 0; i < 8; i++) {
    bool on = row[String("relay") + (i+1)].as<bool>();
    if (on != relayState[i]) {
      relayState[i] = on;
      digitalWrite(RELAY_PINS[i], on ? RELAY_ON : RELAY_OFF);
    }
  }
}

// ============================================================================
//  SUPABASE PATCH helper
// ============================================================================
bool supabasePatch(const String& body) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, SB_PATCH)) return false;
  http.addHeader("apikey",        SB_ANON);
  http.addHeader("Authorization", String("Bearer ") + SB_ANON);
  http.addHeader("Content-Type",  "application/json");
  http.addHeader("Prefer",        "return=minimal");
  http.setTimeout(4000);
  int code = http.PATCH(body);
  http.end();
  return (code == 200 || code == 204);
}

// ============================================================================
//  PUSH LOCAL STATE TO SUPABASE (called on reconnect so cloud catches up)
// ============================================================================
bool supabasePushLocalState() {
  JsonDocument doc;
  for (int i = 0; i < 8; i++) {
    doc[String("relay")  + (i+1)] = relayState[i];
    doc[String("switch") + (i+1)] = lastSwitchState[i];
  }
  String body;
  serializeJson(doc, body);
  Serial.print(F("[SB] Push local state: ")); Serial.println(body);
  return supabasePatch(body);
}

// ============================================================================
//  CHECK IF SUPABASE IS REACHABLE
// ============================================================================
bool checkSupabaseReachable() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, SB_GET)) return false;
  http.addHeader("apikey",        SB_ANON);
  http.addHeader("Authorization", String("Bearer ") + SB_ANON);
  http.setTimeout(5000);
  int code = http.GET();
  http.end();
  return (code == 200);
}

// ============================================================================
//  LOCAL STATUS JSON
// ============================================================================
String buildStatusJson() {
  char room[40];
  String j = "{\"online\":";
  j += supabaseOnline ? "true" : "false";
  j += ",\"mode\":\""; j += fallbackApActive ? "ap" : "sta"; j += "\"";
  j += ",\"ip\":\""; j += (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString()); j += "\"";
  j += ",\"staIp\":\""; j += WiFi.localIP().toString(); j += "\"";
  j += ",\"apIp\":\""; j += WiFi.softAPIP().toString(); j += "\"";
  j += ",\"uptime\":"; j += millis()/1000;
  j += ",\"heap\":";  j += ESP.getFreeHeap();
  j += ",\"relays\":[";
  for (int i = 0; i < 8; i++) {
    getRoomName(i, room, sizeof(room));
    j += "{\"id\":"; j += i;
    j += ",\"relay\":"; j += (i+1);
    j += ",\"name\":\""; j += room; j += "\"";
    j += ",\"relayPin\":"; j += RELAY_PINS[i];
    j += ",\"switchPin\":"; j += SWITCH_PINS[i];
    j += ",\"state\":"; j += relayState[i] ? "true" : "false";
    j += ",\"switchOn\":"; j += lastSwitchState[i] ? "true" : "false";
    j += "}";
    if (i < 7) j += ",";
  }
  j += "]}";
  return j;
}

// ============================================================================
//  LOCAL WEB SERVER HANDLERS
// ============================================================================

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", localDashboardHTML());
}

void handleStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json; charset=utf-8", buildStatusJson());
}

void handleToggle() {
  if (!server.hasArg("id")) { server.send(400, "text/plain", "Missing id"); return; }
  int id = server.arg("id").toInt();
  if (id < 0 || id >= 8) { server.send(400, "text/plain", "Invalid id"); return; }
  relayState[id] = !relayState[id];
  digitalWrite(RELAY_PINS[id], relayState[id] ? RELAY_ON : RELAY_OFF);
  // Try to sync immediately if online; otherwise queue
  if (supabaseOnline) {
    JsonDocument doc;
    doc[String("relay") + (id+1)] = relayState[id];
    String body; serializeJson(doc, body);
    if (!supabasePatch(body)) { supabaseOnline = false; pendingSupabaseSync = true; lastReconnect = millis(); startFallbackAP(); }
  } else {
    pendingSupabaseSync = true;
  }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json; charset=utf-8", buildStatusJson());
}

void handleSetRelay() {
  if (!server.hasArg("id") || !server.hasArg("state")) { server.send(400, "text/plain", "Missing params"); return; }
  int  id    = server.arg("id").toInt();
  bool state = server.arg("state").toInt() == 1;
  if (id < 0 || id >= 8) { server.send(400, "text/plain", "Invalid id"); return; }
  relayState[id] = state;
  digitalWrite(RELAY_PINS[id], state ? RELAY_ON : RELAY_OFF);
  if (supabaseOnline) {
    JsonDocument doc;
    doc[String("relay") + (id+1)] = state;
    String body; serializeJson(doc, body);
    if (!supabasePatch(body)) { supabaseOnline = false; pendingSupabaseSync = true; lastReconnect = millis(); startFallbackAP(); }
  } else {
    pendingSupabaseSync = true;
  }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json; charset=utf-8", buildStatusJson());
}

void handleAll() {
  if (!server.hasArg("state")) { server.send(400, "text/plain", "Missing state"); return; }
  bool state = server.arg("state").toInt() == 1;
  JsonDocument doc;
  for (int i = 0; i < 8; i++) {
    relayState[i] = state;
    digitalWrite(RELAY_PINS[i], state ? RELAY_ON : RELAY_OFF);
    doc[String("relay") + (i+1)] = state;
  }
  if (supabaseOnline) {
    String body; serializeJson(doc, body);
    if (!supabasePatch(body)) { supabaseOnline = false; pendingSupabaseSync = true; lastReconnect = millis(); startFallbackAP(); }
  } else {
    pendingSupabaseSync = true;
  }
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json; charset=utf-8", buildStatusJson());
}

void setupWebServer() {
  server.on("/",           HTTP_GET,  handleRoot);
  server.on("/api/status", HTTP_GET,  handleStatus);
  server.on("/api/toggle", HTTP_POST, handleToggle);
  server.on("/api/set",    HTTP_POST, handleSetRelay);
  server.on("/api/all",    HTTP_POST, handleAll);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
}

// ============================================================================
//  LOCAL DASHBOARD HTML  (Arabic, RTL, 2×4 mobile grid, dark theme)
//  Served from flash. No internet needed to load — no external CDN links.
// ============================================================================
String localDashboardHTML() {
  // All CSS/JS is self-contained — works 100% offline
  String h = F(R"RAWHTML(<!DOCTYPE html>
<html lang="ar" dir="rtl">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>البيت الذكي — وضع محلي</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg:#080c14;--card:#111827;--card-h:#151e30;--border:#1e2a40;
  --green:#10b981;--gdim:#059669;--gglow:rgba(16,185,129,.18);
  --blue:#3b82f6;--amber:#f59e0b;
  --text:#f1f5f9;--dim:#64748b;--mid:#94a3b8;--radius:16px;
}
body{font-family:system-ui,sans-serif;background:
  radial-gradient(ellipse at 15% 0%,rgba(16,185,129,.08) 0%,transparent 40%),
  var(--bg);color:var(--text);min-height:100vh;padding:20px 14px 50px}
.wrap{max-width:960px;margin:0 auto}
header{display:flex;align-items:center;justify-content:space-between;
  flex-wrap:wrap;gap:12px;margin-bottom:22px}
.brand{display:flex;align-items:center;gap:12px}
.logo{width:42px;height:42px;border-radius:13px;flex-shrink:0;
  background:linear-gradient(135deg,var(--green),#06b6d4);
  display:flex;align-items:center;justify-content:center;
  box-shadow:0 0 22px rgba(16,185,129,.3)}
.logo svg{width:22px;height:22px;fill:none;stroke:#fff;stroke-width:2.3;
  stroke-linecap:round;stroke-linejoin:round}
.brand-text h1{font-size:1.35rem;font-weight:800}
.brand-text p{color:var(--dim);font-size:.78rem;margin-top:2px}
#badge{display:flex;align-items:center;gap:7px;padding:7px 14px;
  border-radius:999px;font-size:.78rem;font-weight:700;
  background:rgba(245,158,11,.12);border:1px solid rgba(245,158,11,.3);
  color:var(--amber);transition:.3s}
#badge.ok{background:rgba(16,185,129,.12);border-color:rgba(16,185,129,.3);color:var(--green)}
#badge.local{background:rgba(59,130,246,.12);border-color:rgba(59,130,246,.3);color:var(--blue)}
.dot{width:8px;height:8px;border-radius:50%;background:currentColor;
  animation:pulse 1.8s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}
.stats{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:20px}
.stat{flex:1;min-width:100px;background:var(--card);border:1px solid var(--border);
  border-radius:13px;padding:12px 14px}
.sv{font-size:1.5rem;font-weight:800;color:var(--green);line-height:1}
.sl{font-size:.72rem;color:var(--dim);margin-top:3px}
.toolbar{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:22px}
.btn{border:none;cursor:pointer;font-size:.85rem;font-weight:700;
  border-radius:10px;padding:10px 18px;transition:transform .15s}
.btn:active{transform:scale(.95)}
.bon{background:linear-gradient(135deg,var(--green),var(--gdim));color:#fff;
  box-shadow:0 4px 14px rgba(16,185,129,.25)}
.boff{background:linear-gradient(135deg,#ef4444,#b91c1c);color:#fff;
  box-shadow:0 4px 14px rgba(239,68,68,.2)}
.sec{font-size:.9rem;font-weight:700;color:var(--mid);margin-bottom:12px;
  display:flex;align-items:center;gap:8px}
.sec::after{content:'';flex:1;height:1px;background:var(--border)}
/* 2×4 grid on mobile, 4×2 on wider screens */
.rgrid{display:grid;grid-template-columns:repeat(2,1fr);gap:10px;margin-bottom:26px}
@media(min-width:560px){.rgrid{grid-template-columns:repeat(4,1fr);gap:14px}}
.sgrid{display:grid;grid-template-columns:repeat(2,1fr);gap:10px}
@media(min-width:560px){.sgrid{grid-template-columns:repeat(4,1fr);gap:14px}}
.rcard{background:var(--card);border:1px solid var(--border);
  border-radius:var(--radius);padding:14px;cursor:pointer;
  position:relative;overflow:hidden;transition:all .25s;
  animation:fu .35s ease backwards}
.rcard::before{content:'';position:absolute;inset:0 0 auto 0;height:3px;
  background:var(--border);transition:.25s}
.rcard.on{border-color:rgba(16,185,129,.4);box-shadow:0 8px 26px var(--gglow)}
.rcard.on::before{background:linear-gradient(90deg,var(--green),#06b6d4)}
.rcard:hover{background:var(--card-h)}
@keyframes fu{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:translateY(0)}}
.rcTop{display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:12px}
.ricon{width:34px;height:34px;border-radius:10px;background:#1a2035;
  display:flex;align-items:center;justify-content:center;flex-shrink:0;transition:.25s}
.rcard.on .ricon{background:rgba(16,185,129,.15)}
.ricon svg{width:17px;height:17px;fill:none;stroke:var(--dim);stroke-width:2;
  stroke-linecap:round;stroke-linejoin:round;transition:.25s}
.rcard.on .ricon svg{stroke:var(--green)}
.tog{position:relative;display:inline-block;width:46px;height:26px;flex-shrink:0}
.tog input{opacity:0;width:0;height:0}
.trk{position:absolute;inset:0;border-radius:26px;background:#1e2a40;
  transition:.25s;cursor:pointer}
.trk::before{content:'';position:absolute;width:18px;height:18px;border-radius:50%;
  left:4px;bottom:4px;background:#fff;box-shadow:0 1px 4px rgba(0,0,0,.3);transition:.25s}
input:checked+.trk{background:linear-gradient(135deg,var(--green),var(--gdim))}
input:checked+.trk::before{transform:translateX(20px)}
.rname{font-size:.86rem;font-weight:700;margin-bottom:2px}
.rpin{font-size:.66rem;color:var(--dim);font-family:monospace}
.rst{font-size:.7rem;font-weight:800;letter-spacing:.5px;text-transform:uppercase;
  margin-top:8px;color:var(--dim)}
.rcard.on .rst{color:var(--green)}
.scard{background:rgba(255,255,255,.02);border:1px solid var(--border);
  border-radius:var(--radius);padding:12px 14px;display:flex;
  align-items:center;gap:10px;transition:.25s;animation:fu .35s ease backwards}
.scard.on{background:rgba(59,130,246,.12);border-color:rgba(59,130,246,.35)}
.sdot{width:11px;height:11px;border-radius:50%;background:var(--border);
  flex-shrink:0;transition:.25s}
.scard.on .sdot{background:var(--blue);box-shadow:0 0 9px rgba(59,130,246,.5)}
.sinfo{}
.sname{font-size:.82rem;font-weight:700}
.spin{font-size:.65rem;color:var(--dim);font-family:monospace}
.sst{font-size:.68rem;font-weight:800;color:var(--dim);text-transform:uppercase;
  letter-spacing:.4px;margin-top:3px}
.scard.on .sst{color:var(--blue)}
footer{margin-top:28px;color:var(--dim);font-size:.72rem;
  display:flex;justify-content:space-between;flex-wrap:wrap;gap:6px;
  font-family:monospace}
#toast{position:fixed;bottom:18px;left:50%;
  transform:translateX(-50%) translateY(10px);
  background:var(--card);border:1px solid var(--border);
  padding:10px 18px;border-radius:10px;font-size:.82rem;
  opacity:0;pointer-events:none;transition:.25s;white-space:nowrap;
  box-shadow:0 8px 28px rgba(0,0,0,.4)}
#toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <div class="brand">
      <div class="logo">
        <svg viewBox="0 0 24 24"><path d="M3 9.5L12 3l9 6.5V20a1 1 0 01-1 1H4a1 1 0 01-1-1V9.5z"/><path d="M9 21V12h6v9"/></svg>
      </div>
      <div class="brand-text">
        <h1>البيت الذكي</h1>
        <p>Local Mode · ESP32 Direct</p>
      </div>
    </div>
    <div id="badge"><span class="dot"></span><span id="btext">جاري الاتصال...</span></div>
  </header>

  <div class="stats">
    <div class="stat"><div class="sv" id="s-on">0</div><div class="sl">مضاء حالياً</div></div>
    <div class="stat"><div class="sv" id="s-sw">0</div><div class="sl">مفاتيح نشطة</div></div>
    <div class="stat"><div class="sv" id="s-up">--</div><div class="sl">وقت التشغيل</div></div>
    <div class="stat"><div class="sv" id="s-heap">--</div><div class="sl">RAM المتاح</div></div>
  </div>

  <div class="toolbar">
    <button class="btn bon" onclick="allRelays(1)">⚡ تشغيل الكل</button>
    <button class="btn boff" onclick="allRelays(0)">🔴 إيقاف الكل</button>
  </div>

  <div class="sec">💡 المصابيح والأجهزة</div>
  <div class="rgrid" id="rg"></div>

  <div class="sec">🎛️ حالة المفاتيح الجدارية</div>
  <div class="sgrid" id="sg"></div>

  <footer>
    <span id="f-ip">IP: --</span>
    <span id="f-time">--</span>
  </footer>
</div>
<div id="toast"></div>

<script>
var state={online:false,relays:[]};

function render(d){
  state=d;
  var rg=document.getElementById('rg');
  var sg=document.getElementById('sg');
  var built=rg.childElementCount>0;

  d.relays.forEach(function(r,idx){
    var delay=(idx*0.05)+'s';
    if(!built){
      // Build relay card
      var rc=document.createElement('div');
      rc.id='rc'+r.id;
      rc.className='rcard'+(r.state?' on':'');
      rc.style.animationDelay=delay;
      rc.innerHTML=
        '<div class="rcTop">'+
          '<div class="ricon"><svg viewBox="0 0 24 24"><polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2"></polygon></svg></div>'+
          '<label class="tog" onclick="event.stopPropagation()">'+
            '<input type="checkbox" id="chk'+r.id+'" '+(r.state?'checked':'')+' onchange="toggleRelay('+r.id+')">'+
            '<span class="trk"></span>'+
          '</label>'+
        '</div>'+
        '<div class="rname" id="rn'+r.id+'">'+r.name+'</div>'+
        '<div class="rpin">GPIO '+r.relayPin+' · SW '+r.switchPin+'</div>'+
        '<div class="rst" id="rs'+r.id+'">'+(r.state?'ON ●':'OFF')+'</div>';
      rc.addEventListener('click',function(id){return function(){toggleRelay(id)}}(r.id));
      rg.appendChild(rc);

      // Build switch card
      var sc=document.createElement('div');
      sc.id='sc'+r.id;
      sc.className='scard'+(r.switchOn?' on':'');
      sc.style.animationDelay=delay;
      sc.innerHTML=
        '<div class="sdot" id="sd'+r.id+'"></div>'+
        '<div class="sinfo">'+
          '<div class="sname">'+r.name+'</div>'+
          '<div class="spin">GPIO '+r.switchPin+'</div>'+
          '<div class="sst" id="ss'+r.id+'">'+(r.switchOn?'مضغوط · ON':'مُفرَج · OFF')+'</div>'+
        '</div>';
      sg.appendChild(sc);
    } else {
      // Update existing cards
      var rc=document.getElementById('rc'+r.id);
      if(rc) rc.className='rcard'+(r.state?' on':'');
      var chk=document.getElementById('chk'+r.id);
      if(chk) chk.checked=r.state;
      var rs=document.getElementById('rs'+r.id);
      if(rs) rs.textContent=r.state?'ON ●':'OFF';
      var sc=document.getElementById('sc'+r.id);
      if(sc) sc.className='scard'+(r.switchOn?' on':'');
      var ss=document.getElementById('ss'+r.id);
      if(ss) ss.textContent=r.switchOn?'مضغوط · ON':'مُفرَج · OFF';
    }
  });

  // Stats
  var on=d.relays.filter(function(r){return r.state}).length;
  var sw=d.relays.filter(function(r){return r.switchOn}).length;
  document.getElementById('s-on').textContent=on;
  document.getElementById('s-sw').textContent=sw;
  document.getElementById('s-up').textContent=fmtUp(d.uptime);
  document.getElementById('s-heap').textContent=Math.round(d.heap/1024)+'K';
  document.getElementById('f-ip').textContent='IP: '+d.ip;
  document.getElementById('f-time').textContent='آخر تحديث: '+new Date().toLocaleTimeString('ar');

  // Badge
  var badge=document.getElementById('badge');
  var btext=document.getElementById('btext');
  if(d.online){
    badge.className='ok';btext.textContent='متصل بالإنترنت · Supabase Online';
  } else {
    badge.className='local';btext.textContent='وضع محلي · Local Only';
  }
}

function fmtUp(s){
  var h=Math.floor(s/3600),m=Math.floor((s%3600)/60),sec=s%60;
  return h+'h '+m+'m '+sec+'s';
}

async function fetchStatus(){
  try{
    var r=await fetch('/api/status');
    if(!r.ok) throw 0;
    render(await r.json());
  }catch(e){
    document.getElementById('btext').textContent='جهاز ESP32 غير متصل';
  }
}

async function toggleRelay(id){
  try{
    var r=await fetch('/api/toggle',{
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'id='+id
    });
    var d=await r.json();
    var room=d.relays[id];
    showToast((room.state?'✅ تم تشغيل':'🔴 تم إيقاف')+' '+room.name);
    render(d);
  }catch(e){showToast('خطأ في الاتصال');}
}

async function allRelays(s){
  try{
    var r=await fetch('/api/all',{
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'state='+s
    });
    render(await r.json());
    showToast(s?'✅ تم تشغيل جميع الأجهزة':'🔴 تم إيقاف جميع الأجهزة');
  }catch(e){showToast('خطأ في الاتصال تواصل مع عمك العشاري');}
}

var tt;
function showToast(m){
  var t=document.getElementById('toast');
  t.textContent=m;t.classList.add('show');
  clearTimeout(tt);tt=setTimeout(function(){t.classList.remove('show')},2400);
}

fetchStatus();
setInterval(fetchStatus,1500);
</script>
</body>
</html>)RAWHTML");
  return h;
}
