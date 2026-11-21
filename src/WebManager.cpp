#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include "AppConfig.h"
#include "Types.h"
#include "WebManager.h"

extern WaterData gWater;
extern SystemStatus gStatus;

static WebServer server(WEB_PORT);
static PumpMode webPumpMode = AUTO_MODE;
static bool webManualPumpRequest = false;
static bool webClearFault = false;
static bool wifiCredentialChangePending = false;
static portMUX_TYPE wifiCredentialMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t lastWifiRetryMs = 0;
static String wifiSsid;
static String wifiPassword;
static bool serverStarted = false;
static bool otaAuthorizedForUpload = false;
static bool otaUploadOk = false;
static String otaError;

static bool authorized() {
    return server.hasArg("key") && server.arg("key").length() >= 16 &&
           server.arg("key") == API_KEY;
}

static const char *modeName(PumpMode mode) {
    switch (mode) {
        case AUTO_MODE: return "AUTO";
        case MANUAL_MODE: return "MANUAL";
        default: return "OFF";
    }
}

static const char *faultName(FaultType fault) {
    switch (fault) {
        case LEVEL_SENSOR_FAULT: return "LEVEL_SENSOR";
        case CURRENT_SENSOR_FAULT: return "CURRENT_SENSOR";
        case LOW_CURRENT_FAULT: return "LOW_CURRENT";
        case OVERCURRENT_FAULT: return "OVERCURRENT";
        case MAX_RUNTIME_FAULT: return "MAX_RUNTIME";
        default: return "NONE";
    }
}

static void registerRoutes() {
    server.on("/", HTTP_GET, []() {
        server.send(200, "text/plain",
            "Smart Water Controller Revision 6. Endpoints: /api/status, /api/control, /api/fault/clear, /update");
    });

    server.on("/api/status", HTTP_GET, []() {
        if (!authorized()) {
            server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
            return;
        }
        String json = "{";
        json += "\"firmware\":\"Revision 6\",";
        json += "\"mode\":\"" + String(gStatus.operatingMode == PROTOTYPE_MODE ? "PROTOTYPE" : "REAL") + "\",";
        json += "\"pumpMode\":\"" + String(modeName(webPumpMode)) + "\",";
        json += "\"pump\":\"" + String(gStatus.pumpCommand ? "ON" : "OFF") + "\",";
        json += "\"pumpState\":\"" + String(gStatus.pumpState == PUMP_FAULT ? "FAULT" : (gStatus.pumpCommand ? "RUNNING" : "STOPPED")) + "\",";
        json += "\"levelPercent\":" + String(gWater.levelPercent, 1) + ",";
        json += "\"levelValid\":" + String(gWater.levelValid ? "true" : "false") + ",";
        json += "\"lowLevel\":" + String(gWater.lowLevel ? "true" : "false") + ",";
        json += "\"midLevel\":" + String(gWater.midLevel ? "true" : "false") + ",";
        json += "\"fullLevel\":" + String(gWater.fullLevel ? "true" : "false") + ",";
        json += "\"currentA\":" + String(gStatus.pumpCurrentA, 3) + ",";
        json += "\"temperatureValid\":" + String(gStatus.temperatureValid ? "true" : "false") + ",";
        json += "\"temperatureC\":" + String(gStatus.temperatureC, 2) + ",";
        json += "\"fault\":\"" + String(faultName(gStatus.fault)) + "\",";
        json += "\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
        json += "\"wifiRssi\":" + String(WiFi.RSSI()) + ",";
        json += "\"gsmReady\":" + String(gStatus.gsmReady ? "true" : "false") + ",";
        json += "\"gpsValid\":" + String(gStatus.gpsValid ? "true" : "false") + ",";
        json += "\"latitude\":" + String(gStatus.gpsLatitude, 6) + ",";
        json += "\"longitude\":" + String(gStatus.gpsLongitude, 6) + ",";
        json += "\"satellites\":" + String(gStatus.gpsSatellites);
        json += "}";
        server.send(200, "application/json", json);
    });

    server.on("/api/control", HTTP_POST, []() {
        if (!authorized()) {
            server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
            return;
        }
        if (!server.hasArg("mode")) {
            server.send(400, "application/json", "{\"error\":\"missing mode\"}");
            return;
        }
        String mode = server.arg("mode");
        mode.toLowerCase();
        if (mode == "auto") {
            webPumpMode = AUTO_MODE;
            webManualPumpRequest = false;
        } else if (mode == "off") {
            webPumpMode = PUMP_OFF_MODE;
            webManualPumpRequest = false;
        } else if (mode == "manual") {
            if (!server.hasArg("pump")) {
                server.send(400, "application/json", "{\"error\":\"manual mode requires pump=on or pump=off\"}");
                return;
            }
            const String pump = server.arg("pump");
            if (!pump.equalsIgnoreCase("on") && !pump.equalsIgnoreCase("off")) {
                server.send(400, "application/json", "{\"error\":\"pump must be on or off\"}");
                return;
            }
            webPumpMode = MANUAL_MODE;
            webManualPumpRequest = pump.equalsIgnoreCase("on");
        } else {
            server.send(400, "application/json", "{\"error\":\"invalid mode\"}");
            return;
        }
        server.send(200, "application/json", "{\"accepted\":true}");
    });

    server.on("/api/fault/clear", HTTP_POST, []() {
        if (!authorized()) {
            server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
            return;
        }
        webClearFault = true;
        server.send(200, "application/json", "{\"accepted\":true}");
    });

    // Browser-based OTA page. The API key is submitted as a query parameter
    // to the POST route; use only on a trusted LAN, not over public Internet.
    server.on("/update", HTTP_GET, []() {
        if (!authorized()) {
            server.send(401, "text/plain", "Unauthorized. Supply the configured API key as ?key=...");
            return;
        }
        const String html = R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>Water Controller OTA</title></head><body style="font-family:Arial;max-width:560px;margin:40px auto;padding:16px"><h2>Smart Water Controller — Firmware Update</h2><p>Select a compiled ESP32 firmware .bin file.</p><form id="f" method="POST" action="/update" enctype="multipart/form-data"><label>API key<br><input id="k" type="password" required style="width:100%;padding:10px"></label><p><input type="file" name="firmware" accept=".bin,application/octet-stream" required></p><button style="padding:12px 20px">Upload firmware</button></form><p>Keep the controller powered during the update. It will restart after a successful upload.</p><script>document.getElementById('f').addEventListener('submit',function(e){e.preventDefault();this.action='/update?key='+encodeURIComponent(document.getElementById('k').value);this.submit();});</script></body></html>)HTML";
        server.send(200, "text/html", html);
    });

    server.on("/update", HTTP_POST,
        []() {
            if (!authorized()) {
                server.send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
                return;
            }
            if (otaUploadOk && !Update.hasError()) {
                server.send(200, "application/json", "{\"ok\":true,\"message\":\"Firmware uploaded. Restarting.\"}");
                delay(700);
                ESP.restart();
            } else {
                String msg = otaError.length() ? otaError : String("Upload failed: ") + Update.errorString();
                server.send(500, "application/json", String("{\"ok\":false,\"error\":\"") + msg + "\"}");
            }
        },
        []() {
            HTTPUpload &upload = server.upload();
            if (upload.status == UPLOAD_FILE_START) {
                otaAuthorizedForUpload = authorized();
                otaUploadOk = false;
                otaError = "";
                if (!otaAuthorizedForUpload) {
                    otaError = "Unauthorized";
                    return;
                }
                if (!upload.filename.endsWith(".bin")) {
                    otaError = "Only .bin firmware files are accepted";
                    otaAuthorizedForUpload = false;
                    return;
                }
                if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
                    otaError = String("Unable to begin OTA: ") + Update.errorString();
                    otaAuthorizedForUpload = false;
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (otaAuthorizedForUpload && !otaError.length()) {
                    const size_t written = Update.write(upload.buf, upload.currentSize);
                    if (written != upload.currentSize) {
                        otaError = String("Flash write failed: ") + Update.errorString();
                        Update.abort();
                    }
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (otaAuthorizedForUpload && !otaError.length()) {
                    otaUploadOk = Update.end(true);
                    if (!otaUploadOk) otaError = String("OTA validation failed: ") + Update.errorString();
                }
            } else if (upload.status == UPLOAD_FILE_ABORTED) {
                otaError = "Upload aborted";
                Update.abort();
                otaUploadOk = false;
            }
        }
    );
}

void beginWebManager() {
    if (!WIFI_ENABLED) return;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    Preferences netPrefs;
    if (netPrefs.begin("swc-net", true)) {
        wifiSsid = netPrefs.isKey("ssid") ? netPrefs.getString("ssid", "") : WIFI_SSID;
        wifiPassword = netPrefs.isKey("password") ? netPrefs.getString("password", "") : WIFI_PASSWORD;
        netPrefs.end();
    } else {
        Serial.println("Wi-Fi Preferences unavailable; using compiled local credentials");
        wifiSsid = WIFI_SSID;
        wifiPassword = WIFI_PASSWORD;
    }
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    lastWifiRetryMs = millis();
}

void handleWebManager() {
    if (!WIFI_ENABLED) return;
    portENTER_CRITICAL(&wifiCredentialMux);
    const bool credentialsChanged = wifiCredentialChangePending;
    wifiCredentialChangePending = false;
    portEXIT_CRITICAL(&wifiCredentialMux);
    if (credentialsChanged) {
        Preferences netPrefs;
        if (netPrefs.begin("swc-net", true)) {
            wifiSsid = netPrefs.isKey("ssid") ? netPrefs.getString("ssid", "") : "";
            wifiPassword = netPrefs.isKey("password") ? netPrefs.getString("password", "") : "";
            netPrefs.end();
        }
        WiFi.disconnect(false, false);
        WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
        lastWifiRetryMs = millis();
    }
    if (WiFi.status() != WL_CONNECTED && (uint32_t)(millis() - lastWifiRetryMs) >= 15000) {
        lastWifiRetryMs = millis();
        if (wifiSsid.length()) WiFi.reconnect();
        else Serial.println("Wi-Fi reconnect skipped: SSID is not configured");
    }
    if (WiFi.status() == WL_CONNECTED && !serverStarted) {
        Serial.print("IP:");
        Serial.println(WiFi.localIP());
        registerRoutes();
        server.begin();
        serverStarted = true;
    }
    if (WiFi.status() != WL_CONNECTED && serverStarted) {
        server.stop();
        serverStarted = false;
    }
    if (serverStarted) server.handleClient();
}

void requestWifiCredentials() {
    portENTER_CRITICAL(&wifiCredentialMux);
    wifiCredentialChangePending = true;
    portEXIT_CRITICAL(&wifiCredentialMux);
}

void publishWebStatus(const WaterData &, const SystemStatus &) {}
PumpMode getWebPumpMode() { return webPumpMode; }
bool getWebManualPumpRequest() { return webManualPumpRequest; }
bool consumeWebClearFaultRequest() {
    if (!webClearFault) return false;
    webClearFault = false;
    return true;
}
