/*
  ESP32-CAM Button Camera -> Telegram
  Press a pushbutton (IO13 to GND) to capture a photo and send it to a Telegram bot.

  Author: Farah AlHayek
  Camera support files (app_httpd.cpp, camera_index.h, camera_pins.h,
  board_config.h, partitions.csv) and the RHYX M21-45 camera settings come from:
    https://github.com/YigitMusstafa/rhyx-m21-45-esp32-cam-web-server-code
  which is based on Espressif's CameraWebServer example (Apache License 2.0).
  See README.md for setup.
*/

#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "img_converters.h"   // frame2jpg(): RGB565 -> JPEG

#define CAMERA_MODEL_RHYX_M21
#include "camera_pins.h"

// ---------- Settings ----------
const char* ssid     = "YOUR_WIFI_NAME";
const char* password = "YOUR_WIFI_PASSWORD";

const String BOT_TOKEN = "YOUR_BOT_TOKEN";
const String CHAT_ID   = "YOUR_CHAT_ID";

#define BUTTON_PIN 13          // button between IO13 and GND
#define JPEG_QUALITY 80        // 0-100 (higher = better quality, bigger file)
// ------------------------------

void startCameraServer();      // from app_httpd.cpp (web page keeps working)

// Button debounce state
int lastReading = HIGH;
int stableState = HIGH;
unsigned long lastChangeTime = 0;
const unsigned long DEBOUNCE_MS = 50;

// ---------- Send JPEG to Telegram (sendPhoto, multipart/form-data) ----------
bool sendPhotoToTelegram(const uint8_t* jpg, size_t len) {
  WiFiClientSecure client;
  client.setInsecure();  // skip certificate check (simple; fine for this project)

  Serial.println("Connecting to api.telegram.org ...");
  if (!client.connect("api.telegram.org", 443)) {
    Serial.println("Connection to Telegram failed");
    return false;
  }

  String boundary = "ESP32CAMBoundary";
  String head = String("--") + boundary + "\r\n" +
                "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" +
                CHAT_ID + "\r\n" +
                "--" + boundary + "\r\n" +
                "Content-Disposition: form-data; name=\"photo\"; filename=\"esp32cam.jpg\"\r\n" +
                "Content-Type: image/jpeg\r\n\r\n";
  String tail = String("\r\n--") + boundary + "--\r\n";

  size_t totalLen = head.length() + len + tail.length();

  // HTTP request header
  client.print(String("POST /bot") + BOT_TOKEN + "/sendPhoto HTTP/1.1\r\n");
  client.print("Host: api.telegram.org\r\n");
  client.print(String("Content-Type: multipart/form-data; boundary=") + boundary + "\r\n");
  client.print(String("Content-Length: ") + totalLen + "\r\n");
  client.print("Connection: close\r\n\r\n");

  // Body
  client.print(head);
  const size_t CHUNK = 1024;
  for (size_t i = 0; i < len; i += CHUNK) {
    size_t n = (len - i < CHUNK) ? (len - i) : CHUNK;
    client.write(jpg + i, n);
  }
  client.print(tail);

  // Read the response (max 10 s)
  String response;
  unsigned long start = millis();
  while (millis() - start < 10000) {
    while (client.available()) {
      response += (char)client.read();
    }
    if (!client.connected() && !client.available()) break;
    delay(10);
  }
  client.stop();

  bool ok = response.indexOf("\"ok\":true") >= 0;
  if (ok) {
    Serial.println("Photo sent to Telegram!");
  } else {
    Serial.println("Telegram error. Response:");
    Serial.println(response);
  }
  return ok;
}

// ---------- Capture a frame, convert to JPEG, send ----------
void captureAndSend() {
  // Throw away one frame so we send a fresh one, not an old buffered frame
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);

  fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    return;
  }

  // Sensor gives RGB565 (no hardware JPEG) -> convert to JPEG for Telegram
  uint8_t* jpgBuf = nullptr;
  size_t jpgLen = 0;
  bool converted = frame2jpg(fb, JPEG_QUALITY, &jpgBuf, &jpgLen);
  esp_camera_fb_return(fb);

  if (!converted) {
    Serial.println("JPEG conversion failed");
    return;
  }
  Serial.printf("JPEG ready: %u bytes\n", (unsigned)jpgLen);

  sendPhotoToTelegram(jpgBuf, jpgLen);
  free(jpgBuf);
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  // Same working settings as before — do not change
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_RGB565;   // sensor has no JPEG support
  config.frame_size = FRAMESIZE_QVGA;       // 320x240
  config.jpeg_quality = 10;
  config.fb_count = 2;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return;
  }

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  startCameraServer();

  Serial.print("Camera Ready! Open: http://");
  Serial.println(WiFi.localIP());
  Serial.println("Press the button to send a photo to Telegram.");
}

void loop() {
  // Reconnect Wi-Fi if the hotspot dropped
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    WiFi.reconnect();
    delay(2000);
    return;
  }

  // Debounced button: act once per press (HIGH -> LOW)
  int reading = digitalRead(BUTTON_PIN);
  if (reading != lastReading) {
    lastChangeTime = millis();
    lastReading = reading;
  }
  if ((millis() - lastChangeTime) > DEBOUNCE_MS && reading != stableState) {
    stableState = reading;
    if (stableState == LOW) {
      Serial.println("Button pressed -> capturing...");
      captureAndSend();
    }
  }

  delay(5);
}
