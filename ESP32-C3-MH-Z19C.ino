/*
   CO₂ Monitor with PWM sensor (MH-Z19C) – Version 2.2
   Features:
   - Web configuration (Wi-Fi, MQTT, NTP, screen schedule, button, CO₂ thresholds, fonts, temperature topic)
   - OLED SSD1306 with animation (bubbles, drifting numbers)
   - Button with two modes: short press (wake for set time) / long press (wake while held)
   - MQTT publishing for CO₂ and temperature (DS18B20)
   - Configurable PIN code for saving settings
   - Responsive web interface with version and build timestamp
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ========== ВЕРСИЯ И ДАТА СБОРКИ ==========
#define FW_VERSION "2.2"
#define BUILD_TIMESTAMP __DATE__ " " __TIME__

// ========== ПИНЫ ==========
#define I2C_SDA   2
#define I2C_SCL   3
#define PWM_PIN   4
#define BUTTON_PIN 5   // кнопка с подтяжкой к GND
#define ONE_WIRE_BUS 6 // пин для DS18B20

// ========== ДИСПЛЕЙ ==========
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1

TwoWire I2Cbus = TwoWire(0);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &I2Cbus, OLED_RESET);

// ========== ШРИФТЫ ==========
#include <Fonts/FreeMonoBold9pt7b.h>
const GFXfont* selectedFont = NULL;
bool useBigFont = false;

// ========== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ==========
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3 * 3600, 60000);

WiFiClient espClient;
PubSubClient mqttClient(espClient);

WebServer server(80);

unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 100;   // обновление экрана 10 раз в секунду (анимация)

unsigned long lastMqttSend = 0;
const unsigned long MQTT_INTERVAL = 60000;   // раз в минуту

unsigned long lastTempRead = 0;
const unsigned long TEMP_READ_INTERVAL = 5000; // читаем температуру каждые 5 секунд

int currentPpm = -1;
float currentTemp = -127.0;   // невалидное значение
bool displayState = true;
bool scheduleEnabled = true;
unsigned long buttonWakeUntil = 0;

// Анимация
struct Bubble {
  int x, y;
  int radius;
  int speed;
  bool active;
};
#define MAX_BUBBLES 10
Bubble bubbles[MAX_BUBBLES];
int numberOffsetX = 0;
int numberOffsetY = 0;

// Для чередования позиции температуры
int tempPosition = 0; // 0 - справа снизу, 1 - справа сверху, 2 - слева сверху
unsigned long lastTempPosChange = 0;
const unsigned long TEMP_POS_INTERVAL = 5000; // менять позицию каждые 5 секунд

// ========== НАСТРОЙКИ ==========
struct Config {
  char ssid[32] = "xopkland";
  char password[64] = "1234567890987654321";
  char mqtt_server[40] = "192.168.1.42";
  int mqtt_port = 1883;
  char mqtt_user[32] = "";
  char mqtt_pass[32] = "";
  char mqtt_topic_co2[64] = "CO2/ppm";
  char mqtt_topic_temp[64] = "temperature";  // новый топик для температуры
  char ntp_server[40] = "pool.ntp.org";
  int timezone = 3;

  char schedule[3][32] = {"0:00-6:00,127", "8:00-16:00,31", ""};

  bool button_enabled = true;
  int button_gpio = BUTTON_PIN;
  int button_wake_time = 30; // секунд

  int thresholds[4] = {800, 1200, 1800, 5000};
  char level_names[4][32] = {"Норма", "Внимание", "Опасно", "Полный абзац"};

  bool use_custom_font = false;
} config;

// ========== ДАТЧИК ТЕМПЕРАТУРЫ ==========
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ========== КНОПКА (опрашиваем в loop) ==========
enum ButtonState { BUTTON_IDLE, BUTTON_PRESSED, BUTTON_HELD };
ButtonState btnState = BUTTON_IDLE;
unsigned long buttonPressTime = 0;
bool buttonLongPressHandled = false;
const unsigned long LONG_PRESS_MS = 1000; // 1 секунда для удержания

// ========== ФУНКЦИИ РАБОТЫ С КОНФИГУРАЦИЕЙ ==========
void loadConfig() {
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
    return;
  }
  if (SPIFFS.exists("/config.json")) {
    File file = SPIFFS.open("/config.json", "r");
    if (file) {
      StaticJsonDocument<2048> doc;
      DeserializationError error = deserializeJson(doc, file);
      if (!error) {
        strlcpy(config.ssid, doc["ssid"] | config.ssid, sizeof(config.ssid));
        strlcpy(config.password, doc["password"] | config.password, sizeof(config.password));
        strlcpy(config.mqtt_server, doc["mqtt_server"] | config.mqtt_server, sizeof(config.mqtt_server));
        config.mqtt_port = doc["mqtt_port"] | config.mqtt_port;
        strlcpy(config.mqtt_user, doc["mqtt_user"] | config.mqtt_user, sizeof(config.mqtt_user));
        strlcpy(config.mqtt_pass, doc["mqtt_pass"] | config.mqtt_pass, sizeof(config.mqtt_pass));
        strlcpy(config.mqtt_topic_co2, doc["mqtt_topic_co2"] | config.mqtt_topic_co2, sizeof(config.mqtt_topic_co2));
        strlcpy(config.mqtt_topic_temp, doc["mqtt_topic_temp"] | config.mqtt_topic_temp, sizeof(config.mqtt_topic_temp));
        strlcpy(config.ntp_server, doc["ntp_server"] | config.ntp_server, sizeof(config.ntp_server));
        config.timezone = doc["timezone"] | config.timezone;
        for (int i = 0; i < 3; i++) {
          strlcpy(config.schedule[i], doc["schedule"][i] | config.schedule[i], 32);
        }
        config.button_enabled = doc["button_enabled"] | config.button_enabled;
        config.button_gpio = doc["button_gpio"] | config.button_gpio;
        config.button_wake_time = doc["button_wake_time"] | config.button_wake_time;
        for (int i = 0; i < 4; i++) {
          config.thresholds[i] = doc["thresholds"][i] | config.thresholds[i];
          strlcpy(config.level_names[i], doc["level_names"][i] | config.level_names[i], 32);
        }
        config.use_custom_font = doc["use_custom_font"] | config.use_custom_font;
      } else {
        Serial.println("Failed to parse config.json");
      }
      file.close();
    }
  } else {
    Serial.println("No config.json, using defaults");
    saveConfig();
  }
  SPIFFS.end();
}

void saveConfig() {
  if (!SPIFFS.begin(true)) return;
  StaticJsonDocument<2048> doc;
  doc["ssid"] = config.ssid;
  doc["password"] = config.password;
  doc["mqtt_server"] = config.mqtt_server;
  doc["mqtt_port"] = config.mqtt_port;
  doc["mqtt_user"] = config.mqtt_user;
  doc["mqtt_pass"] = config.mqtt_pass;
  doc["mqtt_topic_co2"] = config.mqtt_topic_co2;
  doc["mqtt_topic_temp"] = config.mqtt_topic_temp;
  doc["ntp_server"] = config.ntp_server;
  doc["timezone"] = config.timezone;
  JsonArray scheduleArr = doc.createNestedArray("schedule");
  for (int i = 0; i < 3; i++) {
    scheduleArr.add(config.schedule[i]);
  }
  doc["button_enabled"] = config.button_enabled;
  doc["button_gpio"] = config.button_gpio;
  doc["button_wake_time"] = config.button_wake_time;
  JsonArray thArr = doc.createNestedArray("thresholds");
  for (int i = 0; i < 4; i++) thArr.add(config.thresholds[i]);
  JsonArray namesArr = doc.createNestedArray("level_names");
  for (int i = 0; i < 4; i++) namesArr.add(config.level_names[i]);
  doc["use_custom_font"] = config.use_custom_font;

  File file = SPIFFS.open("/config.json", "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
    Serial.println("Config saved");
  }
  SPIFFS.end();
}

// ========== УПРАВЛЕНИЕ ЭКРАНОМ ==========
bool isScheduleOff() {
  if (!scheduleEnabled) return false;

  timeClient.update();
  int hour = timeClient.getHours();
  int minute = timeClient.getMinutes();
  int day = timeClient.getDay(); // 0=вс, 1=пн..6=сб
  int dayMask = (day == 0) ? (1 << 6) : (1 << (day - 1));

  for (int i = 0; i < 3; i++) {
    if (strlen(config.schedule[i]) == 0) continue;
    char *token = strtok(config.schedule[i], ",");
    if (!token) continue;
    char *daysStr = strtok(NULL, ",");
    int mask = daysStr ? atoi(daysStr) : 127;
    if ((mask & dayMask) == 0) continue;

    int startHour, startMin, endHour, endMin;
    if (sscanf(token, "%d:%d-%d:%d", &startHour, &startMin, &endHour, &endMin) == 4) {
      int start = startHour * 60 + startMin;
      int end = endHour * 60 + endMin;
      int now = hour * 60 + minute;
      if (start <= end) {
        if (now >= start && now < end) return true;
      } else {
        if (now >= start || now < end) return true;
      }
    }
  }
  return false;
}

bool shouldDisplayOn() {
  if (buttonWakeUntil > millis()) {
    return true;
  } else {
    scheduleEnabled = true;
  }
  return !isScheduleOff();
}

void setDisplayPower(bool on) {
  if (on && !displayState) {
    display.ssd1306_command(SSD1306_DISPLAYON);
    displayState = true;
    updateDisplayContent();
  } else if (!on && displayState) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    displayState = false;
  }
}

// ========== АНИМАЦИЯ ==========
void initBubbles() {
  for (int i = 0; i < MAX_BUBBLES; i++) {
    bubbles[i].active = random(0, 100) < 80;
    if (bubbles[i].active) {
      bubbles[i].x = random(0, SCREEN_WIDTH);
      bubbles[i].y = random(0, SCREEN_HEIGHT);
      bubbles[i].radius = random(2, 6);
      bubbles[i].speed = random(1, 3);
    }
  }
}

void updateBubbles() {
  for (int i = 0; i < MAX_BUBBLES; i++) {
    if (bubbles[i].active) {
      bubbles[i].y -= bubbles[i].speed;
      if (bubbles[i].y + bubbles[i].radius < 0) {
        bubbles[i].y = SCREEN_HEIGHT + bubbles[i].radius;
        bubbles[i].x = random(0, SCREEN_WIDTH);
        bubbles[i].radius = random(2, 6);
        bubbles[i].speed = random(1, 3);
      }
    } else {
      if (random(0, 100) < 2) {
        bubbles[i].active = true;
        bubbles[i].x = random(0, SCREEN_WIDTH);
        bubbles[i].y = SCREEN_HEIGHT + random(0, 10);
        bubbles[i].radius = random(2, 6);
        bubbles[i].speed = random(1, 3);
      }
    }
  }
}

void drawBubbles() {
  for (int i = 0; i < MAX_BUBBLES; i++) {
    if (bubbles[i].active) {
      display.fillCircle(bubbles[i].x, bubbles[i].y, bubbles[i].radius, SSD1306_WHITE);
    }
  }
}

// ========== ОТРИСОВКА СОДЕРЖИМОГО ==========
void updateDisplayContent() {
  if (!displayState) return;

  display.clearDisplay();

  // Анимация пузырьков
  updateBubbles();
  drawBubbles();

  // Определяем уровень CO2
  int levelIndex = 0;
  for (int i = 0; i < 4; i++) {
    if (currentPpm < config.thresholds[i]) {
      levelIndex = i;
      break;
    }
    if (i == 3 && currentPpm >= config.thresholds[3]) levelIndex = 3;
  }

  // Выбор шрифта для PPM
  if (config.use_custom_font) {
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextSize(1);
  } else {
    display.setFont(NULL);
    display.setTextSize(3);
  }

  // Отображаем цифры с плавающим смещением, но корректируем, чтобы не выходили за экран
  int textWidth = 0;
  char ppmStr[6];
  sprintf(ppmStr, "%d", currentPpm);
  if (config.use_custom_font) {
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(ppmStr, 0, 0, &x1, &y1, &w, &h);
    textWidth = w;
  } else {
    textWidth = strlen(ppmStr) * 12; // примерная ширина символа 6x12 при размере 3
  }

  int x = (SCREEN_WIDTH / 2) - (textWidth / 2) + numberOffsetX;
  int y = (SCREEN_HEIGHT / 2) - 12 + numberOffsetY; // центрируем

  // Коррекция, чтобы не выходило за границы
  if (x < 0) x = 0;
  if (x + textWidth > SCREEN_WIDTH) x = SCREEN_WIDTH - textWidth;
  if (y < 0) y = 0;
  if (y + 24 > SCREEN_HEIGHT) y = SCREEN_HEIGHT - 24; // 24 - примерная высота

  display.setTextColor(SSD1306_WHITE);
  if (currentPpm < 0) {
    display.setCursor(20, 20);
    display.println(F("ERR"));
  } else {
    display.setCursor(x, y);
    display.print(ppmStr);
  }

  // Отображаем уровень CO2 (внизу слева)
  display.setFont(NULL);
  display.setTextSize(1);
  display.setCursor(5, SCREEN_HEIGHT - 8);
  display.print(config.level_names[levelIndex]);

  // Отображаем температуру, чередуя позиции
  if (currentTemp > -100.0 && currentTemp < 100.0) {
    char tempStr[10];
    dtostrf(currentTemp, 4, 1, tempStr);
    strcat(tempStr, " C");

    // Определяем позицию
    int tx, ty;
    switch (tempPosition) {
      case 0: // справа снизу
        tx = SCREEN_WIDTH - 5 - (strlen(tempStr) * 6);
        ty = SCREEN_HEIGHT - 8;
        break;
      case 1: // справа сверху
        tx = SCREEN_WIDTH - 5 - (strlen(tempStr) * 6);
        ty = 2;
        break;
      default: // слева сверху
        tx = 5;
        ty = 2;
        break;
    }
    display.setCursor(tx, ty);
    display.print(tempStr);
  }

  // Рамка в зависимости от уровня
  display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  if (levelIndex >= 2) {
    display.drawRect(1, 1, SCREEN_WIDTH-2, SCREEN_HEIGHT-2, SSD1306_WHITE);
  }

  display.display();
}

// ========== ЧТЕНИЕ PWM ==========
int readCO2FromPWM(int pin) {
  unsigned long th_us = pulseIn(pin, HIGH, 2000000);
  unsigned long tl_us = pulseIn(pin, LOW, 2000000);

  if (th_us == 0 || tl_us == 0) return -1;

  float th_ms = th_us / 1000.0;
  float tl_ms = tl_us / 1000.0;
  float period = th_ms + tl_ms;

  if (period < 950 || period > 1050) return -1;

  float ppm = 5000.0 * (th_ms - 2.0) / (period - 4.0);
  if (ppm < 400) ppm = 400;
  if (ppm > 5000) ppm = 5000;
  return (int)ppm;
}

// ========== ЧТЕНИЕ ТЕМПЕРАТУРЫ ==========
void readTemperature() {
  sensors.requestTemperatures();
  float temp = sensors.getTempCByIndex(0);
  if (temp != DEVICE_DISCONNECTED_C) {
    currentTemp = temp;
  } else {
    currentTemp = -127.0;
  }
}

// ========== WI-FI ==========
void setupWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(config.ssid, config.password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" OK!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" Failed! Will retry later.");
  }
}

// ========== MQTT ==========
void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Connecting to MQTT...");
    String clientId = "ESP32C3-" + String(random(0xffff), HEX);
    if (strlen(config.mqtt_user) == 0) {
      if (mqttClient.connect(clientId.c_str())) {
        Serial.println("connected");
      } else {
        Serial.print("failed, rc=");
        Serial.print(mqttClient.state());
        delay(5000);
      }
    } else {
      if (mqttClient.connect(clientId.c_str(), config.mqtt_user, config.mqtt_pass)) {
        Serial.println("connected");
      } else {
        Serial.print("failed, rc=");
        Serial.print(mqttClient.state());
        delay(5000);
      }
    }
  }
}

void sendMqtt() {
  if (currentPpm > 0) {
    String payload = String(currentPpm);
    if (mqttClient.publish(config.mqtt_topic_co2, payload.c_str())) {
      Serial.print("MQTT CO2 sent: ");
      Serial.println(payload);
    } else {
      Serial.println("MQTT CO2 publish failed");
    }
  }
  if (currentTemp > -100.0 && currentTemp < 100.0) {
    String payload = String(currentTemp, 1);
    if (mqttClient.publish(config.mqtt_topic_temp, payload.c_str())) {
      Serial.print("MQTT Temp sent: ");
      Serial.println(payload);
    } else {
      Serial.println("MQTT Temp publish failed");
    }
  }
}

// ========== WEB-СЕРВЕР (красивый, адаптивный) ==========
const char* htmlHeader = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=yes">
    <title>CO₂ Monitor Config</title>
    <style>
        * { box-sizing: border-box; }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: #f0f2f5;
            margin: 0;
            padding: 20px;
            color: #333;
        }
        .container {
            max-width: 800px;
            margin: 0 auto;
            background: white;
            border-radius: 12px;
            box-shadow: 0 2px 10px rgba(0,0,0,0.1);
            overflow: hidden;
        }
        .header {
            background: #2c3e50;
            color: white;
            padding: 20px;
            text-align: center;
        }
        .header h1 {
            margin: 0;
            font-size: 1.8rem;
        }
        .version {
            font-size: 0.8rem;
            opacity: 0.8;
            margin-top: 5px;
        }
        .section {
            border-bottom: 1px solid #eee;
            padding: 15px 20px;
        }
        .section h2 {
            margin: 0 0 10px 0;
            font-size: 1.2rem;
            color: #2c3e50;
        }
        .form-group {
            margin-bottom: 12px;
            display: flex;
            flex-wrap: wrap;
            align-items: center;
        }
        .form-group label {
            flex: 1;
            min-width: 120px;
            font-weight: 500;
            margin-bottom: 5px;
        }
        .form-group input, .form-group select {
            flex: 2;
            padding: 8px;
            border: 1px solid #ccc;
            border-radius: 6px;
            font-size: 1rem;
        }
        .form-group input[type="checkbox"] {
            flex: none;
            width: 20px;
            height: 20px;
            margin-left: 0;
        }
        .schedule-group {
            margin-bottom: 12px;
        }
        .schedule-group input {
            width: 100%;
            padding: 8px;
            border: 1px solid #ccc;
            border-radius: 6px;
        }
        .threshold-group {
            display: flex;
            flex-wrap: wrap;
            gap: 10px;
            margin-bottom: 10px;
        }
        .threshold-group input {
            flex: 1;
        }
        .btn {
            background: #2c3e50;
            color: white;
            border: none;
            padding: 12px 24px;
            font-size: 1rem;
            border-radius: 6px;
            cursor: pointer;
            transition: background 0.2s;
            margin-top: 10px;
        }
        .btn:hover {
            background: #1e2b36;
        }
        .pin-group {
            margin-top: 15px;
            border-top: 1px solid #eee;
            padding-top: 15px;
        }
        .error {
            color: #e74c3c;
            font-size: 0.9rem;
            margin-top: 10px;
        }
        .footer {
            text-align: center;
            padding: 15px;
            font-size: 0.8rem;
            color: #777;
            border-top: 1px solid #eee;
        }
        .footer a {
            color: #2c3e50;
            text-decoration: none;
        }
        @media (max-width: 600px) {
            body { padding: 10px; }
            .form-group {
                flex-direction: column;
                align-items: stretch;
            }
            .form-group label {
                margin-bottom: 5px;
            }
        }
    </style>
</head>
<body>
<div class="container">
    <div class="header">
        <h1>CO₂ Monitor</h1>
        <div class="version">Версия )rawliteral" + String(FW_VERSION) + R"rawliteral( | Сборка: )rawliteral" + String(BUILD_TIMESTAMP) + R"rawliteral(</div>
    </div>
    <form method="POST" action="/save">
)rawliteral";

const char* htmlFooter = R"rawliteral(
        <div class="section">
            <div class="pin-group">
                <div class="form-group">
                    <label>ПИН-код для сохранения:</label>
                    <input type="password" name="pin" placeholder="0000" required>
                </div>
            </div>
            <button type="submit" class="btn">Сохранить и перезагрузить</button>
            <div class="error" id="pinError" style="display:none;">Неверный PIN-код</div>
        </div>
    </form>
    <div class="footer">
        <a href="https://github.com/xorkrus/ESP32-C3-MH-Z19C" target="_blank">Исходный код на GitHub</a>
    </div>
</div>
<script>
    // Простая проверка PIN на клиенте (сервер всё равно проверит)
    document.querySelector('form').addEventListener('submit', function(e) {
        var pin = document.querySelector('input[name="pin"]').value;
        if (pin !== "0000") { // предварительная проверка, но серверная проверка важнее
            e.preventDefault();
            document.getElementById('pinError').style.display = 'block';
        }
    });
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
  String html = htmlHeader;
  // Wi-Fi
  html += "<div class='section'><h2>Wi-Fi</h2>";
  html += "<div class='form-group'><label>SSID:</label><input name='ssid' value='" + String(config.ssid) + "'></div>";
  html += "<div class='form-group'><label>Пароль:</label><input name='password' type='password' value='" + String(config.password) + "'></div></div>";

  // MQTT
  html += "<div class='section'><h2>MQTT</h2>";
  html += "<div class='form-group'><label>Сервер:</label><input name='mqtt_server' value='" + String(config.mqtt_server) + "'></div>";
  html += "<div class='form-group'><label>Порт:</label><input name='mqtt_port' value='" + String(config.mqtt_port) + "'></div>";
  html += "<div class='form-group'><label>Логин:</label><input name='mqtt_user' value='" + String(config.mqtt_user) + "'></div>";
  html += "<div class='form-group'><label>Пароль:</label><input name='mqtt_pass' type='password' value='" + String(config.mqtt_pass) + "'></div>";
  html += "<div class='form-group'><label>Топик CO₂:</label><input name='mqtt_topic_co2' value='" + String(config.mqtt_topic_co2) + "'></div>";
  html += "<div class='form-group'><label>Топик температуры:</label><input name='mqtt_topic_temp' value='" + String(config.mqtt_topic_temp) + "'></div></div>";

  // NTP
  html += "<div class='section'><h2>NTP</h2>";
  html += "<div class='form-group'><label>Сервер:</label><input name='ntp_server' value='" + String(config.ntp_server) + "'></div>";
  html += "<div class='form-group'><label>Часовой пояс (часы):</label><input name='timezone' value='" + String(config.timezone) + "'></div></div>";

  // Расписание
  html += "<div class='section'><h2>Расписание выключения экрана</h2>";
  html += "<div class='form-group'><small>Формат: HH:MM-HH:MM,маска_дней (маска: биты 0-6, бит0=пн...бит6=вс). Пример: 0:00-6:00,127</small></div>";
  for (int i = 0; i < 3; i++) {
    html += "<div class='schedule-group'><input name='schedule" + String(i) + "' value='" + String(config.schedule[i]) + "' placeholder='Интервал " + String(i+1) + "'></div>";
  }
  html += "</div>";

  // Кнопка
  html += "<div class='section'><h2>Кнопка пробуждения</h2>";
  html += "<div class='form-group'><label>Включена:</label><input type='checkbox' name='button_enabled' " + String(config.button_enabled ? "checked" : "") + "></div>";
  html += "<div class='form-group'><label>GPIO:</label><input name='button_gpio' value='" + String(config.button_gpio) + "'></div>";
  html += "<div class='form-group'><label>Время пробуждения (сек):</label><input name='button_wake_time' value='" + String(config.button_wake_time) + "'></div>";
  html += "<div class='form-group'><small>* Краткое нажатие = пробуждение на указанное время. Долгое удержание = пробуждение на время удержания.</small></div></div>";

  // CO2 thresholds
  html += "<div class='section'><h2>Пороги CO₂</h2>";
  for (int i = 0; i < 4; i++) {
    html += "<div class='threshold-group'>";
    html += "<input name='threshold" + String(i) + "' value='" + String(config.thresholds[i]) + "' placeholder='Порог " + String(i+1) + "' size='5'>";
    html += "<input name='levelname" + String(i) + "' value='" + String(config.level_names[i]) + "' placeholder='Название уровня' style='flex:2'>";
    html += "</div>";
  }
  html += "</div>";

  // Font
  html += "<div class='section'><h2>Дисплей</h2>";
  html += "<div class='form-group'><label>Использовать жирный шрифт:</label><input type='checkbox' name='use_custom_font' " + String(config.use_custom_font ? "checked" : "") + "></div></div>";

  html += htmlFooter;
  server.send(200, "text/html", html);
}

void handleSave() {
  // Проверка PIN
  if (!server.hasArg("pin") || server.arg("pin") != "0000") {
    server.send(403, "text/plain", "Неверный PIN-код");
    return;
  }

  // Сохраняем настройки
  if (server.hasArg("ssid")) strlcpy(config.ssid, server.arg("ssid").c_str(), sizeof(config.ssid));
  if (server.hasArg("password")) strlcpy(config.password, server.arg("password").c_str(), sizeof(config.password));
  if (server.hasArg("mqtt_server")) strlcpy(config.mqtt_server, server.arg("mqtt_server").c_str(), sizeof(config.mqtt_server));
  if (server.hasArg("mqtt_port")) config.mqtt_port = server.arg("mqtt_port").toInt();
  if (server.hasArg("mqtt_user")) strlcpy(config.mqtt_user, server.arg("mqtt_user").c_str(), sizeof(config.mqtt_user));
  if (server.hasArg("mqtt_pass")) strlcpy(config.mqtt_pass, server.arg("mqtt_pass").c_str(), sizeof(config.mqtt_pass));
  if (server.hasArg("mqtt_topic_co2")) strlcpy(config.mqtt_topic_co2, server.arg("mqtt_topic_co2").c_str(), sizeof(config.mqtt_topic_co2));
  if (server.hasArg("mqtt_topic_temp")) strlcpy(config.mqtt_topic_temp, server.arg("mqtt_topic_temp").c_str(), sizeof(config.mqtt_topic_temp));
  if (server.hasArg("ntp_server")) strlcpy(config.ntp_server, server.arg("ntp_server").c_str(), sizeof(config.ntp_server));
  if (server.hasArg("timezone")) config.timezone = server.arg("timezone").toInt();
  for (int i = 0; i < 3; i++) {
    String param = "schedule" + String(i);
    if (server.hasArg(param)) strlcpy(config.schedule[i], server.arg(param).c_str(), 32);
  }
  config.button_enabled = server.hasArg("button_enabled");
  if (server.hasArg("button_gpio")) config.button_gpio = server.arg("button_gpio").toInt();
  if (server.hasArg("button_wake_time")) config.button_wake_time = server.arg("button_wake_time").toInt();
  for (int i = 0; i < 4; i++) {
    String thParam = "threshold" + String(i);
    if (server.hasArg(thParam)) config.thresholds[i] = server.arg(thParam).toInt();
    String nameParam = "levelname" + String(i);
    if (server.hasArg(nameParam)) strlcpy(config.level_names[i], server.arg(nameParam).c_str(), 32);
  }
  config.use_custom_font = server.hasArg("use_custom_font");

  saveConfig();
  server.send(200, "text/html", "<html><body><h2>Настройки сохранены, перезагрузка...</h2></body></html>");
  delay(1000);
  ESP.restart();
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  Serial.println("HTTP server started");
}

// ========== КНОПКА (опрашиваем в loop) ==========
void handleButton() {
  if (!config.button_enabled) return;

  bool pinState = digitalRead(config.button_gpio); // INPUT_PULLUP, LOW при нажатии
  unsigned long now = millis();

  switch (btnState) {
    case BUTTON_IDLE:
      if (pinState == LOW) { // нажата
        btnState = BUTTON_PRESSED;
        buttonPressTime = now;
        buttonLongPressHandled = false;
      }
      break;

    case BUTTON_PRESSED:
      if (pinState == HIGH) { // отпущена
        // Краткое нажатие
        if (!buttonLongPressHandled && (now - buttonPressTime) < LONG_PRESS_MS) {
          // короткое нажатие: пробуждение на config.button_wake_time секунд
          buttonWakeUntil = now + config.button_wake_time * 1000UL;
          scheduleEnabled = false;
        }
        btnState = BUTTON_IDLE;
      } else if ((now - buttonPressTime) >= LONG_PRESS_MS && !buttonLongPressHandled) {
        // удержание началось
        buttonLongPressHandled = true;
        // включаем экран и держим включённым, пока кнопка удерживается
        buttonWakeUntil = now + 0xFFFFFFFF; // очень большое значение, чтобы экран не выключался по расписанию
        scheduleEnabled = false;
        setDisplayPower(true);
      }
      break;
  }

  // Если мы в режиме удержания и кнопка отпущена, выключаем экран
  if (buttonLongPressHandled && digitalRead(config.button_gpio) == HIGH) {
    buttonWakeUntil = 0; // отключаем принудительное включение
    scheduleEnabled = true;
    setDisplayPower(shouldDisplayOn()); // обновляем состояние по расписанию
    btnState = BUTTON_IDLE;
  }
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("CO2 Monitor v" + String(FW_VERSION));

  // Инициализация дисплея
  I2Cbus.begin(I2C_SDA, I2C_SCL);
  I2Cbus.setClock(100000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 not found");
    while (1);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.println("CO2 Monitor v" + String(FW_VERSION));
  display.println("Loading...");
  display.display();

  // Загрузка конфигурации
  loadConfig();

  // Настройка шрифта
  if (config.use_custom_font) {
    display.setFont(&FreeMonoBold9pt7b);
  } else {
    display.setFont(NULL);
  }

  // Настройка пинов
  pinMode(PWM_PIN, INPUT);
  if (config.button_enabled) {
    pinMode(config.button_gpio, INPUT_PULLUP);
  }

  // Инициализация DS18B20
  sensors.begin();

  // Wi-Fi
  setupWiFi();

  // MQTT
  mqttClient.setServer(config.mqtt_server, config.mqtt_port);

  // NTP
  timeClient.begin();
  timeClient.setTimeOffset(config.timezone * 3600);
  timeClient.update();

  // Анимация
  initBubbles();
  randomSeed(analogRead(0));

  // Веб-сервер
  setupWebServer();

  // Экран по умолчанию включён
  setDisplayPower(true);
  updateDisplayContent();

  Serial.println("Setup complete");
}

// ========== LOOP ==========
void loop() {
  unsigned long now = millis();

  // Wi-Fi reconnect
  if (WiFi.status() != WL_CONNECTED) {
    setupWiFi();
  }

  // MQTT reconnect & loop
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  // NTP update каждые 5 секунд
  static unsigned long lastTimeUpdate = 0;
  if (now - lastTimeUpdate >= 5000) {
    lastTimeUpdate = now;
    timeClient.update();
  }

  // Чтение PWM (CO2)
  int ppm = readCO2FromPWM(PWM_PIN);
  if (ppm > 0) {
    currentPpm = ppm;
    Serial.print("CO2: ");
    Serial.print(currentPpm);
    Serial.println(" ppm");
  } else {
    Serial.println("PWM read error");
    currentPpm = -1;
  }

  // Чтение температуры
  if (now - lastTempRead >= TEMP_READ_INTERVAL) {
    lastTempRead = now;
    readTemperature();
    Serial.print("Temp: ");
    Serial.println(currentTemp);
  }

  // Меняем позицию температуры каждые 5 секунд
  if (now - lastTempPosChange >= TEMP_POS_INTERVAL) {
    lastTempPosChange = now;
    tempPosition = (tempPosition + 1) % 3;
  }

  // Обработка кнопки
  handleButton();

  // Управление экраном по расписанию/кнопке
  bool needOn = shouldDisplayOn();
  setDisplayPower(needOn);

  // Обновление содержимого с анимацией
  if (now - lastUpdate >= UPDATE_INTERVAL) {
    lastUpdate = now;
    if (displayState) {
      // Обновляем смещение цифр, но корректируем позже при отрисовке
      numberOffsetX = random(-3, 4);
      numberOffsetY = random(-3, 4);
      updateDisplayContent();
    }
  }

  // MQTT отправка раз в минуту
  if (now - lastMqttSend >= MQTT_INTERVAL) {
    lastMqttSend = now;
    if (currentPpm > 0 || (currentTemp > -100.0 && currentTemp < 100.0)) {
      sendMqtt();
    }
  }

  server.handleClient();
  delay(10);
}
