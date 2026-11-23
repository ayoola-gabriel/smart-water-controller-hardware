#pragma once
#include <Arduino.h>

// User-confirmed ESP32-WROOM-32 pin assignment (Revision 6).
// LCD: direct 4-bit HD44780 interface; R/W tied to GND.
constexpr uint8_t PIN_LCD_RS = 4;
constexpr uint8_t PIN_LCD_EN = 5;
constexpr uint8_t PIN_LCD_D4 = 21;
constexpr uint8_t PIN_LCD_D5 = 19;
constexpr uint8_t PIN_LCD_D6 = 22;
constexpr uint8_t PIN_LCD_D7 = 23;

// GPIO13: HIGH = prototype, LOW = real installation. Also drives the current-sensor mux select input.
constexpr uint8_t PIN_MODE_SELECT = 13;
constexpr uint8_t PIN_PUMP_RELAY = 25;

// Three individually controlled LEDs; colors/functions are configurable in Outputs.cpp.
constexpr uint8_t PIN_LED_1 = 14; //YEL
constexpr uint8_t PIN_LED_2 = 26; //GREEN
constexpr uint8_t PIN_LED_3 = 27;  //RED
constexpr uint8_t PIN_BUZZER = 12;

// SIM800L module TX -> ESP32 RX GPIO16; module RX <- ESP32 TX GPIO17.
constexpr int PIN_GSM_RX = 16;
constexpr int PIN_GSM_TX = 17;
constexpr uint8_t PIN_GSM_RESET = 18;

// NEO-6M module TX -> ESP32 RX GPIO15; module RX <- ESP32 TX GPIO2.
constexpr int PIN_GPS_RX = 15;
constexpr int PIN_GPS_TX = 2;

// Shared level inputs: wire electrodes in prototype mode; float switches in real mode.
constexpr uint8_t PIN_SENSOR_COMMON = 32; // Pulsed 3V3 in prototype; steady 3V3 in real mode.
constexpr uint8_t PIN_NTC = 33;            // 10k NTC divider node; ADC1 input.
constexpr uint8_t PIN_LEVEL_FULL = 35;     // Input-only; external bias resistor required.
constexpr uint8_t PIN_LEVEL_MID = 34;      // Input-only; external bias resistor required.
constexpr uint8_t PIN_LEVEL_LOW = 36;      // Input-only; external bias resistor required.
constexpr uint8_t PIN_PUMP_CURRENT = 39;   // Shared current-sensor ADC input.
