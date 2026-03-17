BikeSens Gateway (Arduino IDE, ESP32)

Folder structure:
- BikeSens_Gateway.ino
- src/*.h

Required libraries:
- NimBLE-Arduino (for BLE scanning)
Built-in (ESP32 core):
- WiFi
- WebServer
- Preferences
- HTTPClient
- WiFiClientSecure

IMPORTANT:
1) Fill in Config.h:
   FIREBASE_API_KEY, FIREBASE_EMAIL, FIREBASE_PASSWORD

2) Gateway ID is configured from the AP setup page and stored in Preferences.
   Default value is: gw01

3) BLE telegram parser in BleScanner.h assumes layout:
   [company_id LE(2)][prefix0][prefix1][device_id LE(2)][seq LE(2)][pulses LE(2)][bat(1)]
   If your payload differs, tell me the exact bytes and we adjust only the parser.

Firebase path:
  /gateways/<GATEWAY_ID>/batches/<pushId>
Fields:
  gateway_id, gateway_ts, count, items:[{rx_ts,device_id,seq,pulses,bat}]

AP mode:
- If Wi-Fi not configured or initial connect fails: starts AP + web config.
- BLE scanning continues in AP mode, data buffered.

Overflow:
- When BUFFER_MAX is hit, gateway removes BATCH_SIZE records with smallest pulses (first encountered).
