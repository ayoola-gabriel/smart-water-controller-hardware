#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <stdlib.h>

#include "AppConfig.h"
#include "FirebaseAuth.h"

static constexpr char AUTH_NAMESPACE[] = "swc-auth";
static constexpr char REFRESH_TOKEN_KEY[] = "refresh";
static constexpr uint32_t AUTH_RETRY_DELAY_MS = 60000;
static constexpr time_t VALID_CLOCK_EPOCH = 1700000000;

static Preferences authPrefs;
static String currentIdToken;
static String currentRefreshToken;
static uint32_t refreshAtMs = 0;
static uint32_t retryAfterMs = 0;
static bool ntpStarted = false;
static bool ensureClock() {
    if (time(nullptr) >= VALID_CLOCK_EPOCH) {
        return true;
    }
    if (!ntpStarted) {
        configTime(0, 0, "time.google.com", "pool.ntp.org");
        ntpStarted = true;
    }

    const uint32_t started = millis();
    while ((uint32_t)(millis() - started) < 10000) {
        if (time(nullptr) >= VALID_CLOCK_EPOCH) {
            return true;
        }
        delay(100);
    }
    return false;
}

static bool postAuthRequest(const String &url, const char *contentType,
                            const String &body, String &response) {
    if (WiFi.status() != WL_CONNECTED || !ensureClock()) return false;
    if (!String(FIREBASE_ROOT_CA).startsWith("-----BEGIN CERTIFICATE-----")) {
        Serial.println("Firebase auth paused: trusted CA PEM is not configured");
        return false;
    }

    WiFiClientSecure client;
    client.setCACert(FIREBASE_ROOT_CA);
    HTTPClient http;
    http.setTimeout(7000);
    if (!http.begin(client, url)) return false;
    http.addHeader("Content-Type", contentType);
    const int code = http.POST(body);
    if (code > 0) response = http.getString();
    http.end();
    return code >= 200 && code < 300;
}

static String formEncode(const String &value) {
    static const char HEX_DIGITS[] = "0123456789ABCDEF";
    String encoded;
    encoded.reserve(value.length() * 3);
    for (size_t i = 0; i < value.length(); ++i) {
        const uint8_t ch = static_cast<uint8_t>(value[i]);
        const bool unreserved = (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (unreserved) encoded += static_cast<char>(ch);
        else {
            encoded += '%';
            encoded += HEX_DIGITS[ch >> 4];
            encoded += HEX_DIGITS[ch & 0x0F];
        }
    }
    return encoded;
}

static void clearSavedRefreshToken() {
    currentRefreshToken = "";
    authPrefs.begin(AUTH_NAMESPACE, false);
    authPrefs.remove(REFRESH_TOKEN_KEY);
    authPrefs.end();
}

static bool acceptTokenResponse(const String &response, const char *uidField) {
    DynamicJsonDocument doc(1536);
    if (deserializeJson(doc, response)) return false;

    const char *uid = doc[uidField] | "";
    const char *idToken = doc["idToken"] | doc["id_token"] | "";
    const char *refreshToken = doc["refreshToken"] | doc["refresh_token"] | "";
    if (!uid[0] || !idToken[0] || !refreshToken[0]) return false;

    if (String(uid) != FIREBASE_DEVICE_UID) {
        Serial.println("Firebase auth rejected: authenticated UID does not match device UID");
        clearSavedRefreshToken();
        currentIdToken = "";
        return false;
    }

    JsonVariantConst expiry = doc["expiresIn"];
    if (expiry.isNull()) expiry = doc["expires_in"];
    const uint32_t expiresSeconds = expiry.is<const char *>()
        ? static_cast<uint32_t>(strtoul(expiry.as<const char *>(), nullptr, 10))
        : expiry.as<uint32_t>();
    if (expiresSeconds < 180) return false;

    currentIdToken = idToken;
    currentRefreshToken = refreshToken;
    authPrefs.begin(AUTH_NAMESPACE, false);
    const size_t stored = authPrefs.putString(REFRESH_TOKEN_KEY, currentRefreshToken);
    authPrefs.end();
    if (stored == 0) {
        currentIdToken = "";
        Serial.println("Firebase auth token not retained: NVS write failed");
        return false;
    }

    const uint32_t safeSeconds = expiresSeconds > 120 ? expiresSeconds - 120 : expiresSeconds / 2;
    refreshAtMs = millis() + safeSeconds * 1000UL;
    retryAfterMs = 0;
    return true;
}

static bool signInWithPassword() {
    if (String(FIREBASE_API_KEY).startsWith("YOUR_") ||
        String(FIREBASE_EMAIL).startsWith("YOUR_") ||
        String(FIREBASE_PASSWORD).startsWith("YOUR_")) {
        Serial.println("Firebase auth paused: local API key or device login is not configured");
        return false;
    }

    StaticJsonDocument<256> request;
    request["email"] = FIREBASE_EMAIL;
    request["password"] = FIREBASE_PASSWORD;
    request["returnSecureToken"] = true;
    String body;
    serializeJson(request, body);

    String response;
    const String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") + FIREBASE_API_KEY;
    if (!postAuthRequest(url, "application/json", body, response)) return false;
    return acceptTokenResponse(response, "localId");
}

static bool refreshIdToken() {
    if (!currentRefreshToken.length()) return false;
    const String body = String("grant_type=refresh_token&refresh_token=") + formEncode(currentRefreshToken);
    const String url = String("https://securetoken.googleapis.com/v1/token?key=") + FIREBASE_API_KEY;
    String response;
    if (!postAuthRequest(url, "application/x-www-form-urlencoded", body, response)) return false;
    return acceptTokenResponse(response, "user_id");
}

void beginFirebaseAuth() {
    authPrefs.begin(AUTH_NAMESPACE, true);
    currentRefreshToken = authPrefs.getString(REFRESH_TOKEN_KEY, "");
    authPrefs.end();
}

bool ensureFirebaseAuthentication() {
    if (!FIREBASE_ENABLED || WiFi.status() != WL_CONNECTED) return false;
    if (currentIdToken.length() && static_cast<int32_t>(millis() - refreshAtMs) < 0) return true;
    if (retryAfterMs && static_cast<int32_t>(millis() - retryAfterMs) < 0) return false;
    if (!ensureClock()) {
        retryAfterMs = millis() + AUTH_RETRY_DELAY_MS;
        return false;
    }

    currentIdToken = "";
    bool ok = refreshIdToken();
    if (!ok) ok = signInWithPassword();
    if (!ok) retryAfterMs = millis() + AUTH_RETRY_DELAY_MS;
    return ok;
}

void invalidateFirebaseAuthentication() {
    currentIdToken = "";
    refreshAtMs = 0;
    retryAfterMs = 0;
}

const String &firebaseIdToken() {
    return currentIdToken;
}
