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
#include "FirebaseAuth.h"
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
static bool bleControlActive = false;
static PumpMode cloudMode = AUTO_MODE;
static bool cloudManualPump = false;
static bool cloudControlActive = false;
static uint32_t lastNotify = 0;
static uint32_t recordSequence = 0;
static bool fsReady = false;
static Preferences prefs;
static Preferences commandPrefs;
static portMUX_TYPE commandMux = portMUX_INITIALIZER_UNLOCKED;

struct CloudSnapshot {
    WaterData water;
    SystemStatus status;
};

static QueueHandle_t cloudSnapshotQueue = nullptr;
static String lastProcessedCommandId;

static String firebaseUrl(const String &path) {
    String url = String(FIREBASE_DATABASE_URL) + path + ".json";
    if (firebaseIdToken().length()) url += "?auth=" + firebaseIdToken();
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
    if (code == 401 || code == 403) invalidateFirebaseAuthentication();
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
        if (doc["clearFault"] | false) {
            portENTER_CRITICAL(&commandMux);
            bleClearFault = true;
            portEXIT_CRITICAL(&commandMux);
            return;
        }
        PumpMode newMode;
        bool newManualPump = false;
        if (!strcmp(mode, "AUTO")) newMode = AUTO_MODE;
        else if (!strcmp(mode, "OFF")) newMode = PUMP_OFF_MODE;
        else if (!strcmp(mode, "MANUAL")) { newMode = MANUAL_MODE; newManualPump = doc["pump"] | false; }
        else return;
        portENTER_CRITICAL(&commandMux);
        bleMode = newMode;
        bleManualPump = newManualPump;
        bleControlActive = true;
        portEXIT_CRITICAL(&commandMux);
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

static bool safeCommandId(const char *id) {
    const size_t length = strlen(id);
    if (!length || length > 64) return false;
    for (size_t i = 0; i < length; ++i) {
        const char ch = id[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) return false;
    }
    return true;
}

static bool persistProcessedCommandId(const String &id) {
    commandPrefs.begin("swc-cmd", false);
    const size_t written = commandPrefs.putString("lastId", id);
    commandPrefs.end();
    return written > 0;
}

static bool acknowledgeCloudCommand(const String &id, const char *result, const char *reason) {
    StaticJsonDocument<256> ack;
    ack["commandId"] = id;
    ack["status"] = result;
    ack["reason"] = reason;
    ack["acknowledgedAt"] = static_cast<uint32_t>(time(nullptr));
    String body;
    serializeJson(ack, body);
    String response;
    const String path = String("/devices/") + DEVICE_ID + "/commandAcks/" + id;
    return httpsRequest(firebaseUrl(path), "PUT", body, response);
}

static void processCloudCommand(const String &payload) {
    StaticJsonDocument<512> command;
    if (deserializeJson(command, payload)) return;

    const char *rawId = command["commandId"] | "";
    if (!safeCommandId(rawId)) {
        Serial.println("Cloud command ignored: missing or invalid command ID");
        return;
    }
    const String id = rawId;
    if (id == lastProcessedCommandId) return;

    const time_t now = time(nullptr);
    const bool hasTimes = command["issuedAt"].is<uint32_t>() && command["expiresAt"].is<uint32_t>();
    const uint32_t issuedAt = command["issuedAt"] | 0UL;
    const uint32_t expiresAt = command["expiresAt"] | 0UL;
    const char *mode = command["mode"] | "";
    PumpMode newMode = AUTO_MODE;
    bool newManualPump = false;
    const char *reason = "invalid_command";

    if (!hasTimes) reason = "missing_timestamps";
    else if (expiresAt <= issuedAt || expiresAt - issuedAt > CLOUD_COMMAND_MAX_AGE_SEC) reason = "invalid_expiry";
    else if (issuedAt > static_cast<uint32_t>(now) + CLOUD_COMMAND_FUTURE_SKEW_SEC) reason = "issued_in_future";
    else if (expiresAt <= static_cast<uint32_t>(now)) reason = "expired";
    else if (static_cast<uint32_t>(now) > issuedAt &&
             static_cast<uint32_t>(now) - issuedAt > CLOUD_COMMAND_MAX_AGE_SEC) reason = "too_old";
    else if (!strcmp(mode, "AUTO")) { newMode = AUTO_MODE; reason = nullptr; }
    else if (!strcmp(mode, "OFF")) { newMode = PUMP_OFF_MODE; reason = nullptr; }
    else if (!strcmp(mode, "MANUAL") && command["pump"].is<bool>()) {
        newMode = MANUAL_MODE;
        newManualPump = command["pump"].as<bool>();
        reason = nullptr;
    } else reason = "invalid_mode_or_pump";

    const bool accepted = reason == nullptr;
    if (!acknowledgeCloudCommand(id, accepted ? "accepted" : "rejected",
                                 accepted ? "validated" : reason)) return;
    if (!persistProcessedCommandId(id)) {
        Serial.println("Cloud command acknowledged but not applied: command ID could not be saved");
        return;
    }
    lastProcessedCommandId = id;

    if (accepted) {
        portENTER_CRITICAL(&commandMux);
        cloudMode = newMode;
        cloudManualPump = newManualPump;
        cloudControlActive = true;
        portEXIT_CRITICAL(&commandMux);
    }
}

static void cloudWorker(void *) {
    CloudSnapshot latest{};
    bool haveSnapshot = false;
    uint32_t lastSync = 0;
    uint32_t lastPoll = 0;
    beginFirebaseAuth();
    commandPrefs.begin("swc-cmd", true);
    lastProcessedCommandId = commandPrefs.getString("lastId", "");
    commandPrefs.end();

    for (;;) {
        CloudSnapshot incoming;
        if (cloudSnapshotQueue && xQueueReceive(cloudSnapshotQueue, &incoming, pdMS_TO_TICKS(100)) == pdTRUE) {
            latest = incoming;
            haveSnapshot = true;
            appendTelemetry(latest.water, latest.status);
        }

        const uint32_t now = millis();
        const bool authenticated = ensureFirebaseAuthentication();
        if (haveSnapshot && authenticated &&
            (uint32_t)(now - lastSync) >= CLOUD_SYNC_INTERVAL_MS) {
            lastSync = now;
            publishCurrentStatus(latest.water, latest.status);
            uploadTelemetryBatch();
        }

        if (authenticated &&
            (uint32_t)(now - lastPoll) >= CLOUD_COMMAND_POLL_MS) {
            lastPoll = now;
            String response;
            if (httpsRequest(firebaseUrl(String("/devices/") + DEVICE_ID + "/commands"), "GET", "", response)) {
                processCloudCommand(response);
            }
        }
    }
}

void beginRemoteManager() {
    fsReady = LittleFS.begin(false);
    if (fsReady && !LittleFS.exists(QUEUE_FILE)) { File f = LittleFS.open(QUEUE_FILE, FILE_WRITE); if (f) f.close(); }
    cloudSnapshotQueue = xQueueCreate(8, sizeof(CloudSnapshot));
    if (cloudSnapshotQueue) {
        xTaskCreatePinnedToCore(cloudWorker, "swc-cloud", 8192, nullptr, 1, nullptr, 0);
    } else {
        Serial.println("Cloud worker disabled: unable to allocate snapshot queue");
    }
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
    static uint32_t lastSnapshot = 0;
    if (cloudSnapshotQueue && (uint32_t)(now - lastSnapshot) >= CLOUD_SYNC_INTERVAL_MS) {
        lastSnapshot = now;
        CloudSnapshot snapshot{water, status};
        if (xQueueSend(cloudSnapshotQueue, &snapshot, 0) != pdTRUE) {
            Serial.println("Cloud snapshot queue full; telemetry sample skipped");
        }
    }
}

PumpMode getBlePumpMode() { portENTER_CRITICAL(&commandMux); PumpMode v = bleMode; portEXIT_CRITICAL(&commandMux); return v; }
bool getBleManualPumpRequest() { portENTER_CRITICAL(&commandMux); bool v = bleManualPump; portEXIT_CRITICAL(&commandMux); return v; }
bool isBleControlActive() { portENTER_CRITICAL(&commandMux); bool v = bleControlActive; portEXIT_CRITICAL(&commandMux); return v; }
bool consumeBleClearFaultRequest() { portENTER_CRITICAL(&commandMux); bool v = bleClearFault; bleClearFault = false; portEXIT_CRITICAL(&commandMux); return v; }
PumpMode getCloudPumpMode() { portENTER_CRITICAL(&commandMux); PumpMode v = cloudMode; portEXIT_CRITICAL(&commandMux); return v; }
bool getCloudManualPumpRequest() { portENTER_CRITICAL(&commandMux); bool v = cloudManualPump; portEXIT_CRITICAL(&commandMux); return v; }
bool isCloudControlActive() { portENTER_CRITICAL(&commandMux); bool v = cloudControlActive; portEXIT_CRITICAL(&commandMux); return v; }
