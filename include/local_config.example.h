#pragma once

// Copy this file to local_config.h and fill in device-specific credentials.
// local_config.h is ignored by Git and must never be committed.
constexpr char WIFI_SSID[] = "YOUR_WIFI_SSID";
constexpr char WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";
constexpr char API_KEY[] = "REPLACE_WITH_RANDOM_LOCAL_API_KEY";

#define SUPABASE_PROJECT_URL "https://YOUR_PROJECT_REF.supabase.co"
#define SUPABASE_ANON_KEY "YOUR_SUPABASE_PUBLISHABLE_OR_ANON_KEY"
#define SUPABASE_DEVICE_EMAIL "YOUR_SUPABASE_DEVICE_EMAIL"
#define SUPABASE_DEVICE_PASSWORD "YOUR_SUPABASE_DEVICE_PASSWORD"
#define SUPABASE_DEVICE_UID "YOUR_SUPABASE_AUTH_USER_UUID"
// Development allows refresh-token storage without flash encryption, with a warning.
// Set to 1 for production; cloud auth then fails closed until encryption is provisioned.
#define CLOUD_REQUIRE_FLASH_ENCRYPTION 0
// Set a unique six-digit passkey per controller before building.
constexpr uint32_t BLE_PAIRING_PASSKEY = 0;
