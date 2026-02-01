#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <ESP32Servo.h>
#include <Update.h>

#define FW_VERSION "1.1.0"

Servo blindServo;

#define SERVO_PIN 18
#define SERVO_STOP 90
#define SERVO_OPEN 120
#define SERVO_CLOSE 60

#define BRAKE_TIME 70
#define OPEN_TIME 4200
#define CLOSE_TIME 3800

#define LED_PIN 2
#define RESET_BTN 0
#define RESET_TIME 5000

WebServer server(80);
Preferences prefs;

String wifi_ssid;
String wifi_pass;
String api_key;

unsigned long resetPressedAt = 0;
bool resetTriggered = false;

enum BlindState {
  STATE_OPEN,
  STATE_CLOSED,
  STATE_OPENING,
  STATE_CLOSING,
  STATE_STOPPED
};

BlindState blindState = STATE_STOPPED;
int currentPercent = 0;

enum LedMode {
  LED_OFF,
  LED_ON,
  LED_FAST_BLINK,
  LED_SLOW_BLINK
};

LedMode ledMode = LED_OFF;
unsigned long ledTimer = 0;
bool ledState = false;

void setLedMode(LedMode mode) {
  ledMode = mode;
  ledTimer = millis();
  ledState = false;
  digitalWrite(LED_PIN, LOW);
}

void handleLed() {
  unsigned long now = millis();

  if (ledMode == LED_OFF) {
    digitalWrite(LED_PIN, LOW);
    return;
  }

  if (ledMode == LED_ON) {
    digitalWrite(LED_PIN, HIGH);
    return;
  }

  unsigned long interval =
    (ledMode == LED_FAST_BLINK) ? 150 : 1000;

  if (now - ledTimer >= interval) {
    ledTimer = now;
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState);
  }
}

String stateToString() {
  switch (blindState) {
    case STATE_OPEN: return "open";
    case STATE_CLOSED: return "closed";
    case STATE_OPENING: return "opening";
    case STATE_CLOSING: return "closing";
    case STATE_STOPPED: return "stopped";
  }
  return "unknown";
}

void smartStop(int lastDir) {
  blindServo.write(lastDir == SERVO_OPEN ? SERVO_CLOSE : SERVO_OPEN);
  delay(BRAKE_TIME);
  blindServo.write(SERVO_STOP);
  delay(120);
  blindServo.detach();
}

void startConfigPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP-SmartBlind-Setup");
  setLedMode(LED_FAST_BLINK);

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html",
      "<h2>ESP Smart Blind setup</h2>"
      "<form method='POST' action='/save'>"
      "WiFi SSID:<br><input name='ssid'><br>"
      "WiFi Password:<br><input name='pass' type='password'><br>"
      "API Key:<br><input name='key'><br><br>"
      "<button type='submit'>Save</button>"
      "</form>"
    );
  });

  server.on("/save", HTTP_POST, []() {
    prefs.putString("ssid", server.arg("ssid"));
    prefs.putString("pass", server.arg("pass"));
    prefs.putString("key", server.arg("key"));
    server.send(200, "text/html", "<h3>Saved. Rebooting...</h3>");
    delay(1500);
    ESP.restart();
  });

  server.begin();
}

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
  setLedMode(LED_SLOW_BLINK);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
  }

  if (WiFi.status() == WL_CONNECTED) {
    MDNS.begin("smartblind");
    setLedMode(LED_OFF);
  } else {
    startConfigPortal();
  }
}

bool checkKey() {
  return server.hasArg("key") && server.arg("key") == api_key;
}

bool isLocalClient() {
  IPAddress ip = server.client().remoteIP();
  if (ip[0] == 192 && ip[1] == 168) return true;
  if (ip[0] == 10) return true;
  if (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) return true;
  return false;
}

void setupOTA() {
  server.on("/update", HTTP_GET, []() {
    if (!isLocalClient() || !checkKey()) {
      server.send(403, "text/plain", "Forbidden");
      return;
    }

    server.send(200, "text/html",
      "<h2>ESP Smart Blind OTA</h2>"
      "<p>Firmware: " FW_VERSION "</p>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='firmware'>"
      "<input type='submit' value='Update'>"
      "</form>"
    );
  });
  server.on("/update", HTTP_POST,
    []() {
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
      delay(1000);
      ESP.restart();
    },
    []() {
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
      else if (upload.status == UPLOAD_FILE_WRITE) Update.write(upload.buf, upload.currentSize);
      else if (upload.status == UPLOAD_FILE_END) Update.end(true);
    }
  );
}

void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  pinMode(RESET_BTN, INPUT_PULLUP);

  blindServo.attach(SERVO_PIN);
  blindServo.write(SERVO_STOP);
  blindServo.detach();

  prefs.begin("config", false);

  wifi_ssid = prefs.getString("ssid", "");
  wifi_pass = prefs.getString("pass", "");
  api_key = prefs.getString("key", "");

  blindState = (BlindState)prefs.getInt("state", STATE_STOPPED);

  if (wifi_ssid == "") startConfigPortal();
  else connectToWiFi();

  server.on("/", []() {
    String page;
    page += "<h2>ESP Smart Blind</h2>";
    page += "<p>Firmware: " FW_VERSION "</p>";
    page += "<p>State: " + stateToString() + "</p>";
    page += "<p>IP: " + WiFi.localIP().toString() + "</p>";
    server.send(200, "text/html", page);
  });

  server.on("/open", []() {
    if (!checkKey()) { server.send(403, "Forbidden"); return; }

    blindState = STATE_OPENING;
    prefs.putInt("state", blindState);
    setLedMode(LED_ON);

    blindServo.attach(SERVO_PIN);
    blindServo.write(SERVO_OPEN);
    delay(OPEN_TIME);
    smartStop(SERVO_OPEN);

    blindState = STATE_OPEN;
    prefs.putInt("state", blindState);
    setLedMode(LED_OFF);

    server.send(200, "OPEN OK");
  });

  server.on("/close", []() {
    if (!checkKey()) { server.send(403, "Forbidden"); return; }

    blindState = STATE_CLOSING;
    prefs.putInt("state", blindState);
    setLedMode(LED_ON);

    blindServo.attach(SERVO_PIN);
    blindServo.write(SERVO_CLOSE);
    delay(CLOSE_TIME);
    smartStop(SERVO_CLOSE);

    blindState = STATE_CLOSED;
    prefs.putInt("state", blindState);
    setLedMode(LED_OFF);

    server.send(200, "CLOSE OK");
  });

  server.on("/stop", []() {
    if (!checkKey()) { server.send(403, "Forbidden"); return; }

    blindServo.attach(SERVO_PIN);
    blindServo.write(SERVO_STOP);
    delay(100);
    blindServo.detach();

    blindState = STATE_STOPPED;
    prefs.putInt("state", blindState);
    setLedMode(LED_OFF);

    server.send(200, "STOPPED");
  });

  setupOTA();
  server.begin();
}

void loop() {
  server.handleClient();
  handleLed();

  if (digitalRead(RESET_BTN) == LOW) {
    if (resetPressedAt == 0) resetPressedAt = millis();

    if (!resetTriggered && millis() - resetPressedAt > RESET_TIME) {
      resetTriggered = true;
      setLedMode(LED_SLOW_BLINK);
      prefs.clear();
      delay(800);
      ESP.restart();
    }
  } else {
    resetPressedAt = 0;
    resetTriggered = false;
  }
}
