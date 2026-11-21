#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <cstring>
#include <time.h>
#include <NimBLEDevice.h>
#include "AppConfig.h"
#include "SupabaseAuth.h"
#include "TrustedCloudClient.h"
#include "RemoteManager.h"
#include "WebManager.h"

extern WaterData gWater;
extern SystemStatus gStatus;

static const char *SERVICE_UUID = "7c2e0001-7d5a-4f3b-9b2a-5c2d8f001001";
static const char *STATUS_UUID  = "7c2e0002-7d5a-4f3b-9b2a-5c2d8f001001";
static const char *CONTROL_UUID = "7c2e0003-7d5a-4f3b-9b2a-5c2d8f001001";
static const char *PROFILE_UUID = "7c2e0004-7d5a-4f3b-9b2a-5c2d8f001001";
static const char *WIFI_UUID    = "7c2e0005-7d5a-4f3b-9b2a-5c2d8f001001";
static const char *QUEUE_FILE = "/telemetry.jsonl";
static const char *QUEUE_TMP_FILE = "/telemetry.tmp";
static const char *QUEUE_BACKUP_FILE = "/telemetry.bak";
static const char *STATUS_TABLE_PATH = "/rest/v1/device_status";
static const char *HISTORY_TABLE_PATH = "/rest/v1/telemetry_history";
static const char *COMMAND_TABLE_PATH = "/rest/v1/device_commands";

static NimBLECharacteristic *statusCharacteristic = nullptr;
static PumpMode bleMode = AUTO_MODE;
static bool bleManualPump = false;
static bool bleClearFault = false;
static bool bleControlActive = false;
static bool blePairingPending = false;
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

static_assert(BLE_PAIRING_PASSKEY >= 100000 && BLE_PAIRING_PASSKEY <= 999999,
              "Set a unique six-digit BLE_PAIRING_PASSKEY in local_config.h");

static String supabaseUrl(const String &path) {
    String base = SUPABASE_PROJECT_URL;
    while (base.endsWith("/")) base.remove(base.length() - 1);
    return base + path;
}

static bool supabaseRequest(const String &url, const String &method, const String &body,
                            String &response, const char *prefer = "return=minimal") {
    if (WiFi.status() != WL_CONNECTED || !supabaseAccessToken().length()) return false;
    TrustedCloudClient client;
    if (!client.useSystemCertificateBundle()) return false;
    HTTPClient http;
    http.setTimeout(5000);
    if (!http.begin(client, url)) return false;
    http.addHeader("apikey", SUPABASE_ANON_KEY);
    http.addHeader("Authorization", String("Bearer ") + supabaseAccessToken());
    http.addHeader("Content-Type", "application/json");
    if (prefer && prefer[0]) http.addHeader("Prefer", prefer);
    int code = -1;
    if (method == "GET") code = http.GET();
    else if (method == "PATCH") code = http.sendRequest("PATCH", (uint8_t *)body.c_str(), body.length());
    else if (method == "POST") code = http.POST(body);
    if (code > 0) response = http.getString();
    http.end();
    if (code == 401 || code == 403) invalidateSupabaseAuthentication();
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
    JsonArray rows = batch.to<JsonArray>();
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
        if (id.length() == 0) id = String(millis()) + "-" + String(count);
        JsonObject row = rows.createNestedObject();
        row["device_id"] = DEVICE_ID;
        row["event_id"] = id;
        row["telemetry"] = record.as<JsonObject>();
        count++;
    }
    while (f.available()) remaining += f.readStringUntil('\n') + "\n";
    f.close();
    if (!count) return false;
    String payload; serializeJson(batch, payload);
    String response;
    const String url = supabaseUrl(String(HISTORY_TABLE_PATH) +
        "?on_conflict=device_id,event_id");
    if (!supabaseRequest(url, "POST", payload, response,
                         "resolution=ignore-duplicates,return=minimal")) return false;
    File out = LittleFS.open(QUEUE_TMP_FILE, FILE_WRITE);
    if (!out) return false;
    const size_t written = out.print(remaining);
    out.flush();
    const bool tempWriteOk = written == remaining.length();
    out.close();
    if (!tempWriteOk) return false;

    if (LittleFS.exists(QUEUE_BACKUP_FILE) && !LittleFS.remove(QUEUE_BACKUP_FILE)) return false;
    if (!LittleFS.rename(QUEUE_FILE, QUEUE_BACKUP_FILE)) return false;
    if (!LittleFS.rename(QUEUE_TMP_FILE, QUEUE_FILE)) {
        LittleFS.rename(QUEUE_BACKUP_FILE, QUEUE_FILE);
        return false;
    }
    LittleFS.remove(QUEUE_BACKUP_FILE);
    Serial.printf("Supabase: uploaded %u queued telemetry records\\n", count);
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
    StaticJsonDocument<1024> row;
    row["device_id"] = DEVICE_ID;
    row["telemetry"] = doc.as<JsonObject>();
    String payload; serializeJson(row, payload); String response;
    const String url = supabaseUrl(String(STATUS_TABLE_PATH) + "?on_conflict=device_id");
    supabaseRequest(url, "POST", payload, response,
                    "resolution=merge-duplicates,return=minimal");
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
        if (!prefs.begin("swc-profile", false)) {
            Serial.println("BLE profile update failed: Preferences unavailable");
            return;
        }
        if (doc["name"].is<const char*>() && strlen(doc["name"].as<const char*>()) <= 48) {
            prefs.putString("name", doc["name"].as<String>());
        }
        if (doc["phone"].is<const char*>() && strlen(doc["phone"].as<const char*>()) <= 24) {
            prefs.putString("phone", doc["phone"].as<String>());
        }
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
        if (!prefs.begin("swc-net", false)) {
            Serial.println("BLE Wi-Fi provisioning failed: Preferences unavailable");
            return;
        }
        const size_t ssidWritten = prefs.putString("ssid", ssid);
        const size_t passwordWritten = prefs.putString("password", password);
        prefs.end();
        if (!ssidWritten || !passwordWritten) {
            Serial.println("BLE Wi-Fi provisioning failed: credentials were not saved");
            return;
        }
        requestWifiCredentials();
    }
};

class DeviceServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *, ble_gap_conn_desc *desc) override {
        portENTER_CRITICAL(&commandMux);
        blePairingPending = true;
        portEXIT_CRITICAL(&commandMux);
        const int securityResult = NimBLEDevice::startSecurity(desc->conn_handle);
        if (securityResult != 0) {
            Serial.printf("BLE security negotiation could not start (%d)\n", securityResult);
        }
    }

    void onDisconnect(NimBLEServer *) override {
        portENTER_CRITICAL(&commandMux);
        blePairingPending = false;
        portEXIT_CRITICAL(&commandMux);
        NimBLEDevice::getAdvertising()->start();
    }

    uint32_t onPassKeyRequest() override {
        return BLE_PAIRING_PASSKEY;
    }

    void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
        const bool secured = desc->sec_state.encrypted && desc->sec_state.authenticated;
        portENTER_CRITICAL(&commandMux);
        blePairingPending = !secured;
        portEXIT_CRITICAL(&commandMux);
        if (secured) {
            Serial.println("BLE pairing secured with authenticated encryption");
        } else {
            Serial.println("BLE pairing failed; removing this peer's bond and disconnecting. Forget the controller on the phone before retrying.");
            NimBLEDevice::deleteBond(NimBLEAddress(desc->peer_id_addr));
            NimBLEServer *server = NimBLEDevice::getServer();
            if (server) server->disconnect(desc->conn_handle);
        }
    }
};

static DeviceServerCallbacks deviceServerCallbacks;

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
    if (!commandPrefs.begin("swc-cmd", false)) return false;
    const size_t written = commandPrefs.putString("lastId", id);
    commandPrefs.end();
    return written > 0;
}

static bool acknowledgeCloudCommand(const String &id, const char *result, const char *reason) {
    StaticJsonDocument<192> ack;
    ack["status"] = result;
    ack["reason"] = reason;
    ack["acknowledged_at"] = static_cast<uint32_t>(time(nullptr));
    String body;
    serializeJson(ack, body);
    String response;
    const String url = supabaseUrl(String(COMMAND_TABLE_PATH) +
        "?command_id=eq." + id + "&device_id=eq." + DEVICE_ID + "&status=eq.pending");
    if (!supabaseRequest(url, "PATCH", body, response, "return=representation")) return false;
    DynamicJsonDocument updated(1024);
    return !deserializeJson(updated, response) && updated.is<JsonArray>() &&
           updated.as<JsonArray>().size() > 0;
}

static void processCloudCommand(const String &payload) {
    StaticJsonDocument<2048> responseDoc;
    if (deserializeJson(responseDoc, payload) || !responseDoc.is<JsonArray>()) return;
    JsonArray commands = responseDoc.as<JsonArray>();
    if (commands.size() == 0 || !commands[0].is<JsonObject>()) return;
    JsonObject command = commands[0].as<JsonObject>();

    const char *rawId = command["command_id"] | command["commandId"] | "";
    if (!safeCommandId(rawId)) {
        Serial.println("Supabase command ignored: missing or invalid command ID");
        return;
    }
    const String id = rawId;
    if (id == lastProcessedCommandId) return;

    const time_t now = time(nullptr);
    JsonVariantConst issued = command["issued_at"];
    if (issued.isNull()) issued = command["issuedAt"];
    JsonVariantConst expires = command["expires_at"];
    if (expires.isNull()) expires = command["expiresAt"];
    const bool hasTimes = !issued.isNull() && !expires.isNull();
    const uint32_t issuedAt = issued.as<uint32_t>();
    const uint32_t expiresAt = expires.as<uint32_t>();
    String mode = command["mode"] | "";
    mode.toUpperCase();
    PumpMode newMode = AUTO_MODE;
    bool newManualPump = false;
    const char *reason = "invalid_command";

    if (!hasTimes) reason = "missing_timestamps";
    else if (expiresAt <= issuedAt || expiresAt - issuedAt > CLOUD_COMMAND_MAX_AGE_SEC) reason = "invalid_expiry";
    else if (issuedAt > static_cast<uint32_t>(now) + CLOUD_COMMAND_FUTURE_SKEW_SEC) reason = "issued_in_future";
    else if (expiresAt <= static_cast<uint32_t>(now)) reason = "expired";
    else if (static_cast<uint32_t>(now) > issuedAt &&
             static_cast<uint32_t>(now) - issuedAt > CLOUD_COMMAND_MAX_AGE_SEC) reason = "too_old";
    else if (mode == "AUTO") { newMode = AUTO_MODE; reason = nullptr; }
    else if (mode == "OFF") { newMode = PUMP_OFF_MODE; reason = nullptr; }
    else if (mode == "MANUAL" && command["pump"].is<bool>()) {
        newMode = MANUAL_MODE;
        newManualPump = command["pump"].as<bool>();
        reason = nullptr;
    } else reason = "invalid_mode_or_pump";

    const bool accepted = reason == nullptr;
    if (!acknowledgeCloudCommand(id, accepted ? "accepted" : "rejected",
                                 accepted ? "validated" : reason)) {
        Serial.println("Supabase command acknowledgement failed; command not applied");
        return;
    }
    if (!persistProcessedCommandId(id)) {
        Serial.println("Supabase command acknowledged but not applied: command ID could not be saved");
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

static void pollSupabaseCommand() {
    const String url = supabaseUrl(String(COMMAND_TABLE_PATH) +
        "?select=command_id,issued_at,expires_at,mode,pump" \
        "&device_id=eq." + DEVICE_ID + "&status=eq.pending&order=issued_at.asc&limit=1");
    String response;
    if (supabaseRequest(url, "GET", "", response)) processCloudCommand(response);
}

static void cloudWorker(void *) {
    CloudSnapshot latest{};
    bool haveSnapshot = false;
    uint32_t lastSync = 0;
    uint32_t lastPoll = 0;
    beginSupabaseAuth();
    if (commandPrefs.begin("swc-cmd", true)) {
        lastProcessedCommandId = commandPrefs.isKey("lastId")
            ? commandPrefs.getString("lastId", "") : "";
        commandPrefs.end();
    } else {
        Serial.println("Cloud replay protection unavailable: command Preferences could not open");
    }

    for (;;) {
        CloudSnapshot incoming;
        if (cloudSnapshotQueue && xQueueReceive(cloudSnapshotQueue, &incoming, pdMS_TO_TICKS(100)) == pdTRUE) {
            latest = incoming;
            haveSnapshot = true;
            appendTelemetry(latest.water, latest.status);
        }

        const uint32_t now = millis();
        const bool authenticated = ensureSupabaseAuthentication();
        if (haveSnapshot && authenticated &&
            (uint32_t)(now - lastSync) >= CLOUD_SYNC_INTERVAL_MS) {
            lastSync = now;
            publishCurrentStatus(latest.water, latest.status);
            uploadTelemetryBatch();
        }

        if (authenticated &&
            (uint32_t)(now - lastPoll) >= CLOUD_COMMAND_POLL_MS) {
            lastPoll = now;
            pollSupabaseCommand();
        }
    }
}

void beginRemoteManager() {
    fsReady = LittleFS.begin(false);
    Preferences fsPrefs;
    const bool fsPrefsReady = fsPrefs.begin("swc-fs", false);
    const bool previouslyMounted = fsPrefsReady && fsPrefs.isKey("mounted");
    if (!fsReady && fsPrefsReady && !previouslyMounted) {
        Serial.println("LittleFS first-use mount failed; formatting once for initial recovery");
        fsReady = LittleFS.format() && LittleFS.begin(false);
    }
    if (fsReady) {
        if (fsPrefsReady && !previouslyMounted) fsPrefs.putBool("mounted", true);
        if (!LittleFS.exists(QUEUE_FILE) && LittleFS.exists(QUEUE_BACKUP_FILE)) {
            LittleFS.rename(QUEUE_BACKUP_FILE, QUEUE_FILE);
        }
        if (!LittleFS.exists(QUEUE_FILE) && LittleFS.exists(QUEUE_TMP_FILE)) {
            LittleFS.rename(QUEUE_TMP_FILE, QUEUE_FILE);
        }
        if (LittleFS.exists(QUEUE_FILE)) {
            if (LittleFS.exists(QUEUE_BACKUP_FILE)) LittleFS.remove(QUEUE_BACKUP_FILE);
            if (LittleFS.exists(QUEUE_TMP_FILE)) LittleFS.remove(QUEUE_TMP_FILE);
        } else {
            File f = LittleFS.open(QUEUE_FILE, FILE_WRITE);
            if (f) f.close();
        }
    } else {
        Serial.println(previouslyMounted
            ? "LittleFS mount failed after prior use; preserving partition for recovery"
            : "LittleFS unavailable; offline telemetry queue disabled");
    }
    if (fsPrefsReady) fsPrefs.end();
    cloudSnapshotQueue = xQueueCreate(8, sizeof(CloudSnapshot));
    if (cloudSnapshotQueue) {
        xTaskCreatePinnedToCore(cloudWorker, "swc-cloud", 8192, nullptr, 1, nullptr, 0);
    } else {
        Serial.println("Cloud worker disabled: unable to allocate snapshot queue");
    }
    if (!BLE_ENABLED) return;
    NimBLEDevice::init("Smart Water Controller");
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
    NimBLEDevice::setSecurityPasskey(BLE_PAIRING_PASSKEY);
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    Serial.printf("BLE bond store loaded: %d paired device(s)\n", NimBLEDevice::getNumBonds());
    NimBLEServer *server = NimBLEDevice::createServer();
    server->setCallbacks(&deviceServerCallbacks, false);
    NimBLEService *service = server->createService(SERVICE_UUID);
    statusCharacteristic = service->createCharacteristic(STATUS_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY |
        NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN);
    auto *control = service->createCharacteristic(CONTROL_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN);
    auto *profile = service->createCharacteristic(PROFILE_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |
        NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::WRITE_ENC |
        NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::WRITE_AUTHEN);
    auto *wifi = service->createCharacteristic(WIFI_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN);
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
bool isBlePairingPending() { portENTER_CRITICAL(&commandMux); bool v = blePairingPending; portEXIT_CRITICAL(&commandMux); return v; }
uint32_t getBlePairingPasskey() { return BLE_PAIRING_PASSKEY; }
