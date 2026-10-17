#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "esp_camera.h"
#include <WebSocketsClient.h>

// ---------- WIFI / DJANGO ----------
const char* WIFI_SSID = "Prajwal";
const char* WIFI_PASSWORD = "1234567890";
const char* SERVER_IP = "10.76.140.207"; // PC LAN IP
const uint16_t SERVER_PORT = 8000;
const char* DEVICE_ID = "Esp32-cam-222";
const char* DEVICE_TOKEN = "8NxT0fF6KNCiBbqkElkKfugRnbuoYFin3NjRDADzjLo";

String API_URL = String("http://") + SERVER_IP + ":" +
                 String(SERVER_PORT) + "/api/potholes/detect/";
// AI-Thinker ESP32-CAM built-in flash LED
#define FLASH_LED_PIN 4

WebSocketsClient captureSocket;

String pendingCaptureId = "";
bool captureRequested = false;
bool captureInProgress = false;

// ---------- AI-THINKER CAMERA PIN MAP ----------
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// UART: CAM RX=GPIO3/U0R, CAM TX=GPIO1/U0T.
// These are also the programming/serial pins.
HardwareSerial WroomSerial(1);
#define WROOM_RX 3
#define WROOM_TX 1

bool cameraOK = false;
unsigned long lastUpload = 0;
const unsigned long MIN_UPLOAD_INTERVAL_MS = 2500;

bool initCamera() {
  camera_config_t config = {};
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

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_QVGA;  // 320 x 240
  config.jpeg_quality = 12;             // ignored for RGB565
  config.fb_count = 1;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  // Use PSRAM when available.
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t* sensor = esp_camera_sensor_get();
  if (sensor) {
    Serial.printf("Camera PID: 0x%04X\n", sensor->id.PID);
  }

  return true;
}

bool uploadFrame(const JsonDocument& event) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi disconnected; cannot upload.");
    return false;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed.");
    return false;
  }

  if (fb->format != PIXFORMAT_RGB565) {
    Serial.println("ERROR: camera frame is not RGB565.");
    esp_camera_fb_return(fb);
    return false;
  }

  // For RGB565, expected payload size is normally width * height * 2.
  size_t expectedBytes = fb->width * fb->height * 2;
  if (fb->len != expectedBytes) {
    Serial.printf("Unexpected RGB565 size: got %u, expected %u\n",
                  (unsigned)fb->len, (unsigned)expectedBytes);
    esp_camera_fb_return(fb);
    return false;
  }

  String boundary = "----ESP32PotholeBoundary7MA4YWxk";
  String part = "--" + boundary + "\r\n";
  part += "Content-Disposition: form-data; name=\"image\"; filename=\"pothole_rgb565.bin\"\r\n";
  part += "Content-Type: application/octet-stream\r\n\r\n";

  String fields = "\r\n";
  fields += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"width\"\r\n\r\n";
  fields += String(fb->width) + "\r\n";
  fields += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"height\"\r\n\r\n";
  fields += String(fb->height) + "\r\n";

  fields += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"latitude\"\r\n\r\n";
  fields += String(event["latitude"] | 0.0, 6) + "\r\n";
  fields += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"longitude\"\r\n\r\n";
  fields += String(event["longitude"] | 0.0, 6) + "\r\n";
  fields += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"speed\"\r\n\r\n";
  fields += String(event["speed"] | 0.0, 2) + "\r\n";
  fields += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"accuracy\"\r\n\r\n";
  fields += String(event["accuracy"] | 0.0, 2) + "\r\n";
  fields += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"satellites\"\r\n\r\n";
  fields += String(event["satellites"] | 0) + "\r\n";
  fields += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"ax\"\r\n\r\n";
  fields += String(event["ax"] | 0.0, 4) + "\r\n";
  fields += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"ay\"\r\n\r\n";
  fields += String(event["ay"] | 0.0, 4) + "\r\n";
  fields += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"az\"\r\n\r\n";
  fields += String(event["az"] | 0.0, 4) + "\r\n";
  fields += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"severity\"\r\n\r\n";
  fields += String((const char*)(event["severity"] | "Medium")) + "\r\n";
  fields += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"confidence\"\r\n\r\n";
  fields += String(event["confidence"] | 0.90, 2) + "\r\n";
  fields += "--" + boundary + "--\r\n";

  size_t totalLen = part.length() + fb->len + fields.length();

  // Build one multipart buffer for HTTPClient::POST.
  // QVGA RGB565 is typically 153,600 bytes plus form fields.
  uint8_t* body = (uint8_t*)ps_malloc(totalLen);
  if (!body) body = (uint8_t*)malloc(totalLen);

  if (!body) {
    Serial.println("Not enough memory for multipart body.");
    esp_camera_fb_return(fb);
    return false;
  }

  size_t offset = 0;
  memcpy(body + offset, part.c_str(), part.length());
  offset += part.length();
  memcpy(body + offset, fb->buf, fb->len);
  offset += fb->len;
  memcpy(body + offset, fields.c_str(), fields.length());

  HTTPClient http;
  http.setTimeout(20000);
  http.begin(API_URL);
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  // These header names must match the authentication code in Django.
  http.addHeader("Device-ID", DEVICE_ID);
  http.addHeader("Device-Token", DEVICE_TOKEN);

  Serial.printf("Uploading RGB565 %ux%u, %u bytes...\n",
                (unsigned)fb->width, (unsigned)fb->height,
                (unsigned)fb->len);

  int status = http.POST(body, totalLen);
  String response = http.getString();

  Serial.printf("Django HTTP status: %d\n", status);
  Serial.println(response);

  http.end();
  free(body);
  esp_camera_fb_return(fb);

  return status >= 200 && status < 300;
}

//this is the code for testing the cam board

bool captureTestFrame() {
  if (!cameraOK) {
    Serial.println("Camera not initialized");
    return false;
  }

  Serial.println("Blinking flash LED...");
  digitalWrite(FLASH_LED_PIN, HIGH);
  delay(1000);
  digitalWrite(FLASH_LED_PIN, LOW);

  // Discard an old buffered frame, if available.
  camera_fb_t* oldFrame = esp_camera_fb_get();
  if (oldFrame) {
    esp_camera_fb_return(oldFrame);
  }

  // Capture a fresh frame after the LED is off.
  camera_fb_t* fb = esp_camera_fb_get();

  if (!fb) {
    Serial.println("Test capture failed");
    return false;
  }

  Serial.printf(
    "Captured test frame: %ux%u, %u bytes, format=%d\n",
    fb->width,
    fb->height,
    (unsigned)fb->len,
    fb->format
  );

  // Upload function will be added in the next step.
  esp_camera_fb_return(fb);
  return true;
}

// this is the code for capturing the frames for testing the cam board

bool uploadTestFrame(const String& captureId) {
  camera_fb_t* fb = esp_camera_fb_get();

  if (!fb) {
    Serial.println("Camera capture failed");
    return false;
  }

  if (fb->format != PIXFORMAT_RGB565) {
    Serial.println("Unexpected camera format");
    esp_camera_fb_return(fb);
    return false;
  }

  String boundary = "----RoadGuardTestCapture";
  String part = "--" + boundary + "\r\n";
  part += "Content-Disposition: form-data; name=\"image\"; "
          "filename=\"test.rgb565\"\r\n";
  part += "Content-Type: application/octet-stream\r\n\r\n";

  String fields = "\r\n";
  fields += "--" + boundary +
            "\r\nContent-Disposition: form-data; name=\"width\"\r\n\r\n";
  fields += String(fb->width) + "\r\n";

  fields += "--" + boundary +
            "\r\nContent-Disposition: form-data; name=\"height\"\r\n\r\n";
  fields += String(fb->height) + "\r\n";

  fields += "--" + boundary + "--\r\n";

  size_t totalLen = part.length() + fb->len + fields.length();

  uint8_t* body = (uint8_t*)ps_malloc(totalLen);
  if (!body) {
    body = (uint8_t*)malloc(totalLen);
  }

  if (!body) {
    Serial.println("Insufficient memory for upload");
    esp_camera_fb_return(fb);
    return false;
  }

  size_t offset = 0;
  memcpy(body + offset, part.c_str(), part.length());
  offset += part.length();

  memcpy(body + offset, fb->buf, fb->len);
  offset += fb->len;

  memcpy(body + offset, fields.c_str(), fields.length());

  String url = String("http://") + SERVER_IP + ":" +
               String(SERVER_PORT) +
               "/api/potholes/capture-test/" +
               captureId + "/upload/";

  HTTPClient http;
  http.setTimeout(20000);
  http.begin(url);
  http.addHeader(
    "Content-Type",
    "multipart/form-data; boundary=" + boundary
  );
  http.addHeader("Device-ID", DEVICE_ID);
  http.addHeader("Device-Token", DEVICE_TOKEN);

  int code = http.POST(body, totalLen);
  String response = http.getString();

  Serial.printf("Test image upload status: %d\n", code);
  Serial.println(response);

  http.end();
  free(body);
  esp_camera_fb_return(fb);

  return code >= 200 && code < 300;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nESP32-CAM pothole uploader starting");

  WroomSerial.begin(115200, SERIAL_8N1, WROOM_RX, WROOM_TX);

  if (psramFound()) {
    Serial.printf("PSRAM found: %u bytes\n", (unsigned)ESP.getPsramSize());
  } else {
    Serial.println("WARNING: PSRAM not found.");
  }

  cameraOK = initCamera();
  Serial.println(cameraOK ? "Camera initialized" : "Camera unavailable");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("CAM IP: ");
  Serial.println(WiFi.localIP());

  Serial.println("Waiting for UART pothole events...");

  // this code is for iniciating the websocket cannection

pinMode(FLASH_LED_PIN, OUTPUT);
digitalWrite(FLASH_LED_PIN, LOW);

String wsPath = "/ws/camera/" + String(DEVICE_ID) + "/";

captureSocket.begin(SERVER_IP, SERVER_PORT, wsPath);
captureSocket.onEvent([](WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    Serial.println("Capture WebSocket connected");
  }

  if (type == WStype_DISCONNECTED) {
    Serial.println("Capture WebSocket disconnected");
  }

  if (type == WStype_TEXT) {
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload, length);

    if (err) {
      Serial.println("Invalid WebSocket JSON");
      return;
    }

    if (strcmp(doc["type"] | "", "capture_request") == 0) {
      const char* id = doc["request_id"] | "";

      if (strlen(id) > 0 && !captureInProgress) {
        pendingCaptureId = id;
        captureRequested = true;
      }
    }
  }
});

captureSocket.setReconnectInterval(5000);
}


void loop() {
captureSocket.loop();

  if (captureRequested && !captureInProgress) {
    captureRequested = false;
    captureInProgress = true;

    String id = pendingCaptureId;
    pendingCaptureId = "";

    // Blink LED for one second, turn it off, then capture/upload.
    digitalWrite(FLASH_LED_PIN, HIGH);
    delay(1000);
    digitalWrite(FLASH_LED_PIN, LOW);

    bool ok = uploadTestFrame(id);

    Serial.println(
      ok ? "Test image uploaded" : "Test image upload failed"
    );

    captureInProgress = false;
  }

  // Keep your existing WROOM UART pothole-event code below.
  if (!WroomSerial.available()) {
    delay(2);
    return;
  }
  
// this code is sending the pothole trigger frame
  String line = WroomSerial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  Serial.println("UART event: " + line);

  StaticJsonDocument<512> event;
  DeserializationError err = deserializeJson(event, line);
  if (err) {
    Serial.print("Invalid UART JSON: ");
    Serial.println(err.c_str());
    return;
  }
  if (strcmp(event["type"] | "", "POTHOLE") != 0) {
    Serial.println("Ignoring non-pothole UART message.");
    return;
  }

  if (!event["latitude"].is<float>() ||
      !event["longitude"].is<float>()) {
    Serial.println("Event has no valid GPS coordinates.");
    return;
  }

  if (!cameraOK) {
    Serial.println("Camera is not initialized; event skipped.");
    return;
  }

  if (millis() - lastUpload < MIN_UPLOAD_INTERVAL_MS) {
    Serial.println("Upload cooldown active; event skipped.");
    return;
  }

  lastUpload = millis();
  bool ok = uploadFrame(event);
  Serial.println(ok ? "Pothole upload succeeded" : "Pothole upload failed");
}

