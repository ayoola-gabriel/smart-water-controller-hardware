#pragma once

#if __has_include("local_config.h")
#include "local_config.h"
#else
#error "Copy include/local_config.example.h to include/local_config.h and set local credentials."
#endif

// Revision 6 application configuration. Replace placeholders before deployment.
constexpr bool RELAY_ACTIVE_LOW = false; // Verify relay module input polarity.
constexpr uint32_t MODE_SETTLE_MS = 30;
constexpr uint8_t LCD_COLS = 20;
constexpr uint8_t LCD_ROWS = 4;

// Sensor logic: GPIO32 is pulsed in prototype mode and held HIGH in real mode.
constexpr uint8_t ELECTRODE_WET_STATE = HIGH;
constexpr uint8_t FLOAT_ACTIVE_STATE = HIGH;
constexpr uint32_t SENSOR_UPDATE_MS = 50;
constexpr uint16_t ELECTRODE_PULSE_US = 10000;

// 10k B3950 NTC: 3V3 -> NTC -> GPIO33 -> fixed 10k -> GND.
constexpr float NTC_NOMINAL_RESISTANCE_OHMS = 10000.0f;
constexpr float NTC_NOMINAL_TEMPERATURE_K = 298.15f;
constexpr float NTC_BETA_K = 3950.0f;
constexpr float NTC_FIXED_RESISTOR_OHMS = 10000.0f;
constexpr float NTC_DIVIDER_SUPPLY_V = 3.3f;
constexpr uint8_t NTC_ADC_SAMPLES = 16;
constexpr uint32_t TEMPERATURE_UPDATE_MS = 1000;

// Prototype: unamplified low-side 4.7-ohm shunt directly measured by GPIO39.
// The shunt voltage is V=I*R. At 0.2A this is 0.94V and 0.188W.
constexpr float SHUNT_OHMS = 4.7f;
constexpr float PROTOTYPE_ZERO_OFFSET_V_DEFAULT = 0.010f;

// Real installation: ACS712-30A module. Assumes its output is divided by 1.5
// before GPIO39 (e.g. 10k series / 20k to GND). Verify actual module output.
constexpr float ACS712_SENSITIVITY_V_PER_A = 0.066f;
constexpr float ACS712_DIVIDER_CORRECTION = 1.5f;
constexpr bool ENABLE_OVERCURRENT_PROTECTION = false; // Calibrate before enabling.
constexpr float PROTOTYPE_OVERCURRENT_A = 0.60f;
constexpr float REAL_OVERCURRENT_A = 8.0f;
constexpr uint32_t OVERCURRENT_TRIP_DELAY_MS = 1000;
constexpr bool ENABLE_LOW_CURRENT_FAULT = false; // Calibrate before enabling.
constexpr float PROTOTYPE_MIN_RUNNING_CURRENT_A = 0.03f;
constexpr float REAL_MIN_RUNNING_CURRENT_A = 0.20f;
constexpr uint32_t LOW_CURRENT_TRIP_DELAY_MS = 5000;

constexpr uint32_t MAX_PUMP_RUNTIME_MS = 30UL * 60UL * 1000UL;
constexpr uint32_t PUMP_STARTUP_GRACE_MS = 3000;
constexpr uint32_t CONTROL_UPDATE_MS = 50;
constexpr uint32_t DISPLAY_UPDATE_MS = 1000;
constexpr uint32_t GSM_BAUD = 9600;
constexpr uint32_t GPS_BAUD = 115200; // Confirmed by NEO-6M diagnostic.
constexpr bool GPS_ENABLED = true;
constexpr bool GSM_ENABLED = true;
constexpr char ALERT_PHONE_NUMBER[] = "+234XXXXXXXXXX";
constexpr bool WIFI_ENABLED = true;
constexpr uint16_t WEB_PORT = 80;
constexpr uint32_t BUZZER_TOGGLE_MS = 250;


// Cloud / BLE configuration. Secrets are supplied by ignored local_config.h.
constexpr bool BLE_ENABLED = true;
constexpr bool FIREBASE_ENABLED = true;
constexpr char DEVICE_ID[] = "swc-device-001"; // Set a unique ID per controller.
constexpr char FIREBASE_DEVICE_UID[] = "iunwURhsG8Z35wlyD7hqEJD5e1A2";
constexpr char FIREBASE_DATABASE_URL[] = "https://smart-water-controller-c2558-default-rtdb.europe-west1.firebasedatabase.app";
constexpr uint32_t CLOUD_SYNC_INTERVAL_MS = 15000;
constexpr uint32_t CLOUD_COMMAND_POLL_MS = 5000;
constexpr uint32_t CLOUD_COMMAND_MAX_AGE_SEC = 300;
constexpr uint32_t CLOUD_COMMAND_FUTURE_SKEW_SEC = 60;
constexpr uint16_t TELEMETRY_BATCH_SIZE = 20;
constexpr size_t TELEMETRY_QUEUE_MAX_BYTES = 256 * 1024;
constexpr char FIREBASE_ROOT_CA[] = "REPLACE_WITH_FIREBASE_TRUSTED_ROOT_CA_PEM";
