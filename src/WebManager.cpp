#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
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
static bool webCurrentCalibrationRequest = false;
static portMUX_TYPE webStateMux = portMUX_INITIALIZER_UNLOCKED;
static bool wifiCredentialChangePending = false;
static portMUX_TYPE wifiCredentialMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t lastWifiRetryMs = 0;
static String wifiSsid;
static String wifiPassword;
static bool serverStarted = false;
static bool routesRegistered = false;
static void webServerTask(void *parameter);

static void setWebControl(PumpMode mode, bool manualPump) {
    portENTER_CRITICAL(&webStateMux);
    webPumpMode = mode;
    webManualPumpRequest = manualPump;
    portEXIT_CRITICAL(&webStateMux);
}

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
            "Smart Water Controller Revision 6. Endpoints: /api/status, /api/control, /api/fault/clear");
    });

    server.on("/api/status", HTTP_GET, []() {
        if (!authorized()) {
            server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
            return;
        }
        String json = "{";
        json += "\"firmware\":\"Revision 6\",";
        json += "\"mode\":\"" + String(gStatus.operatingMode == PROTOTYPE_MODE ? "PROTOTYPE" : "REAL") + "\",";
        portENTER_CRITICAL(&webStateMux);
        const PumpMode modeSnapshot = webPumpMode;
        portEXIT_CRITICAL(&webStateMux);
        json += "\"pumpMode\":\"" + String(modeName(modeSnapshot)) + "\",";
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
            setWebControl(AUTO_MODE, false);
        } else if (mode == "off") {
            setWebControl(PUMP_OFF_MODE, false);
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
            setWebControl(MANUAL_MODE, pump.equalsIgnoreCase("on"));
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
        portENTER_CRITICAL(&webStateMux);
        webClearFault = true;
        portEXIT_CRITICAL(&webStateMux);
        server.send(200, "application/json", "{\"accepted\":true}");
    });

    server.on("/api/current/calibrate", HTTP_POST, []() {
        if (!authorized()) {
            server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
            return;
        }
        portENTER_CRITICAL(&webStateMux);
        webCurrentCalibrationRequest = true;
        portEXIT_CRITICAL(&webStateMux);
        server.send(202, "application/json", "{\"accepted\":true,\"note\":\"calibration starts only when pump is OFF and mode is PROTOTYPE\"}");
    });

}

void beginWebManager() {
    if (!WIFI_ENABLED) return;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    Preferences netPrefs;
    // Create the namespace on first boot; missing Wi-Fi keys still fall back
    // to the compiled local credentials below.
    if (netPrefs.begin("swc-net", false)) {
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
    if (xTaskCreatePinnedToCore(webServerTask, "swc-web", 6144,
                                nullptr, 1, nullptr, 0) != pdPASS) {
        Serial.println("Web server task could not start; web control unavailable");
    }
}

static void serviceWebManager() {
    if (!WIFI_ENABLED) return;
    portENTER_CRITICAL(&wifiCredentialMux);
    const bool credentialsChanged = wifiCredentialChangePending;
    wifiCredentialChangePending = false;
    portEXIT_CRITICAL(&wifiCredentialMux);
    if (credentialsChanged) {
        Preferences netPrefs;
        if (netPrefs.begin("swc-net", false)) {
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
        if (!routesRegistered) {
            registerRoutes();
            routesRegistered = true;
        }
        server.begin();
        serverStarted = true;
    }
    if (WiFi.status() != WL_CONNECTED && serverStarted) {
        server.stop();
        serverStarted = false;
    }
    if (serverStarted) server.handleClient();
}

static void webServerTask(void *) {
    for (;;) {
        serviceWebManager();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void requestWifiCredentials() {
    portENTER_CRITICAL(&wifiCredentialMux);
    wifiCredentialChangePending = true;
    portEXIT_CRITICAL(&wifiCredentialMux);
}

void publishWebStatus(const WaterData &, const SystemStatus &) {}
PumpMode getWebPumpMode() {
    portENTER_CRITICAL(&webStateMux);
    const PumpMode mode = webPumpMode;
    portEXIT_CRITICAL(&webStateMux);
    return mode;
}
bool getWebManualPumpRequest() {
    portENTER_CRITICAL(&webStateMux);
    const bool manualPump = webManualPumpRequest;
    portEXIT_CRITICAL(&webStateMux);
    return manualPump;
}
bool consumeWebClearFaultRequest() {
    portENTER_CRITICAL(&webStateMux);
    const bool request = webClearFault;
    webClearFault = false;
    portEXIT_CRITICAL(&webStateMux);
    return request;
}
bool consumeWebCurrentCalibrationRequest() {
    portENTER_CRITICAL(&webStateMux);
    const bool request = webCurrentCalibrationRequest;
    webCurrentCalibrationRequest = false;
    portEXIT_CRITICAL(&webStateMux);
    return request;
}
