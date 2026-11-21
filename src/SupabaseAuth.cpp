#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <stdlib.h>
#include <esp_flash_encrypt.h>
#include <esp_partition.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "AppConfig.h"
#include "SupabaseAuth.h"
#include "TrustedCloudClient.h"

static constexpr char AUTH_NAMESPACE[] = "swc-auth";
static constexpr char REFRESH_TOKEN_KEY[] = "refresh";
static constexpr char AUTH_NVS_PARTITION[] = "auth";
static constexpr char NVS_KEYS_PARTITION[] = "nvs_keys";
static constexpr uint32_t AUTH_RETRY_DELAY_MS = 60000;
static constexpr time_t VALID_CLOCK_EPOCH = 1700000000;

static String currentAccessToken;
static String currentRefreshToken;
static uint32_t refreshAtMs = 0;
static uint32_t retryAfterMs = 0;
static bool ntpStarted = false;
static bool authNvsReady = false;

static String supabaseBaseUrl() {
    String url = SUPABASE_PROJECT_URL;
    while (url.endsWith("/")) url.remove(url.length() - 1);
    return url;
}

static bool saveRefreshToken(const String &token) {
    if (!authNvsReady) return false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(AUTH_NVS_PARTITION, AUTH_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return false;
    err = nvs_set_str(handle, REFRESH_TOKEN_KEY, token.c_str());
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

static void loadRefreshToken() {
    if (!authNvsReady) return;
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(AUTH_NVS_PARTITION, AUTH_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK) return;

    size_t length = 0;
    err = nvs_get_str(handle, REFRESH_TOKEN_KEY, nullptr, &length);
    if (err == ESP_OK && length > 1 && length <= 2048) {
        char *buffer = static_cast<char *>(malloc(length));
        if (buffer) {
            if (nvs_get_str(handle, REFRESH_TOKEN_KEY, buffer, &length) == ESP_OK) {
                currentRefreshToken = buffer;
            }
            free(buffer);
        }
    }
    nvs_close(handle);
}

static bool initializeCloudAuthNvs() {
    if (!esp_flash_encryption_enabled()) {
#if CLOUD_REQUIRE_FLASH_ENCRYPTION
        Serial.println("Supabase auth disabled: production requires ESP32 flash encryption");
        return false;
#else
        Serial.println("WARNING: Supabase development mode; refresh-token NVS is not protected by flash encryption");
        const esp_err_t err = nvs_flash_init_partition(AUTH_NVS_PARTITION);
        if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES && err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
            Serial.printf("Supabase development NVS initialization failed (%d)\n", static_cast<int>(err));
            return false;
        }
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            Serial.println("Cloud auth NVS needs explicit erase/reprovisioning; refusing to erase tokens automatically");
            return false;
        }
        authNvsReady = true;
        return true;
#endif
    }

    const esp_partition_t *keyPartition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, NVS_KEYS_PARTITION);
    if (!keyPartition) {
        Serial.println("Supabase auth disabled: encrypted NVS key partition is missing");
        return false;
    }

    nvs_sec_cfg_t securityConfig{};
    esp_err_t err = nvs_flash_read_security_cfg(keyPartition, &securityConfig);
    if (err == ESP_ERR_NVS_KEYS_NOT_INITIALIZED) {
        err = nvs_flash_generate_keys(keyPartition, &securityConfig);
    }
    if (err != ESP_OK) {
        Serial.printf("Supabase auth disabled: NVS key setup failed (%d)\n", static_cast<int>(err));
        return false;
    }

    err = nvs_flash_secure_init_partition(AUTH_NVS_PARTITION, &securityConfig);
    if (err != ESP_OK) {
        Serial.printf("Supabase auth disabled: encrypted NVS initialization failed (%d)\n", static_cast<int>(err));
        return false;
    }
    authNvsReady = true;
    return true;
}

static bool ensureClock() {
    if (time(nullptr) >= VALID_CLOCK_EPOCH) return true;
    if (!ntpStarted) {
        configTime(0, 0, "time.google.com", "pool.ntp.org");
        ntpStarted = true;
    }

    const uint32_t started = millis();
    while ((uint32_t)(millis() - started) < 10000) {
        if (time(nullptr) >= VALID_CLOCK_EPOCH) return true;
        delay(100);
    }
    return false;
}

static bool postAuthRequest(const String &url, const String &body, String &response) {
    if (WiFi.status() != WL_CONNECTED || !ensureClock()) return false;
    TrustedCloudClient client;
    if (!client.useSystemCertificateBundle()) {
        Serial.println("Supabase auth paused: trusted certificate bundle is unavailable");
        return false;
    }
    HTTPClient http;
    http.setTimeout(7000);
    if (!http.begin(client, url)) return false;
    http.addHeader("apikey", SUPABASE_ANON_KEY);
    http.addHeader("Content-Type", "application/json");
    const int code = http.POST(body);
    if (code > 0) response = http.getString();
    http.end();
    return code >= 200 && code < 300;
}

static void clearSavedRefreshToken() {
    currentRefreshToken = "";
    if (!authNvsReady) return;
    nvs_handle_t handle;
    if (nvs_open_from_partition(AUTH_NVS_PARTITION, AUTH_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    const esp_err_t err = nvs_erase_key(handle, REFRESH_TOKEN_KEY);
    if (err == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

static bool acceptTokenResponse(const String &response) {
    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, response)) return false;

    const char *uid = doc["user"]["id"] | "";
    const char *accessToken = doc["access_token"] | "";
    const char *refreshToken = doc["refresh_token"] | "";
    if (!uid[0] || !accessToken[0] || !refreshToken[0]) return false;

    if (String(uid) != SUPABASE_DEVICE_UID) {
        Serial.println("Supabase auth rejected: authenticated user UID does not match this device");
        clearSavedRefreshToken();
        currentAccessToken = "";
        return false;
    }

    const uint32_t expiresSeconds = doc["expires_in"] | 0UL;
    if (expiresSeconds < 180) return false;
    currentAccessToken = accessToken;
    currentRefreshToken = refreshToken;
    if (!saveRefreshToken(currentRefreshToken)) {
        currentAccessToken = "";
        Serial.println("Supabase auth token not retained: cloud NVS write failed");
        return false;
    }

    const uint32_t safeSeconds = expiresSeconds > 120 ? expiresSeconds - 120 : expiresSeconds / 2;
    refreshAtMs = millis() + safeSeconds * 1000UL;
    retryAfterMs = 0;
    return true;
}

static bool configurationReady() {
    return String(SUPABASE_PROJECT_URL).startsWith("https://") &&
           String(SUPABASE_PROJECT_URL).indexOf("YOUR_PROJECT_REF") < 0 &&
           !String(SUPABASE_ANON_KEY).startsWith("YOUR_") &&
           !String(SUPABASE_DEVICE_EMAIL).startsWith("YOUR_") &&
           !String(SUPABASE_DEVICE_PASSWORD).startsWith("YOUR_") &&
           !String(SUPABASE_DEVICE_UID).startsWith("YOUR_");
}

static bool signInWithPassword() {
    if (!configurationReady()) {
        Serial.println("Supabase auth paused: local project URL, publishable key, device login or UID is not configured");
        return false;
    }

    StaticJsonDocument<256> request;
    request["email"] = SUPABASE_DEVICE_EMAIL;
    request["password"] = SUPABASE_DEVICE_PASSWORD;
    String body;
    serializeJson(request, body);

    String response;
    const String url = supabaseBaseUrl() + "/auth/v1/token?grant_type=password";
    if (!postAuthRequest(url, body, response)) return false;
    return acceptTokenResponse(response);
}

static bool refreshAccessToken() {
    if (!currentRefreshToken.length()) return false;
    StaticJsonDocument<256> request;
    request["refresh_token"] = currentRefreshToken;
    String body;
    serializeJson(request, body);

    String response;
    const String url = supabaseBaseUrl() + "/auth/v1/token?grant_type=refresh_token";
    if (!postAuthRequest(url, body, response)) return false;
    return acceptTokenResponse(response);
}

void beginSupabaseAuth() {
    if (initializeCloudAuthNvs()) loadRefreshToken();
}

bool ensureSupabaseAuthentication() {
    if (!SUPABASE_ENABLED || !authNvsReady || WiFi.status() != WL_CONNECTED) return false;
    if (currentAccessToken.length() && static_cast<int32_t>(millis() - refreshAtMs) < 0) return true;
    if (retryAfterMs && static_cast<int32_t>(millis() - retryAfterMs) < 0) return false;
    if (!ensureClock()) {
        retryAfterMs = millis() + AUTH_RETRY_DELAY_MS;
        return false;
    }

    currentAccessToken = "";
    bool ok = refreshAccessToken();
    if (!ok) ok = signInWithPassword();
    if (!ok) retryAfterMs = millis() + AUTH_RETRY_DELAY_MS;
    return ok;
}

void invalidateSupabaseAuthentication() {
    currentAccessToken = "";
    refreshAtMs = 0;
    retryAfterMs = 0;
}

const String &supabaseAccessToken() {
    return currentAccessToken;
}
