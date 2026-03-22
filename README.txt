BikeSens Gateway (Arduino IDE, ESP32)

Purpose:
- Collect BLE advertisements from BikeSens sensor nodes.
- Keep only unique records identified by: device_id + seq.
- Buffer records locally and send them in batches to Firebase Realtime Database.
- Keep BLE reception as the main priority.

Folder structure:
- BikeSens_Gateway.ino
- src/*.h

Required libraries:
- NimBLE-Arduino

Built-in ESP32 libraries used:
- WiFi
- WebServer
- Preferences
- HTTPClient
- WiFiClientSecure
- Update

Configuration:
1) Fill in src/LoginData.h with:
   FIREBASE_API_KEY, FIREBASE_EMAIL, FIREBASE_PASSWORD, FIREBASE_DB_URL

2) Gateway ID is configured from the AP setup page and stored in Preferences.
   Default value: gw01

3) Current firmware version is defined in:
   src/Config.h -> FW_VERSION

BLE payload format:
- Parser in BleScanner.h assumes:
  [company_id LE(2)][prefix0][prefix1][device_id LE(2)][seq LE(2)][pulses LE(2)][bat(1)]

Data flow:
1) Sensors advertise the same measurement packet multiple times.
2) Gateway accepts only unique (device_id, seq) pairs.
3) Records are stored in RAM buffer.
4) When enough records are collected, or after inactivity timeout, gateway sends one batch to Firebase.
5) After successful send, gateway updates status in Firebase.

Firebase structure:
- /gateways/<gateway_id>/batches/<pushId>
- /gateways/<gateway_id>/status
- /gateways/<gateway_id>/control/update
- /gateways/<gateway_id>/control/reboot
- /gateways/<gateway_id>/control/maintenance
- /gateways/<gateway_id>/devices
- /gateways/<gateway_id>/meta

Batch payload:
- gateway_id
- gateway_ts
- count
- items: [{rx_ts, device_id, seq, pulses, bat}]

Status payload:
- last_seen
- buffer_size
- fw_version
- ip
- wifi_rssi
- last_batch_count
- last_batch_ts
- last_ota_ts
- last_ota_version
- last_ota_result
- last_wifi_reconnect_ts
- last_wifi_reconnect_result
- reset_reason
- last_error
- uptime_s
- free_heap
- status_sync_ok_count
- status_sync_fail_count

AP mode:
- If Wi-Fi is not configured or connection at boot fails, gateway starts AP + web config page.
- AP page allows setting:
  - gateway_id
  - Wi-Fi SSID
  - Wi-Fi password
- If AP stays active longer than AP_TIMEOUT_MS, gateway restarts.

Overflow policy:
- Buffer is stored in RAM only.
- When BUFFER_MAX is hit, gateway removes BATCH_SIZE records with the smallest pulses.

OTA update:
- OTA is controlled from Firebase:
  /gateways/<gateway_id>/control/update
- Expected fields:
  - pending
  - version
  - url
  - sha256
  - force
- OTA starts only when:
  - pending == true
  - buffer is empty
  - version != FW_VERSION
- URL must be a full http:// or https:// link.
- For GitHub Releases use the asset:
  BikeSens_Gateway.ino.bin
- After successful OTA:
  - gateway stores OTA result in status
  - gateway clears control/update/pending
  - gateway restarts

Remote reboot:
- Controlled from:
  /gateways/<gateway_id>/control/reboot
- When reboot == true:
  - gateway clears reboot flag
  - gateway restarts immediately

Maintenance:
- Loaded once at boot from:
  /gateways/<gateway_id>/control/maintenance
- Expected structure:
  - auto_reboot_enabled: true/false
  - times: { "1": "00:01", "2": "08:46" }
- Scheduled reboot is executed locally by gateway when current local time matches one of the configured slots.
- If periodic Firebase maintenance requests start failing, gateway first tries runtime recovery:
  - reset Firebase session
  - reset Wi-Fi stack
  - perform fresh Firebase login
- During such failures, maintenance retries are temporarily accelerated instead of waiting for the normal 5-minute heartbeat period.
- If repeated recovery attempts still cannot restore Firebase login, gateway performs a controlled restart.

Devices filter:
- Controlled from:
  /gateways/<gateway_id>/devices
- Expected structure:
  - mode: "all" or "selected"
  - allowed: { "<device_id>": true, ... }
- mode = all:
  - gateway accepts all valid BikeSens advertisements
- mode = selected:
  - gateway accepts only device_id values present in allowed
- Devices list is loaded at boot and checked with a lightweight binary search.

Notes:
- Current implementation uses FIREBASE_TLS_INSECURE = 1.
- build/ and firmware binaries should not be committed to git.
