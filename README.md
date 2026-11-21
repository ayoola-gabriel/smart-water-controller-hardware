# Smart Water Controller — Revision 6 Firmware

PlatformIO project for the ESP32-WROOM-32 using the Arduino framework. Revision 6 follows the user's verified functional specification and GPIO assignment.

## 1. Confirmed operating modes

- **Prototype mode:** GPIO13 HIGH. Three wire electrodes detect LOW (GPIO36), MID (GPIO34), and FULL (GPIO35). GPIO32 supplies a 10ms 3.3V pulse during each sample. Wet electrodes read HIGH. Tap water is the reference water source.
- **Real mode:** GPIO13 LOW. Three float switches detect LOW (GPIO36), MID (GPIO34), and FULL (GPIO35). GPIO32 supplies stable 3.3V HIGH; each active contact reads HIGH through the external wiring. Hardware testing confirmed active-HIGH inputs. Fit external pulldowns on GPIO34/35/36. Valid patterns are 000 (below LOW), 100 (LOW), 110 (MID), and 111 (FULL); other patterns latch a sensor fault.
- Add an external 10k pulldown from GPIO13 to GND. A jumper to 3V3 selects prototype; open/GND selects real. Change mode with power removed; mode is sampled at startup.

## 2. Current measurement

- **Prototype:** unamplified 4.7Ω low-side shunt measured directly by GPIO39. There is no LM358 stage in Revision 6. Firmware calculates `I = (Vadc - zeroOffset) / 4.7`.
- **Real:** ACS712-30A measured by GPIO39. Firmware averages ADC conversions and calculates AC RMS from sampled waveform variance. The 10k/10k output divider correction is 2.0.
- If a 74HC4053/CD4053 analog mux is used, connect the prototype shunt signal to X1 and the divided ACS712 output to X0; connect select to GPIO13 so HIGH selects prototype and LOW selects real; mux common output goes to GPIO39. Power the mux at 3.3V and keep all analog pins within 0–3.3V.
- The 4.7Ω shunt drops 0.94V and dissipates 0.188W at 200mA. It can materially reduce the voltage available to a 5V pump. The unamplified ADC input is limited by the ESP32 input range; 3.3V/4.7Ω is approximately 0.70A before safety margin. Measure actual startup and running current and pump terminal voltage. Do not enable current trips until calibrated.
- Prototype zero calibration is available at `POST /api/current/calibrate?key=...`. It samples asynchronously for about 0.9 seconds, only in prototype mode with the pump OFF, and stores the result in Preferences. Inspect the serial current ADC/voltage/zero diagnostics after calibration.

## 3. Temperature

- 10kΩ B3950 NTC from 3.3V to GPIO33; fixed 10kΩ resistor from GPIO33 to GND.
- Firmware samples and averages the ADC, calculates NTC resistance and uses the Beta equation with R0=10kΩ at 25°C and B=3950K.
- At 25°C, expect approximately 1.65V at GPIO33. Accuracy depends on NTC tolerance, fixed resistor tolerance, 3.3V accuracy and ESP32 ADC calibration.

## 4. GPIO assignment

| Function | GPIO |
|---|---:|
| LCD RS / EN | 4 / 5 |
| LCD D4 / D5 / D6 / D7 | 21 / 19 / 22 / 23 |
| SIM800L TX / RX / RST | 16 / 17 / 18 |
| NEO-6M RX / TX | 2 / 15 |
| Mode select | 13 |
| Buzzer | 12 |
| LEDs (LED1 / LED2 / LED3) | 14 / 26 / 27 |
| Relay control | 25 |
| NTC ADC | 33 |
| Sensor common | 32 |
| FULL / MID / LOW | 35 / 34 / 36 |
| Shared current ADC | 39 |

Direction reminder: SIM800L TX → ESP32 GPIO16; SIM800L RX ← ESP32 GPIO17. GPS TX → ESP32 GPIO2; GPS RX ← ESP32 GPIO15.

## 5. Web API

All API routes require `?key=YOUR_API_KEY`.

- `GET /api/status?key=...` — system state, water-level flags, pump mode/state, current, temperature, fault, Wi-Fi, GSM and GPS data.
- `POST /api/control?key=...&mode=auto`
- `POST /api/control?key=...&mode=off`
- `POST /api/control?key=...&mode=manual&pump=on`
- `POST /api/control?key=...&mode=manual&pump=off`
- `POST /api/fault/clear?key=...`
- `POST /api/current/calibrate?key=...` — prototype-only, pump must be OFF.

## 6. Web OTA

1. Copy `include/local_config.example.h` to `include/local_config.h` and set the Wi-Fi credentials, local API key, Supabase project URL, publishable/anon key, device email/password/UID, plus a unique six-digit BLE pairing passkey. The local file is ignored by Git.
2. Connect the ESP32 to the configured Wi-Fi network.
3. Open `http://ESP32_IP/update?key=YOUR_API_KEY` in a browser on the trusted local network.
4. Enter the API key in the form, select the compiled `.bin` firmware file and upload.
5. The device writes to the inactive OTA app partition and restarts after a successful update.
6. Build firmware with PlatformIO; the binary is normally ` .pio/build/esp32dev/firmware.bin` (without the leading space).

`partitions_ota.csv` defines two 1.5625MB OTA application slots. OTA upload uses the same API key as the web API. This is plain HTTP, not encrypted HTTPS. Do not expose port 80 or `/update` directly to the public Internet; use only on a trusted LAN or add a secure VPN/reverse proxy. Keep power stable during upload.

## 7. Control and protection

- Pump modes: AUTO, MANUAL and OFF.
- AUTO starts in the below-LOW and LOW-only bands, retains the refill request at MID, and stops/clears the request at FULL.
- MANUAL ON is still inhibited by invalid water-level data, FULL state, a latched fault and maximum runtime. OFF stops the pump.
- Maximum runtime is 30 minutes by default.
- Overcurrent and low-current trip features are present but disabled by default until the real pump and current measurement are calibrated. Configure thresholds in `AppConfig.h` only after testing.
- Fault SMS messages include GPS coordinates when a valid fix exists.

## 8. Build and upload

Install PlatformIO Core, open this folder, and run:

```bash
pio run
pio run -t upload
pio device monitor
```

The `.bin` generated by `pio run` can be uploaded through `/update` for subsequent OTA updates.

## 9. Important hardware safety and validation

- GPIO34, GPIO35 and GPIO36 are input-only and have no internal pull-up/down. Fit external 10k pulldowns for the stated active-HIGH sensor wiring.
- GPIO12, GPIO2 and GPIO15 are ESP32 strapping pins; test repeated cold starts with peripherals connected. Keep the buzzer driver from pulling GPIO12 HIGH during reset. GPIO5 is also a strapping pin; ensure LCD EN does not force an invalid reset level.
- ESP32 GPIO is not 5V tolerant. Ensure LCD and sensor interfaces do not drive 5V into ESP32 pins. Tie LCD R/W to GND; use suitable logic buffering if required.
- SIM800L requires its own regulated supply around 4.0V with at least 2A peak capability; share ground with ESP32.
- For the 220V AC 0.5–1HP pump, use a properly rated contactor, breaker, overload protection, earthing, enclosure and independent high-high cutout. Do not switch the motor directly with the small ESP32 relay board.
- ACS712 breakout isolation and creepage/clearance must be assessed for mains use. Installation and mains wiring must be done by a qualified person.
- Verify relay polarity, float switch orientation, mux pinout, shunt voltage drop, ADC range, thermistor calibration, GPS boot behaviour and GSM supply before enabling automatic operation.

## 10. Validation status

This project is generated from the Revision 5 source and updated for Revision 6. PlatformIO builds successfully. Firmware upload and the hardware checks listed by the user have passed on their board; the corrections in this revision have not yet been retested on hardware. This is development firmware, not a certified or production-validated controller.

## 11. BLE and Wi-Fi cloud connectivity (development addition)

- BLE uses NimBLE-Arduino. Service UUID: `7c2e0001-7d5a-4f3b-9b2a-5c2d8f001001`.
- Characteristics: status notify/read (`...0002...`), pump control write (`...0003...`), profile write/read (`...0004...`), Wi-Fi provisioning write (`...0005...`).
- BLE connections require bonding, Secure Connections, MITM authentication and an encrypted link. The configured pairing passkey is shown on the LCD while pairing; the companion client must enter it once before reading status or writing controls. Credentials and profile/Wi-Fi changes are restricted to authenticated links. Bonds are retained across boots. After a failed authentication, the controller removes that peer's bond; also forget the controller on the phone before pairing again.
- Pump control JSON examples: `{"mode":"AUTO"}`, `{"mode":"OFF"}`, `{"mode":"MANUAL","pump":true}`, `{"clearFault":true}`.
- Profile JSON: `{"name":"User","phone":"+234..."}`. Wi-Fi provisioning JSON: `{"ssid":"...","password":"..."}`.
- Wi-Fi credentials are stored in ESP32 Preferences (NVS). SIM800L remains SMS-only; cellular packet data is not used.
- Telemetry is appended to LittleFS at `/telemetry.jsonl` and uploaded in batches of up to 20 records to Supabase `telemetry_history` when Wi-Fi is connected. Records are removed only after a successful PostgREST response. Unique `(device_id,event_id)` keys make retried batches idempotent. Queue size is bounded by `TELEMETRY_QUEUE_MAX_BYTES`; when full, newest samples are dropped and existing queued data is retained.
- Latest status is upserted into Supabase `device_status`; cloud commands are polled from `device_commands` where `status='pending'`.
- A cloud command row must include a unique `command_id`, Unix-second `issued_at` and `expires_at`, `mode`, and (for MANUAL) a boolean `pump`. Commands older than five minutes, expired commands, and commands with invalid timestamps or IDs are rejected. The device conditionally updates a pending row with its acknowledgement before applying an accepted command. The last processed ID is retained in NVS to prevent replay after restart.
- After a successful history upload, the remaining queue is written to a temporary LittleFS file and swapped through a backup file. Startup recovery restores the backup or temporary file if power is lost during replacement; duplicates can occur after interrupted writes, but unsent records are retained.
- Cloud HTTP, status publication and queue upload run in a low-priority FreeRTOS worker. The Wi-Fi web server/OTA handler and GSM SMS retries/sends also have separate workers. Pump-control sensor and protection work remains in the main loop; network timeouts do not hold it. BLE GATT callbacks only enqueue/update small control state.
- Supabase email/password sign-in and refresh-token exchange use `/auth/v1/token`. The returned user UUID must match `SUPABASE_DEVICE_UID`; the access token is sent as a bearer token to PostgREST and refreshed before expiry. HTTPS uses the ESP-IDF trusted certificate bundle and fails closed if the bundle is unavailable. The firmware sends the Supabase publishable/anon key and authenticated user JWT; never put a `service_role` key in firmware. Device credentials belong in the ignored `include/local_config.h`; never commit that file.
- `CLOUD_REQUIRE_FLASH_ENCRYPTION` is a separate build setting. Development defaults to `0`: Supabase Auth and verified HTTPS remain enabled, but the refresh token is stored in unencrypted NVS and a warning is printed. Set it to `1` for production; auth then fails closed until flash encryption and the encrypted NVS key partition are provisioned. The firmware never burns eFuses or calls `setInsecure()`.
- Run [`supabase/schema.sql`](supabase/schema.sql) in the Supabase SQL Editor, create a dedicated device user in Supabase Auth, and insert its device ID and Auth UUID into `device_registry`. Copy the project URL, publishable/anon key, device login and that user's Supabase UUID into the ignored local config. The SQL enables row-level security; commands should be inserted through a trusted dashboard/server context. Do not ship or expose the service-role key.
- Register the device user with `insert into public.device_registry (device_id, user_id) values ('swc-device-001', '<SUPABASE_AUTH_USER_UUID>');` using the UUID shown for the dedicated Auth user. Keep the placeholder angle-bracketed value replaced by that user's actual UUID.
- This firmware uploads new status/history and consumes Supabase commands. It does not migrate existing Firebase records or Firebase users; historical data backfill is a separate operation.
- `partitions_ota.csv` defines two 1.5625 MB OTA slots, a 688 KB LittleFS partition, a 128 KB core-dump partition, and 16 KB for `auth`/`nvs_keys`. Back up LittleFS before flashing the new table and firmware over USB. LittleFS formats only after a first-use mount failure; a later mount failure preserves the partition and disables the offline queue for recovery. The telemetry queue remains bounded at 256 KB.
- Current fault recovery is configurable in `AppConfig.h`: max-runtime faults can clear after 60 seconds only after the demand has ended. Level-sensor, current-sensor, overcurrent, and low-current faults require a manual clear. Overcurrent and low-current detection remain disabled until the sensors are calibrated.
- GPS parsing runs in a dedicated task on UART Serial2 at 115200 baud (RX GPIO2, TX GPIO15). Serial diagnostics report characters, checksum-valid/failed sentences, satellites, fix validity and age; one raw NMEA line is printed every five seconds.
- The ESP-IDF trusted certificate bundle is supplied by the selected PlatformIO Espressif platform/framework. Keep that platform updated at least twice a year so Supabase service roots stay current.
- The profile characteristic currently stores name and phone. APN is intentionally omitted because cellular data has been removed. Wi-Fi provisioning is handled separately.
- BLE control takes precedence over cloud control, which takes precedence over the legacy local HTTP control API after each source has issued a command. Restarting the device resets these runtime overrides.
- The first implementation does not include a mobile app; the UUIDs and JSON payloads above are the companion-app contract.

## 12. Thermistor troubleshooting output

The serial monitor reports `NTC: ADC=... Node=...V Rntc=... ohm TempValid=...`, current ADC/averaged voltage/zero/current values, and GPS receive/fix diagnostics. For the documented thermistor divider (3V3 → NTC → GPIO33 → fixed 10k → GND), at room temperature expect approximately 1.65V and 10kΩ. If the node voltage is near 3.1V, the computed resistance is only a few hundred ohms, which explains a high displayed temperature. Confirm the actual resistor topology and measure the NTC resistance with power disconnected before changing Beta or nominal-resistance constants.
