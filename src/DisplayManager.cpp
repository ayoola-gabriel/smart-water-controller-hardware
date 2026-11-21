#include <Arduino.h>
#include <LiquidCrystal.h>
#include <stdio.h>
#include <string.h>
#include "AppConfig.h"
#include "Hardware.h"
#include "DisplayManager.h"

static LiquidCrystal lcd(PIN_LCD_RS, PIN_LCD_EN, PIN_LCD_D4,
                         PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7);
static char previousRows[LCD_ROWS][LCD_COLS + 1] = {};

static void formatRow(char (&row)[LCD_COLS + 1], const char *format,
                      const char *first = "", const char *second = "") {
    memset(row, ' ', LCD_COLS);
    row[LCD_COLS] = '\0';
    if (format[0] == '\0') return;
    snprintf(row, sizeof(row), format, first, second);
    const size_t textLength = strnlen(row, LCD_COLS);
    if (textLength < LCD_COLS) memset(row + textLength, ' ', LCD_COLS - textLength);
    row[LCD_COLS] = '\0';
}

static void updateRow(uint8_t rowIndex, const char (&row)[LCD_COLS + 1]) {
    uint8_t column = 0;
    while (column < LCD_COLS) {
        if (row[column] == previousRows[rowIndex][column]) {
            ++column;
            continue;
        }
        lcd.setCursor(column, rowIndex);
        while (column < LCD_COLS && row[column] != previousRows[rowIndex][column]) {
            lcd.write(static_cast<uint8_t>(row[column]));
            previousRows[rowIndex][column] = row[column];
            ++column;
        }
    }
}

void beginDisplay() {
    lcd.begin(LCD_COLS, LCD_ROWS);
    lcd.clear(); // One-time initialization only.
    memset(previousRows, 0, sizeof(previousRows));
}

void renderDisplay(const WaterData &water, const SystemStatus &status,
                   OperatingMode mode, bool pairingPending, uint32_t passkey) {
    char rows[LCD_ROWS][LCD_COLS + 1];
    const char *level = !water.levelValid ? "INVALID" :
        (water.fullLevel ? "FULL" : (water.midLevel ? "MID" :
        (water.lowLevel ? "LOW" : "EMPTY")));
    const char *pumpMode = status.pumpMode == AUTO_MODE ? "AUTO" :
        (status.pumpMode == MANUAL_MODE ? "MANUAL" : "OFF");
    char modeLabel[10];
    snprintf(modeLabel, sizeof(modeLabel), "%c:%s",
             mode == PROTOTYPE_MODE ? 'P' : 'R', pumpMode);
    formatRow(rows[0], "W:%-7s %-8s", level, modeLabel);

    char temp[10];
    if (status.temperatureValid) snprintf(temp, sizeof(temp), "%.1fC", status.temperatureC);
    else snprintf(temp, sizeof(temp), "--.-C");
    char pumpTemp[32];
    snprintf(pumpTemp, sizeof(pumpTemp), "Pump:%s T:%s",
             status.pumpState == PUMP_FAULT ? "FAULT" :
             (status.pumpCommand ? "ON" : "OFF"), temp);
    memset(rows[1], ' ', LCD_COLS);
    rows[1][LCD_COLS] = '\0';
    memcpy(rows[1], pumpTemp, strnlen(pumpTemp, LCD_COLS));

    if (status.gpsValid) {
        snprintf(rows[2], sizeof(rows[2]), "I:%.2fA GPS:%02luSAT",
                 status.pumpCurrentA, static_cast<unsigned long>(status.gpsSatellites));
    } else {
        snprintf(rows[2], sizeof(rows[2]), "I:%.2fA GPS:--",
                 status.pumpCurrentA);
    }
    if (strlen(rows[2]) < LCD_COLS) memset(rows[2] + strlen(rows[2]), ' ', LCD_COLS - strlen(rows[2]));
    rows[2][LCD_COLS] = '\0';

    memset(rows[3], ' ', LCD_COLS);
    rows[3][LCD_COLS] = '\0';
    if (pairingPending) {
        snprintf(rows[3], sizeof(rows[3]), "BLE PIN: %06lu", static_cast<unsigned long>(passkey));
    } else if (status.fault != NO_FAULT) {
        const char *fault = status.fault == LEVEL_SENSOR_FAULT ? "LEVEL SENSOR" :
            (status.fault == CURRENT_SENSOR_FAULT ? "CURRENT SENSOR" :
            (status.fault == LOW_CURRENT_FAULT ? "LOW CURRENT" :
            (status.fault == OVERCURRENT_FAULT ? "OVERCURRENT" : "MAX RUNTIME")));
        snprintf(rows[3], sizeof(rows[3]), "FAULT:%s", fault);
    } else if (status.gpsValid) {
        snprintf(rows[3], sizeof(rows[3]), "%+.4f,%+.4f",
                 status.gpsLatitude, status.gpsLongitude);
    } else {
        snprintf(rows[3], sizeof(rows[3]), "GPS: WAITING FOR FIX");
    }
    if (strlen(rows[3]) < LCD_COLS) memset(rows[3] + strlen(rows[3]), ' ', LCD_COLS - strlen(rows[3]));
    rows[3][LCD_COLS] = '\0';

    for (uint8_t row = 0; row < LCD_ROWS; ++row) updateRow(row, rows[row]);
}
