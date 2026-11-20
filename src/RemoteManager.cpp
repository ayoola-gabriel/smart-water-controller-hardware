#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <cstring>
#include <time.h>
#include <NimBLEDevice.h>
#include "AppConfig.h"
#include "RemoteManager.h"

extern WaterData gWater;
extern SystemStatus gStatus;

static const char *SERVICE_UUID = "7c2e0001-7d5a-4f3b-9b2a-5c2d8f001001";
static const char *STATUS_UUID  = "7c2e0002-7d5a-4f3b-9b2a-5c2d8f001001";
static const char *CONTROL_UUID = "7c2e0003-7d5a-4f3b-9b2a-5c2d8f001001";
static const char *PROFILE_UUID = "7c2e0004-7d5a-4f3b-9b2a-5c2d8f001001";
static const char *WIFI_UUID    = "7c2e0005-7d5a-4f3b-9b2a-5c2d8f001001";
static const char *QUEUE_FILE = "/telemetry.jsonl";
static const char *HISTORY_PATH = "/devices/";

static NimBLECharacteristic *statusCharacteristic = nullptr;
static PumpMode bleMode = AUTO_MODE;
static bool bleManualPump = false;
static bool bleClearFault = false;
static bool bleCommandPending = false;
static bool bleControlActive = false;
static uint32_t lastCloudSync = 0;
static uint32_t lastCommandPoll = 0;
static uint32_t lastNotify = 0;
static uint32_t recordSequence = 0;
static bool fsReady = false;
static Preferences prefs;

static String firebaseUrl(const String &path) {
    String url = String(FIREBASE_DATABASE_URL) + path + ".json";
    if (strlen(FIREBASE_AUTH_TOKEN) > 10 && String(FIREBASE_AUTH_TOKEN) != "REPLACE_WITH_SHORT_LIVED_ID_TOKEN")
        url += "?auth=" + String(FIREBASE_AUTH_TOKEN);
    return url;
}

static bool httpsRequest(const String &url, const String &method, const String &body, String &response) {
    if (WiFi.status() != WL_CONNECTED) return false;
    WiFiClientSecure client;
    if (String(FIREBASE_ROOT_CA).startsWith("-----BEGIN CERTIFICATE-----")) client.setCACert(FIREBASE_ROOT_CA);
    else {
        Serial.println("Firebase TLS disabled: install the trusted root CA in AppConfig.h");
        return false;
    }
    HTTPClient http;
    http.setTimeout(5000);
    if (!http.begin(client, url)) return false;
    http.addHeader("Content-Type", "application/json");
    int code = -1;
    if (method == "GET") code = http.GET();
    else if (method == "PATCH") code = http.sendRequest("PATCH", (uint8_t *)body.c_str(), body.length());
    else if (method == "PUT") code = http.PUT(body);
    if (code > 0) response = http.getString();
    http.end();
    return code >= 200 && code < 300;
}

static void appendTelemetry(const WaterData &w, const SystemStatus &s) {
    if (!fsReady) return;
    File f = LittleFS.open(QUEUE_FILE, FILE_APPEND);
    if (!f) return;
    StaticJsonDocument<512> doc;
    doc["id"] = String(millis()) + "-" + String(++recordSequence);
    doc["uptimeMs"] = millis();
    const time_t epochNow = time(nullptr);
    if (epochNow > 1700000000) doc["timestamp"] = static_cast<uint32_t>(epochNow);
    doc["waterLevel"] = w.fullLevel ? "FULL" : (w.midLevel ? "BETWEEN" : (w.lowLevel ? "LOW" : "EMPTY"));
    doc["levelValid"] = w.levelValid;
    doc["levelPercent"] = w.levelPercent;
    doc["pumpRunning"] = s.pumpCommand;
    doc["pumpMode"] = s.pumpMode == AUTO_MODE ? "AUTO" : (s.pumpMode == MANUAL_MODE ? "MANUAL" : "OFF");
    doc["temperatureValid"] = s.temperatureValid;
    if (s.temperatureValid) doc["temperatureC"] = s.temperatureC;
    doc["temperatureNodeV"] = s.temperatureNodeV;
    doc["currentA"] = s.pumpCurrentA;
    doc["fault"] = static_cast<int>(s.fault);
    doc["gpsValid"] = s.gpsValid;
    if (s.gpsValid) { doc["latitude"] = s.gpsLatitude; doc["longitude"] = s.gpsLongitude; }
    String line; serializeJson(doc, line);
    if (LittleFS.usedBytes() + line.length() + 2 <= TELEMETRY_QUEUE_MAX_BYTES) {
        f.println(line);
    } else {
        Serial.println("Telemetry queue full; oldest records retained, new sample dropped");
    }
    f.close();
}

static bool uploadTelemetryBatch() {
    if (!fsReady || WiFi.status() != WL_CONNECTED) return false;
    File f = LittleFS.open(QUEUE_FILE, FILE_READ);
    if (!f || f.size() == 0) { if (f) f.close(); return true; }
    DynamicJsonDocument batch(12288);
    JsonObject root = batch.to<JsonObject>();
    String remaining;
    uint16_t count = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.length()) continue;
        if (count >= TELEMETRY_BATCH_SIZE) { remaining += line + "\n"; continue; }
        StaticJsonDocument<512> record;
        if (deserializeJson(record, line)) { remaining += line + "\n"; continue; }
        const char* recordId = record["id"] | "";
        String id = recordId;

        if (id.length() == 0) {
            id = String(millis());
            id += "_";
            id += String(count);
        }
        id.replace(".", "_"); id.replace("$", "_"); id.replace("#", "_");
        id.replace("[", "_"); id.replace("]", "_"); id.replace("/", "_");
        root[id] = record.as<JsonObject>();
        count++;
    }
    while (f.available()) remaining += f.readStringUntil('\n') + "\n";
    f.close();
    if (!count) return false;
    String payload; serializeJson(batch, payload);
    String response;
    const String path = String(HISTORY_PATH) + DEVICE_ID + "/history";
    if (!httpsRequest(firebaseUrl(path), "PATCH", payload, response)) return false;
    File out = LittleFS.open("/telemetry.tmp", FILE_WRITE);
    if (!out) return false;
    out.print(remaining); out.close();
    LittleFS.remove(QUEUE_FILE);
    LittleFS.rename("/telemetry.tmp", QUEUE_FILE);
    Serial.printf("Firebase: uploaded %u queued telemetry records\\n", count);
    return true;
}

static void publishCurrentStatus(const WaterData &w, const SystemStatus &s) {
    StaticJsonDocument<768> doc;
    doc["waterLevel"] = w.fullLevel ? "FULL" : (w.midLevel ? "BETWEEN" : (w.lowLevel ? "LOW" : "EMPTY"));
    doc["levelValid"] = w.levelValid; doc["levelPercent"] = w.levelPercent;
    doc["pumpRunning"] = s.pumpCommand;
    doc["pumpMode"] = s.pumpMode == AUTO_MODE ? "AUTO" : (s.pumpMode == MANUAL_MODE ? "MANUAL" : "OFF");
    doc["temperatureValid"] = s.temperatureValid;
    if (s.temperatureValid) doc["temperatureC"] = s.temperatureC;
    doc["temperatureNodeV"] = s.temperatureNodeV;
    doc["currentA"] = s.pumpCurrentA; doc["fault"] = static_cast<int>(s.fault);
    doc["wifiConnected"] = WiFi.status() == WL_CONNECTED;
    doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
    doc["gpsValid"] = s.gpsValid;
    if (s.gpsValid) { doc["latitude"] = s.gpsLatitude; doc["longitude"] = s.gpsLongitude; }
    doc["uptimeMs"] = millis();
    const time_t epochNow = time(nullptr);
    if (epochNow > 1700000000) doc["timestamp"] = static_cast<uint32_t>(epochNow);
    String payload; serializeJson(doc, payload); String response;
    httpsRequest(firebaseUrl(String("/devices/") + DEVICE_ID + "/status"), "PUT", payload, response);
}

class ControlCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c) override {
        std::string value = c->getValue();
        StaticJsonDocument<192> doc;
        if (deserializeJson(doc, value)) return;
        const char *mode = doc["mode"] | "";
        if (!strcmp(mode, "AUTO")) { bleMode = AUTO_MODE; bleManualPump = false; }
        else if (!strcmp(mode, "OFF")) { bleMode = PUMP_OFF_MODE; bleManualPump = false; }
        else if (!strcmp(mode, "MANUAL")) { bleMode = MANUAL_MODE; bleManualPump = doc["pump"] | false; }
        else if (doc["clearFault"] | false) bleClearFault = true;
        else return;
        bleCommandPending = true;
        bleControlActive = true;
    }
};

class ProfileCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c) override {
        std::string value = c->getValue();
        StaticJsonDocument<384> doc;
        if (deserializeJson(doc, value)) return;
        prefs.begin("swc-profile", false);
        if (doc["name"].is<const char*>()) prefs.putString("name", doc["name"].as<String>());
        if (doc["phone"].is<const char*>()) prefs.putString("phone", doc["phone"].as<String>());
        prefs.end();
    }
};

class WifiCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c) override {
        std::string value = c->getValue();
        StaticJsonDocument<256> doc;
        if (deserializeJson(doc, value)) return;
        const char *ssid = doc["ssid"] | "";
        const char *password = doc["password"] | "";
        if (!strlen(ssid) || strlen(ssid) > 32 || strlen(password) > 63) return;
        prefs.begin("swc-net", false);
        prefs.putString("ssid", ssid); prefs.putString("password", password);
        prefs.end();
        WiFi.disconnect(); WiFi.begin(ssid, password);
    }
};

void beginRemoteManager() {
    fsReady = LittleFS.begin(false);
    if (fsReady && !LittleFS.exists(QUEUE_FILE)) { File f = LittleFS.open(QUEUE_FILE, FILE_WRITE); if (f) f.close(); }
    if (!BLE_ENABLED) return;
    NimBLEDevice::init("Smart Water Controller");
    NimBLEServer *server = NimBLEDevice::createServer();
    NimBLEService *service = server->createService(SERVICE_UUID);
    statusCharacteristic = service->createCharacteristic(STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    auto *control = service->createCharacteristic(CONTROL_UUID, NIMBLE_PROPERTY::WRITE);
    auto *profile = service->createCharacteristic(PROFILE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    auto *wifi = service->createCharacteristic(WIFI_UUID, NIMBLE_PROPERTY::WRITE);
    control->setCallbacks(new ControlCallbacks());
    profile->setCallbacks(new ProfileCallbacks());
    wifi->setCallbacks(new WifiCallbacks());
    profile->setValue("{}");
    service->start();
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(SERVICE_UUID);
    advertising->start();
}

void updateRemoteManager(const WaterData &water, SystemStatus &status) {
    const uint32_t now = millis();
    if (statusCharacteristic && now - lastNotify >= 2000) {
        lastNotify = now;
        StaticJsonDocument<512> doc;
        doc["levelValid"] = water.levelValid; doc["levelPercent"] = water.levelPercent;
        doc["low"] = water.lowLevel; doc["mid"] = water.midLevel; doc["full"] = water.fullLevel;
        doc["pump"] = status.pumpCommand; doc["temperatureValid"] = status.temperatureValid;
        if (status.temperatureValid) doc["temperatureC"] = status.temperatureC;
        doc["currentA"] = status.pumpCurrentA; doc["fault"] = static_cast<int>(status.fault);
        doc["wifi"] = WiFi.status() == WL_CONNECTED;
        String json; serializeJson(doc, json); statusCharacteristic->setValue(json.c_str());
        statusCharacteristic->notify();
    }
    if (now - lastCloudSync >= CLOUD_SYNC_INTERVAL_MS) {
        lastCloudSync = now;
        appendTelemetry(water, status);
        if (WiFi.status() == WL_CONNECTED) {
            publishCurrentStatus(water, status);
            uploadTelemetryBatch();
        }
    }
    if (WiFi.status() == WL_CONNECTED && now - lastCommandPoll >= CLOUD_COMMAND_POLL_MS) {
        lastCommandPoll = now;
        String response;
        if (httpsRequest(firebaseUrl(String("/devices/") + DEVICE_ID + "/commands"), "GET", "", response)) {
            StaticJsonDocument<256> command;
            if (!deserializeJson(command, response)) {
                const char *mode = command["mode"] | "";
                if (!strcmp(mode, "AUTO")) { bleMode = AUTO_MODE; bleManualPump = false; bleCommandPending = true; bleControlActive = true; }
                else if (!strcmp(mode, "OFF")) { bleMode = PUMP_OFF_MODE; bleManualPump = false; bleCommandPending = true; bleControlActive = true; }
                else if (!strcmp(mode, "MANUAL")) { bleMode = MANUAL_MODE; bleManualPump = command["pump"] | false; bleCommandPending = true; bleControlActive = true; }
            }
        }
    }
}

PumpMode getBlePumpMode() { return bleMode; }
bool getBleManualPumpRequest() { return bleManualPump; }
bool isBleControlActive() { return bleControlActive; }
bool consumeBleClearFaultRequest() { bool v = bleClearFault; bleClearFault = false; return v; }
