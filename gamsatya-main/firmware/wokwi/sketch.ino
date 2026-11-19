/*
  GramSatya — hardware simulation for Wokwi (ESP32)
  =================================================
  This is the same node as firmware/esp32_node/esp32_node.ino, adapted so it runs
  in Wokwi's browser simulator: Wokwi provides the virtual Wi-Fi network
  "Wokwi-GUEST" with full internet access, so the signed reading really travels
  to the GramSatya server and shows up on the Devices page.

  What the parts do in the simulation
    HC-SR04 ultrasonic .... tank / bin level. Click the sensor in the diagram and
                            drag the distance slider: the level (and the app) change.
    Potentiometer ......... handpump flow 0-20 L/min, or streetlight load 0-24 W.
                            Turn it to 0 to show a dead pump / dark lamp.
    Green button .......... send a reading immediately (waiting 10 s is boring on stage).
    Green LED ............. short flash = server accepted the signature;
                            three fast blinks = the server rejected it.

  Only the first line of the config block must be edited: paste the public URL of
  your running GramSatya app (see firmware/wokwi/README.md for the one command
  that prints it).

  Wiring (identical GPIOs to the real firmware)
    TRIG GPIO 5, ECHO GPIO 18, flow/light analog GPIO 34, flow pulse GPIO 27, status LED GPIO 25
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "mbedtls/md.h"

#include <stdint.h>

// ======================= CONFIGURE =======================
// Paste the public URL of your app, ending in /api/ingest.
// localhost will NOT work: the simulator runs in the cloud, not on your laptop.
const char* SERVER_URL    = "https://YOUR-TUNNEL-URL/api/ingest";

const char* DEVICE_SECRET = "gramsatya-demo-secret";  // must equal DEVICE_SECRET on the server
const char* DEVICE_ID     = "wokwi-node-01";
const char* SENSOR_ID     = "radhaganj-handpump-1";   // any id from the Devices page

// Pick the sensor this node is pretending to be:
//   KIND_TANK          -> haripur-char-tank-1        (HC-SR04, % full)
//   KIND_HANDPUMP      -> radhaganj-handpump-1       (pot, L/min)
//   KIND_STREETLIGHT   -> radhaganj-streetlight-2    (pot, watts)
//   KIND_BIN           -> radhaganj-bin-1            (HC-SR04, % full)
#define KIND_TANK        1
#define KIND_HANDPUMP    2
#define KIND_STREETLIGHT 3
#define KIND_BIN         4
const int SENSOR_KIND = KIND_HANDPUMP;

const unsigned long SEND_EVERY_MS = 10000;
const float CONTAINER_DEPTH_CM = 150.0;  // sensor to the bottom of the tank or bin
const bool  FORGE_READING = false;       // true = send a broken signature (demo the rejection)
// =========================================================

const char* WIFI_SSID = "Wokwi-GUEST";   // Wokwi's virtual network, open, no password
const char* WIFI_PASS = "";

const int TRIG_PIN = 5, ECHO_PIN = 18, POT_PIN = 34, BTN_PIN = 27, LED_PIN = 25;

unsigned long lastSend = 0;
bool buttonWasUp = true;
unsigned long buttonAt = 0;

// ---------------------------------------------------------------- helpers

String hmacSha256(const char* key, const String& msg) {
  unsigned char out[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key, strlen(key));
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)msg.c_str(), msg.length());
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", out[i]);
  hex[64] = 0;
  return String(hex);
}

/** Same rounding as the server (value.toFixed(2)), so the signature matches exactly. */
float round2(float v) { return roundf(v * 100.0f) / 100.0f; }

/** UTC epoch. Wokwi reaches NTP; if that is blocked we fall back to a synthetic
 *  clock so the demo still works (the server only needs the timestamp to match
 *  the signature, not to be wall-clock accurate). */
long epochNow() {
  time_t t = time(nullptr);
  if (t > 1700000000L) return (long)t;
  return 1700000000L + (long)(millis() / 1000UL);
}

float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(20);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long us = pulseIn(ECHO_PIN, HIGH, 30000);
  if (us == 0) return -1;
  return us * 0.0343 / 2.0;
}

float levelPercent() {
  float d = readDistanceCm();
  if (d < 0) return 0;                                   // no echo: report empty
  return constrain((CONTAINER_DEPTH_CM - d) / CONTAINER_DEPTH_CM * 100.0, 0, 100);
}

float readValue() {
  switch (SENSOR_KIND) {
    case KIND_TANK:
    case KIND_BIN:
      return levelPercent();
    case KIND_HANDPUMP:
      return analogRead(POT_PIN) / 4095.0 * 20.0;        // 0-20 L/min
    case KIND_STREETLIGHT:
      return analogRead(POT_PIN) / 4095.0 * 24.0;        // 0-24 W
  }
  return 0;
}

void blink(int times, int onMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH); delay(onMs);
    digitalWrite(LED_PIN, LOW);  delay(onMs);
  }
}

// The client must outlive http.POST(), so both clients live at function scope.
WiFiClient plainClient;
WiFiClientSecure secureClient;

bool sendHttp(const String& body) {
  HTTPClient http;
  String url = String(SERVER_URL);
  http.setTimeout(20000);
  http.setConnectTimeout(20000);

  bool ok;
  if (url.startsWith("https")) {
    secureClient.setInsecure();  // prototype only: pin the certificate in production
    ok = http.begin(secureClient, url);
  } else {
    ok = http.begin(plainClient, url);
  }
  if (!ok) {
    Serial.println("Could not create the HTTP connection");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Bypass-Tunnel-Reminder", "true");  // keeps free tunnels from showing their HTML reminder
  int code = http.POST(body);
  String reply = http.getString();
  http.end();

  Serial.printf("POST %d  %s\n", code, reply.c_str());
  return code == 200;
}

/** Signs and sends one reading. Returns true when the server verified the signature. */
bool sendReading(bool forge = FORGE_READING) {
  float value = round2(readValue());
  long ts = epochNow();
  String v = String(value, 2);                          // "12.30"
  String msg = String(DEVICE_ID) + "|" + SENSOR_ID + "|" + v + "|" + String(ts);
  String sig;
  if (forge) {
    for (int i = 0; i < 64; i++) sig += "0";            // well-formed but wrong signature
  } else {
    sig = hmacSha256(DEVICE_SECRET, msg);
  }

  String body = String("{\"deviceId\":\"") + DEVICE_ID +
                "\",\"sensorId\":\"" + SENSOR_ID +
                "\",\"value\":" + v +
                ",\"ts\":" + String(ts) +
                ",\"sig\":\"" + sig + "\"}";

  Serial.printf("-> %s %s = %s\n", DEVICE_ID, SENSOR_ID, v.c_str());
  bool verified = WiFi.status() == WL_CONNECTED && sendHttp(body);

  if (verified) blink(1, 250);
  else          blink(3, 80);
  return verified;
}

// ---------------------------------------------------------------- lifecycle

void connectWifi() {
  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    delay(250);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) Serial.printf("\nConnected, IP %s\n", WiFi.localIP().toString().c_str());
  else Serial.println("\nWi-Fi failed, will keep retrying in the loop");
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(POT_PIN, INPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);

  connectWifi();
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  Serial.println("GramSatya Wokwi node ready");
  Serial.println("Edit SERVER_URL at the top of the sketch if this prints 000 / could not connect.");
}

void loop() {
  // Button: send one reading now (edge-detected, 1 s lockout)
  bool up = digitalRead(BTN_PIN) == HIGH;
  if (up && !buttonWasUp && millis() - buttonAt > 1000) {
    buttonAt = millis();
    if (WiFi.status() != WL_CONNECTED) connectWifi();
    sendReading();
  }
  buttonWasUp = up;

  if (millis() - lastSend >= SEND_EVERY_MS) {
    lastSend = millis();
    if (WiFi.status() != WL_CONNECTED) connectWifi();
    sendReading();
  }
  delay(10);
}
