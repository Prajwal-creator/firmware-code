#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <TinyGPSPlus.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <MPU6050.h>
#include <math.h>

// =====================================================
// WIFI / DJANGO
// =====================================================

const char* WIFI_SSID = "Prajwal";
const char* WIFI_PASSWORD = "1234567890";

const char* SERVER_IP = "10.76.140.207";
const uint16_t SERVER_PORT = 8000;

const char* DEVICE_ID = "Esp32-cam-222";
const char* DEVICE_TOKEN = "8NxT0fF6KNCiBbqkElkKfugRnbuoYFin3NjRDADzjLo";

// =====================================================
// PIN CONFIGURATION
// =====================================================

#define MPU_SDA 27
#define MPU_SCL 21

#define OLED_SDA 22
#define OLED_SCL 4

#define GPS_RX 15

#define CAM_UART_RX 16
#define CAM_UART_TX 17

#define BUTTON1 2
#define BUTTON2 18
#define BUTTON3 12

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

//user added that variables for ui and ux

// ========== ROADGUARD AI OLED UI ==========

enum OLEDPage {
  PAGE_HOME = 0,
  PAGE_GPS,
  PAGE_SENSOR,
  PAGE_DEVICE,
  PAGE_ALERT
};

OLEDPage currentPage = PAGE_HOME;

bool wsConnected = false;
bool mpuConnected = false;
bool cameraConnected = false;

bool potholeAlert = false;
float lastJerkDelta = 0.0f;
unsigned long alertStarted = 0;

unsigned long bootStarted = 0;
unsigned long lastButtonPress = 0;

const unsigned long BUTTON_DEBOUNCE_MS = 180;
const unsigned long ALERT_DISPLAY_MS = 5000;

// =====================================================
// I2C BUSES
// =====================================================
TwoWire OLEDWire(1);
// =====================================================
// DEVICES
// =====================================================

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &OLEDWire,
  -1
);

// Electronic Cats MPU6050 library
MPU6050 mpu;

HardwareSerial GPSSerial(1);
HardwareSerial CamSerial(2);

WebSocketsClient webSocket;
TinyGPSPlus gps;

// =====================================================
// TIMERS
// =====================================================

unsigned long lastTelemetry = 0;
unsigned long lastHealth = 0;
unsigned long lastDisplay = 0;
unsigned long lastGpsByte = 0;
unsigned long lastPothole = 0;

const unsigned long TELEMETRY_MS = 1000;
const unsigned long HEALTH_MS = 5000;
const unsigned long DISPLAY_MS = 500;
const unsigned long COOLDOWN_MS = 2500;

const float JERK_THRESHOLD = 5.0f;

// =====================================================
// STATUS
// =====================================================

bool oledOK = false;
bool hasGPSFix = false;

// =====================================================
// SENSOR VALUES
// =====================================================

float ax = 0;
float ay = 0;
float az = 0;

float speedKmph = 0.0f;
float accuracy = 0.0f;

int satellites = 0;

float latitude = 0.0f;
float longitude = 0.0f;

//ui animated first intro

void showBootAnimation() {
  if (!oledOK) return;

  for (int frame = 0; frame <= 100; frame += 5) {
    display.clearDisplay();

    // Animated road icon
    display.drawTriangle(
      54, 5, 74, 5, 64, 22,
      SSD1306_WHITE
    );

    display.fillRect(62, 8, 4, 3, SSD1306_BLACK);
    display.fillRect(62, 14, 4, 3, SSD1306_BLACK);

    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(25, 27);
    display.println("ROADGUARD AI");

    display.setCursor(25, 38);
    display.println("ROAD MONITOR");

    display.drawRect(8, 51, 112, 8, SSD1306_WHITE);

    int barWidth = (frame * 108) / 100;
    display.fillRect(
      10, 53, barWidth, 4,
      SSD1306_WHITE
    );

    display.display();
    delay(35);
  }
}

//monipolic display home

void drawHeader(const char* title) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(title);
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);
}

void drawHomeScreen() {
  drawHeader("ROADGUARD AI");

  display.setCursor(0, 13);
  display.print("ID:");
  display.println(DEVICE_ID);

  display.setCursor(0, 23);
  display.print("WiFi:");
  display.print(
    WiFi.status() == WL_CONNECTED ? "OK" : "OFF"
  );

  display.print(" WS:");
  display.println(wsConnected ? "LIVE" : "WAIT");

  display.setCursor(0, 33);
  display.print("CAM:");
  display.print(cameraConnected ? "OK" : "WAIT");

  display.print(" GPS:");
  display.println(hasGPSFix ? "FIX" : "WAIT");

  display.setCursor(0, 43);
  display.print("MPU:");
  display.print(mpuConnected ? "OK" : "ERROR");

  display.setCursor(0, 54);
  display.print("D2>NEXT D18<BACK");
}

void drawGPSScreen() {
  drawHeader("GPS INFORMATION");

  display.setCursor(0, 14);
  display.print("Fix: ");
  display.println(hasGPSFix ? "VALID" : "SEARCHING");

  display.setCursor(0, 25);
  display.print("Lat:");
  if (hasGPSFix) display.println(latitude, 5);
  else display.println("--");

  display.setCursor(0, 36);
  display.print("Lon:");
  if (hasGPSFix) display.println(longitude, 5);
  else display.println("--");

  display.setCursor(0, 47);
  display.print("Speed:");
  display.print(speedKmph, 1);
  display.println(" km/h");

  display.setCursor(0, 57);
  display.print("Satellites:");
  display.print(satellites);
}

void drawSensorScreen() {
  drawHeader("MPU6050 SENSOR");

  display.setCursor(0, 15);
  display.print("AX: ");
  display.print(ax, 2);
  display.println(" m/s2");

  display.setCursor(0, 27);
  display.print("AY: ");
  display.print(ay, 2);
  display.println(" m/s2");

  display.setCursor(0, 39);
  display.print("AZ: ");
  display.print(az, 2);
  display.println(" m/s2");

  display.setCursor(0, 53);
  display.println(
    mpuConnected ? "Sensor: CONNECTED" : "Sensor: ERROR"
  );
}

void drawDeviceScreen() {
  drawHeader("DEVICE STATUS");

  display.setCursor(0, 14);
  display.print("ID:");
  display.println(DEVICE_ID);

  display.setCursor(0, 25);
  display.print("WiFi RSSI:");
  if (WiFi.status() == WL_CONNECTED) {
    display.print(WiFi.RSSI());
    display.println(" dBm");
  } else {
    display.println("OFF");
  }

  display.setCursor(0, 36);
  display.print("WebSocket:");
  display.println(wsConnected ? "LIVE" : "OFF");

  display.setCursor(0, 47);
  display.print("Camera:");
  display.println(cameraConnected ? "ONLINE" : "WAIT");

  display.setCursor(0, 57);
  display.print("GPS:");
  display.print(hasGPSFix ? "FIX" : "NO FIX");
}

//alert screen when pothole dectations

void drawAlertScreen() {
  bool invert = ((millis() / 250) % 2) == 0;

  display.fillScreen(
    invert ? SSD1306_WHITE : SSD1306_BLACK
  );

  display.setTextColor(
    invert ? SSD1306_BLACK : SSD1306_WHITE
  );

  display.setTextSize(1);
  display.setCursor(15, 2);
  display.println("! POTHOLE ALERT !");

  display.drawLine(0, 12, 127, 12,
                   invert ? SSD1306_BLACK : SSD1306_WHITE);

  display.setCursor(0, 17);
  display.print("Change:");
  display.print(lastJerkDelta, 1);
  display.println(" m/s2");

  display.setCursor(0, 29);
  display.print("GPS:");
  display.println(hasGPSFix ? "AVAILABLE" : "NO FIX");

  display.setCursor(0, 40);
  display.print("Camera:");
  display.println(cameraConnected ? "ONLINE" : "WAIT");

  display.setCursor(0, 54);
  display.println("D12: ACKNOWLEDGE");
}

// =====================================================
// WEBSOCKET EVENTS
// =====================================================


void webSocketEvent(
  WStype_t type,
  uint8_t* payload,
  size_t length
) {
  switch (type) {
    case WStype_CONNECTED:
      wsConnected = true;
      Serial.println("WebSocket connected");
      break;

    case WStype_DISCONNECTED:
      wsConnected = false;
      Serial.println("WebSocket disconnected");
      break;

    case WStype_ERROR:
      wsConnected = false;
      Serial.println("WebSocket error");
      break;

    case WStype_TEXT:
      Serial.print("Server: ");
      Serial.write(payload, length);
      Serial.println();
      break;

    default:
      break;
  }
}

// =====================================================
// GPS
// =====================================================

void readGPS() {
  while (GPSSerial.available()) {
    gps.encode(GPSSerial.read());
    lastGpsByte = millis();
  }

  hasGPSFix =
    gps.location.isValid() &&
    gps.location.age() < 3000;

  if (hasGPSFix) {
    latitude = gps.location.lat();
    longitude = gps.location.lng();
  }

  if (gps.speed.isValid()) {
    speedKmph = gps.speed.kmph();
  }

  if (gps.hdop.isValid()) {
    accuracy = gps.hdop.hdop();
  }

  if (gps.satellites.isValid()) {
    satellites = gps.satellites.value();
  }
}

// =====================================================
// MPU6050
// =====================================================

void readMPU() {

    int16_t rawAx;
    int16_t rawAy;
    int16_t rawAz;

    int16_t rawGx;
    int16_t rawGy;
    int16_t rawGz;


    mpu.getMotion6(
        &rawAx,
        &rawAy,
        &rawAz,
        &rawGx,
        &rawGy,
        &rawGz
    );


    // Default MPU6050 sensitivity:
    // ±2g = 16384 LSB/g

    ax =
        ((float)rawAx / 16384.0) * 9.80665;

    ay =
        ((float)rawAy / 16384.0) * 9.80665;

    az =
        ((float)rawAz / 16384.0) * 9.80665;
}


// =====================================================
// SEND POTHOLE EVENT TO ESP32-CAM THROUGH UART
// =====================================================

void sendPotholeEvent(float magnitudeChange) {
  if (!hasGPSFix) {
    Serial.println(
      "Jerk detected, but GPS fix unavailable."
    );
    return;
  }

  StaticJsonDocument<384> event;

  event["type"] = "POTHOLE";
  event["latitude"] = latitude;
  event["longitude"] = longitude;
  event["speed"] = speedKmph;
  event["accuracy"] = accuracy;
  event["satellites"] = satellites;

  event["ax"] = ax;
  event["ay"] = ay;
  event["az"] = az;

  event["jerk_delta"] = magnitudeChange;

  event["severity"] =
    magnitudeChange >= 8.0f ? "High" : "Medium";

  event["confidence"] = 0.90;

  // One newline-terminated JSON message
  serializeJson(event, CamSerial);
  CamSerial.println();

  Serial.println("POTHOLE EVENT SENT TO CAM:");

  serializeJsonPretty(event, Serial);
  Serial.println();
}

// =====================================================
// SEND TELEMETRY TO DJANGO THROUGH WEBSOCKET
// =====================================================

void sendTelemetry(bool includeHealth) {
  if (!webSocket.isConnected()) {
    return;
  }

  StaticJsonDocument<512> doc;

  JsonObject g = doc.createNestedObject("gps");

  if (hasGPSFix) {
    g["latitude"] = latitude;
    g["longitude"] = longitude;
  }

  g["speed"] = speedKmph;
  g["accuracy"] = accuracy;
  g["satellites"] = satellites;

  JsonObject s = doc.createNestedObject("sensor");

  s["ax"] = ax;
  s["ay"] = ay;
  s["az"] = az;

  if (includeHealth) {
    doc["wifi_rssi"] =
      WiFi.status() == WL_CONNECTED
        ? WiFi.RSSI()
        : -127;

    doc["gps_connected"] =
      lastGpsByte != 0 &&
      millis() - lastGpsByte < 5000;

    doc["mpu6050_connected"] = true;

    // Until CAM status messages are implemented,
    // this remains unknown and is not a real connection test.
    doc["camera_connected"] = false;
  }

  String message;
  serializeJson(doc, message);

  webSocket.sendTXT(message);

  Serial.println("Telemetry: " + message);
}

// =====================================================
// OLED
// =====================================================

void updateOLED() {
  if (!oledOK) return;

  // Automatically return home after alert timeout
  if (potholeAlert &&
      millis() - alertStarted >= ALERT_DISPLAY_MS) {
    potholeAlert = false;
    currentPage = PAGE_HOME;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (potholeAlert || currentPage == PAGE_ALERT) {
    drawAlertScreen();
  } else {
    switch (currentPage) {
      case PAGE_HOME:
        drawHomeScreen();
        break;

      case PAGE_GPS:
        drawGPSScreen();
        break;

      case PAGE_SENSOR:
        drawSensorScreen();
        break;

      case PAGE_DEVICE:
        drawDeviceScreen();
        break;

      default:
        currentPage = PAGE_HOME;
        drawHomeScreen();
        break;
    }
  }

  display.display();
}

// =====================================================
// BUTTONS
// =====================================================


void readButtons() {
  static int lastButton1 = HIGH;
  static int lastButton2 = HIGH;
  static int lastButton3 = HIGH;

  int button1 = digitalRead(BUTTON1);
  int button2 = digitalRead(BUTTON2);
  int button3 = digitalRead(BUTTON3);

  unsigned long now = millis();

  bool pressed1 = lastButton1 == HIGH && button1 == LOW;
  bool pressed2 = lastButton2 == HIGH && button2 == LOW;
  bool pressed3 = lastButton3 == HIGH && button3 == LOW;

  if ((pressed1 || pressed2 || pressed3) &&
      now - lastButtonPress >= BUTTON_DEBOUNCE_MS) {

    lastButtonPress = now;

    if (pressed1) {
      currentPage = (OLEDPage)(
        ((int)currentPage + 1) % 4
      );
    }

    if (pressed2) {
      currentPage = (OLEDPage)(
        ((int)currentPage + 3) % 4
      );
    }

    if (pressed3 && potholeAlert) {
      potholeAlert = false;
      currentPage = PAGE_HOME;
    } else if (pressed3) {
      currentPage = PAGE_HOME;
    }
  }

  lastButton1 = button1;
  lastButton2 = button2;
  lastButton3 = button3;
}
// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("Starting ESP32 WROOM...");

  pinMode(BUTTON1, INPUT_PULLUP);
  pinMode(BUTTON2, INPUT_PULLUP);
  pinMode(BUTTON3, INPUT_PULLUP);

  // ---------------------------------------------------
  // Initialize I2C buses
  // ---------------------------------------------------

  Wire.begin(MPU_SDA,MPU_SCL);
  OLEDWire.begin(OLED_SDA, OLED_SCL);

  bool oledBusOK =  oledOK = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    

  Serial.printf(
    "MPU I2C bus: "
  );

  Serial.printf(
    "OLED I2C bus: %s\n",
    oledBusOK ? "READY" : "FAILED"
  );

  // ---------------------------------------------------
  // Initialize MPU6050
  // ---------------------------------------------------
mpu.initialize();


    if (mpu.testConnection()) {

        Serial.println(
            "✅ MPU6050 connected"
        );

    } else {

        Serial.println(
            "❌ MPU6050 NOT FOUND"
        );
    }

  

  // ---------------------------------------------------
  // Initialize OLED
  // ---------------------------------------------------

  if (oledBusOK) {
    oledOK = display.begin(SSD1306_SWITCHCAPVCC,0x3C);

    if (oledOK) {
        showBootAnimation();
        display.clearDisplay();
        display.display();
    } else {
       Serial.println("OLED initialization failed");
      }
  }

  Serial.println(
    oledOK ? "OLED OK" : "OLED NOT FOUND"
  );

  if (oledOK) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);

    display.println("ESP32 WROOM");
    display.println("Initializing...");

    display.display();
  }

  // ---------------------------------------------------
  // GPS UART
  // ---------------------------------------------------

  GPSSerial.begin(
    9600,
    SERIAL_8N1,
    GPS_RX,
    -1
  );

  // ---------------------------------------------------
  // ESP32-CAM UART
  // ---------------------------------------------------

  CamSerial.begin(
    115200,
    SERIAL_8N1,
    CAM_UART_RX,
    CAM_UART_TX
  );

  // ---------------------------------------------------
  // Wi-Fi
  // ---------------------------------------------------

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting Wi-Fi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  Serial.print("WROOM IP: ");
  Serial.println(WiFi.localIP());

  // ---------------------------------------------------
  // Django WebSocket
  // ---------------------------------------------------

  String path =
    "/ws/devices/" +
    String(DEVICE_ID) +
    "/telemetry/";

  webSocket.begin(
    SERVER_IP,
    SERVER_PORT,
    path
  );

  String auth =
    "Authorization: Token " +
    String(DEVICE_TOKEN);

  webSocket.setExtraHeaders(auth.c_str());
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);

  Serial.println("WROOM ready");
}

// =====================================================
// MAIN LOOP
// =====================================================

void loop() {
  webSocket.loop();

  // Read GPS and motion sensor
  readGPS();
  readMPU();
  readButtons();

  // Forward any CAM UART output to Serial Monitor
  while (CamSerial.available()) {
    Serial.write(CamSerial.read());
  }

  // Refresh OLED
  unsigned long now = millis();

  if (now - lastDisplay >= DISPLAY_MS) {
    lastDisplay = now;
    updateOLED();
  }

  // Send telemetry every second
  if (now - lastTelemetry >= TELEMETRY_MS) {
    lastTelemetry = now;

    bool includeHealth =
      now - lastHealth >= HEALTH_MS;

    if (includeHealth) {
      lastHealth = now;
    }

    sendTelemetry(includeHealth);
  }

  // ---------------------------------------------------
  // Detect sudden acceleration-magnitude changes
  // ---------------------------------------------------

  static float previousMagnitude = 9.80665f;

  float magnitude = sqrtf(
    ax * ax +
    ay * ay +
    az * az
  );

  float delta = fabsf(
    magnitude - previousMagnitude
  );

  previousMagnitude = magnitude;

  if (
    delta >= JERK_THRESHOLD &&
    now - lastPothole >= COOLDOWN_MS
  ) {
    lastPothole = now;

    Serial.printf(
      "JERK DETECTED delta=%.2f m/s^2\n",
      delta
    );

    // Activate the RoadGuard AI OLED alert
    potholeAlert = true;
    currentPage = PAGE_ALERT;
    lastJerkDelta = delta;
    alertStarted = millis();

    // Forward the event to ESP32-CAM
    sendPotholeEvent(delta);
  }

  delay(2);
}
