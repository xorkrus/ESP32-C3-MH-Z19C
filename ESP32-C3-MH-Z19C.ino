/*
Монитор CO₂ с ШИМ-датчиком (MH-Z19C) – Версия 2.6
Функции:
- Веб-конфигурация (Wi-Fi, MQTT, NTP, расписание экрана, кнопка, пороговые значения CO₂, размеры шрифтов, настройки DS18B20)
- OLED SSD1306 с анимацией (пузырьки, движущиеся цифры)
- Кнопка с двумя режимами: короткое нажатие (пробуждение на заданное время) / длительное нажатие (пробуждение при удержании)
- Публикация данных о CO₂ и температуре через MQTT (DS18B20)
- Настраиваемый PIN-код для сохранения настроек
- Адаптивный веб-интерфейс с указанием версии, метки времени сборки, графиков CO₂/температуры в реальном времени
- Регулируемые размеры шрифта для CO₂, температуры и информационного текста (1–3)
- IP-адрес отображается на экране в течение 5 секунд после подключения к Wi-Fi
- Улучшенная заставка: метка уровня CO₂ перемещается между левым/правым нижним углами
- Анимация пузырьков: заполненные и обведенные пузырьки, без наложения с цифрами CO₂
- Режим точки доступа для настройки при сбое Wi-Fi-соединения
- НОВОЕ: Страница конфигурации с вкладками и горизонтальной прокруткой на мобильных устройствах
- НОВОЕ: Отключение анимации (пузырьков) и отключение движения (дрейф, рамка, переключение положения уровня)
- ИСПРАВЛЕНИЕ: Панель управления работает в режиме точки доступа с данными в реальном времени и графиками
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
#define FW_VERSION "2.6"
#define BUILD_TIMESTAMP __DATE__ " " __TIME__

// ========== ПИНЫ ==========
#define I2C_SDA   2
#define I2C_SCL   3
#define PWM_PIN   4
#define BUTTON_PIN 5   // кнопка с подтяжкой к GND

// ========== ДИСПЛЕЙ ==========
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1

TwoWire I2Cbus = TwoWire(0);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &I2Cbus, OLED_RESET);

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
  bool filled;   // true = закрашенный, false = контурный
};
#define MAX_BUBBLES 15
Bubble bubbles[MAX_BUBBLES];
int numberOffsetX = 0;
int numberOffsetY = 0;

// Для чередования позиции уровня CO2
int levelNamePos = 0; // 0 - левый нижний, 1 - правый нижний
unsigned long lastLevelPosChange = 0;
const unsigned long LEVEL_POS_INTERVAL = 10000; // менять позицию каждые 10 секунд

// Для хранения истории (до 20 измерений)
#define HISTORY_SIZE 20
int co2History[HISTORY_SIZE];
float tempHistory[HISTORY_SIZE];
int historyIndex = 0;
int historyCount = 0;
unsigned long lastHistoryAdd = 0;
const unsigned long HISTORY_ADD_INTERVAL = 60000; // добавляем в историю раз в минуту

// Для отображения IP
String lastIP = "";
unsigned long ipDisplayUntil = 0;

// Режим работы Wi-Fi
bool apMode = false;          // true если работаем в режиме точки доступа
const String apSSIDprefix = "CO2Monitor_";   // префикс SSID в AP режиме

// ========== НАСТРОЙКИ ==========
struct Config {
  char ssid[32] = "xopkland";
  char password[64] = "1234567890987654321";
  char mqtt_server[40] = "192.168.1.42";
  int mqtt_port = 1883;
  char mqtt_user[32] = "";
  char mqtt_pass[32] = "";
  char mqtt_topic_co2[64] = "CO2/ppm";
  char mqtt_topic_temp[64] = "temperature";
  char ntp_server[40] = "pool.ntp.org";
  int timezone = 3;

  char schedule[3][32] = {"0:00-6:00,127", "8:00-16:00,31", ""};

  bool button_enabled = true;
  int button_gpio = BUTTON_PIN;
  int button_wake_time = 30; // секунд

  int thresholds[4] = {800, 1200, 1800, 5000};
  char level_names[4][32] = {"Норма", "Внимание", "Опасно", "Полный абзац"};

  // Датчик температуры
  bool temp_enabled = true;
  int temp_gpio = 6;

  // Настройки шрифтов
  int font_scale_co2 = 3;      // размер шрифта для PPM (1..3)
  int font_scale_temp = 1;     // размер шрифта температуры (1..3)
  int font_scale_info = 1;     // размер шрифта надписи порогов (1..3)

  // Режимы отображения
  bool disable_animation = false;  // отключает пузырьки
  bool disable_movement = false;   // отключает дрейф цифр, чередование позиции, рамку
} config;

// ========== ДАТЧИК ТЕМПЕРАТУРЫ ==========
OneWire* oneWire = nullptr;
DallasTemperature* sensors = nullptr;

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
      StaticJsonDocument<4096> doc; // увеличили размер из-за новых полей
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
        config.temp_enabled = doc["temp_enabled"] | config.temp_enabled;
        config.temp_gpio = doc["temp_gpio"] | config.temp_gpio;
        config.font_scale_co2 = doc["font_scale_co2"] | config.font_scale_co2;
        config.font_scale_temp = doc["font_scale_temp"] | config.font_scale_temp;
        config.font_scale_info = doc["font_scale_info"] | config.font_scale_info;
        config.disable_animation = doc["disable_animation"] | config.disable_animation;
        config.disable_movement = doc["disable_movement"] | config.disable_movement;

        if (config.font_scale_co2 < 1) config.font_scale_co2 = 1;
        if (config.font_scale_co2 > 3) config.font_scale_co2 = 3;
        if (config.font_scale_temp < 1) config.font_scale_temp = 1;
        if (config.font_scale_temp > 3) config.font_scale_temp = 3;
        if (config.font_scale_info < 1) config.font_scale_info = 1;
        if (config.font_scale_info > 3) config.font_scale_info = 3;
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
  StaticJsonDocument<4096> doc;
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
  doc["temp_enabled"] = config.temp_enabled;
  doc["temp_gpio"] = config.temp_gpio;
  doc["font_scale_co2"] = config.font_scale_co2;
  doc["font_scale_temp"] = config.font_scale_temp;
  doc["font_scale_info"] = config.font_scale_info;
  doc["disable_animation"] = config.disable_animation;
  doc["disable_movement"] = config.disable_movement;

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
      bubbles[i].filled = random(0, 2); // 50% закрашенных, 50% контурных
    }
  }
}

void updateBubbles() {
  if (config.disable_animation) return;
  for (int i = 0; i < MAX_BUBBLES; i++) {
    if (bubbles[i].active) {
      bubbles[i].y -= bubbles[i].speed;
      if (bubbles[i].y + bubbles[i].radius < 0) {
        bubbles[i].y = SCREEN_HEIGHT + bubbles[i].radius;
        bubbles[i].x = random(0, SCREEN_WIDTH);
        bubbles[i].radius = random(2, 6);
        bubbles[i].speed = random(1, 3);
        bubbles[i].filled = random(0, 2);
      }
    } else {
      if (random(0, 100) < 2) {
        bubbles[i].active = true;
        bubbles[i].x = random(0, SCREEN_WIDTH);
        bubbles[i].y = SCREEN_HEIGHT + random(0, 10);
        bubbles[i].radius = random(2, 6);
        bubbles[i].speed = random(1, 3);
        bubbles[i].filled = random(0, 2);
      }
    }
  }
}

void drawBubbles(int digitsX, int digitsY, int digitsW, int digitsH) {
  if (config.disable_animation) return;
  for (int i = 0; i < MAX_BUBBLES; i++) {
    if (bubbles[i].active) {
      // Проверяем пересечение с областью цифр
      bool intersects = (bubbles[i].x + bubbles[i].radius > digitsX && bubbles[i].x - bubbles[i].radius < digitsX + digitsW &&
                         bubbles[i].y + bubbles[i].radius > digitsY && bubbles[i].y - bubbles[i].radius < digitsY + digitsH);
      if (!intersects) {
        if (bubbles[i].filled) {
          display.fillCircle(bubbles[i].x, bubbles[i].y, bubbles[i].radius, SSD1306_WHITE);
        } else {
          display.drawCircle(bubbles[i].x, bubbles[i].y, bubbles[i].radius, SSD1306_WHITE);
        }
      }
    }
  }
}

// ========== ОТРИСОВКА СОДЕРЖИМОГО ==========
void updateDisplayContent() {
  if (!displayState) return;

  display.clearDisplay();

  // Если нужно показать IP
  if (ipDisplayUntil > millis()) {
    display.setFont(NULL);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("IP:");
    display.println(lastIP);
    if (apMode) {
      display.println("AP mode");
      display.println("Connect to:");
      display.println(apSSIDprefix + WiFi.macAddress().substring(9));
    } else {
      display.println("Connect to");
      display.println("/config");
    }
    display.display();
    return;
  }

  // Определяем уровень CO2
  int levelIndex = 0;
  for (int i = 0; i < 4; i++) {
    if (currentPpm < config.thresholds[i]) {
      levelIndex = i;
      break;
    }
    if (i == 3 && currentPpm >= config.thresholds[3]) levelIndex = 3;
  }

  // Если отключено перемещение, фиксируем параметры
  if (config.disable_movement) {
    numberOffsetX = 0;
    numberOffsetY = 0;
    levelNamePos = 0; // всегда слева внизу
  }

  // Рисуем CO₂
  display.setFont(NULL);
  display.setTextSize(config.font_scale_co2);
  char ppmStr[6];
  sprintf(ppmStr, "%d", currentPpm);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(ppmStr, 0, 0, &x1, &y1, &w, &h);
  int textWidth = w;
  int textHeight = h;
  int x = (SCREEN_WIDTH / 2) - (textWidth / 2) + numberOffsetX;
  int y = (SCREEN_HEIGHT / 2) - (textHeight / 2) + numberOffsetY;
  if (x < 0) x = 0;
  if (x + textWidth > SCREEN_WIDTH) x = SCREEN_WIDTH - textWidth;
  if (y < 0) y = 0;
  if (y + textHeight > SCREEN_HEIGHT) y = SCREEN_HEIGHT - textHeight;

  // Обновляем и рисуем пузырьки, если анимация включена
  updateBubbles();
  drawBubbles(x, y, textWidth, textHeight);

  // Рисуем цифры
  display.setTextColor(SSD1306_WHITE);
  if (currentPpm < 0) {
    display.setTextSize(1);
    display.setCursor(20, 20);
    display.println(F("ERR"));
  } else {
    display.setCursor(x, y);
    display.print(ppmStr);
  }

  // Рисуем уровень CO₂ (всегда слева внизу при отключённом перемещении)
  display.setTextSize(config.font_scale_info);
  int levelX, levelY = SCREEN_HEIGHT - 8 * config.font_scale_info;
  if (config.disable_movement || levelNamePos == 0) {
    levelX = 3;
  } else {
    int levelWidth = strlen(config.level_names[levelIndex]) * 6 * config.font_scale_info;
    levelX = SCREEN_WIDTH - levelWidth - 3;
  }
  display.setCursor(levelX, levelY);
  display.print(config.level_names[levelIndex]);

  // Рисуем температуру (в правом верхнем углу)
  if (config.temp_enabled && currentTemp > -100.0 && currentTemp < 100.0) {
    display.setTextSize(config.font_scale_temp);
    char tempStr[10];
    dtostrf(currentTemp, 4, 1, tempStr);
    strcat(tempStr, " C");
    int tempWidth = strlen(tempStr) * 6 * config.font_scale_temp;
    display.setCursor(SCREEN_WIDTH - tempWidth - 2, 2);
    display.print(tempStr);
  }

  // Рисуем рамку, если не отключено перемещение
  if (!config.disable_movement) {
    display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
    if (levelIndex >= 2) {
      display.drawRect(1, 1, SCREEN_WIDTH-2, SCREEN_HEIGHT-2, SSD1306_WHITE);
    }
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
  if (!config.temp_enabled || sensors == nullptr) {
    currentTemp = -127.0;
    return;
  }
  sensors->requestTemperatures();
  float temp = sensors->getTempCByIndex(0);
  if (temp != DEVICE_DISCONNECTED_C) {
    currentTemp = temp;
  } else {
    currentTemp = -127.0;
  }
}

// ========== WI-FI ==========
void setupAP() {
  String apSSID = apSSIDprefix + WiFi.macAddress().substring(9);
  Serial.println("Starting Access Point: " + apSSID);
  bool result = WiFi.softAP(apSSID.c_str(), "12345678");
  if (result) {
    Serial.println("AP Started. IP: " + WiFi.softAPIP().toString());
    lastIP = WiFi.softAPIP().toString();
    apMode = true;
    ipDisplayUntil = millis() + 5000; // показать IP на 5 секунд
  } else {
    Serial.println("AP Failed!");
  }
}

void setupWiFi() {
  if (apMode) return; // уже в режиме AP, не пытаемся подключиться

  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("Connecting to WiFi");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
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
    lastIP = WiFi.localIP().toString();
    ipDisplayUntil = millis() + 5000; // показать IP на 5 секунд
    apMode = false;
  } else {
    Serial.println(" Failed! Switching to AP mode.");
    setupAP();
  }
}

// ========== MQTT ==========
void reconnectMQTT() {
  if (apMode) return; // в AP режиме MQTT не используем
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
  if (apMode) return; // не отправляем в AP режиме
  if (currentPpm > 0) {
    String payload = String(currentPpm);
    if (mqttClient.publish(config.mqtt_topic_co2, payload.c_str())) {
      Serial.print("MQTT CO2 sent: ");
      Serial.println(payload);
    } else {
      Serial.println("MQTT CO2 publish failed");
    }
  }
  if (config.temp_enabled && currentTemp > -100.0 && currentTemp < 100.0) {
    String payload = String(currentTemp, 1);
    if (mqttClient.publish(config.mqtt_topic_temp, payload.c_str())) {
      Serial.print("MQTT Temp sent: ");
      Serial.println(payload);
    } else {
      Serial.println("MQTT Temp publish failed");
    }
  }
}

// ========== ИСТОРИЯ ДЛЯ ГРАФИКОВ ==========
void addToHistory(int co2, float temp) {
  co2History[historyIndex] = co2;
  tempHistory[historyIndex] = temp;
  historyIndex = (historyIndex + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE) historyCount++;
}

// ========== WEB-СЕРВЕР ==========
void handleRoot() {
  String modeInfo = apMode ? " (AP mode)" : "";
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=yes">
    <title>CO₂ Monitor Dashboard</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: Arial, sans-serif;
            background: #f0f2f5;
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 10px;
        }
        .dashboard {
            max-width: 800px;
            width: 100%;
            background: white;
            border-radius: 12px;
            padding: 15px;
            box-shadow: 0 2px 10px rgba(0,0,0,0.1);
            text-align: center;
        }
        .values-row {
            display: flex;
            justify-content: center;
            align-items: baseline;
            gap: 20px;
            flex-wrap: wrap;
            margin-bottom: 20px;
        }
        .co2-value {
            font-size: 4rem;
            font-weight: bold;
            color: #2c3e50;
        }
        .temp-value {
            font-size: 2rem;
            color: #2c3e50;
        }
        canvas {
            max-width: 100%;
            height: auto;
            background: #fff;
            border-radius: 8px;
            margin-top: 10px;
        }
        .config-link {
            margin-top: 15px;
        }
        .config-link a {
            color: #2c3e50;
            text-decoration: none;
            font-weight: bold;
        }
        .footer {
            margin-top: 15px;
            font-size: 0.8rem;
            color: #777;
        }
        .mode-badge {
            background: #e67e22;
            color: white;
            display: inline-block;
            padding: 4px 8px;
            border-radius: 12px;
            font-size: 0.7rem;
            margin-left: 10px;
            vertical-align: middle;
        }
        @media (max-width: 600px) {
            .co2-value { font-size: 2.5rem; }
            .temp-value { font-size: 1.5rem; }
        }
    </style>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
</head>
<body>
<div class="dashboard">
    <h1>CO₂ Monitor)rawliteral" + modeInfo + R"rawliteral(</h1>
    <div class="values-row">
        <div class="co2-value" id="co2">-- ppm</div>
        <div class="temp-value" id="temp">-- °C</div>
    </div>
    <canvas id="co2Chart" width="600" height="300"></canvas>
    <div class="config-link"><a href="/config">Настройки</a></div>
    <div class="footer">Версия )rawliteral" + String(FW_VERSION) + R"rawliteral( | Сборка: )rawliteral" + String(BUILD_TIMESTAMP) + R"rawliteral(</div>
</div>
<script>
    const ctx = document.getElementById('co2Chart').getContext('2d');
    let chart = new Chart(ctx, {
        type: 'line',
        data: {
            labels: [],
            datasets: [{
                label: 'CO₂ (ppm)',
                data: [],
                borderColor: 'rgb(75, 192, 192)',
                tension: 0.1,
                fill: false
            }]
        },
        options: {
            responsive: true,
            maintainAspectRatio: true,
            scales: {
                y: { title: { display: true, text: 'ppm' } }
            }
        }
    });

    function fetchData() {
        fetch('/api/data')
            .then(response => response.json())
            .then(data => {
                document.getElementById('co2').innerText = data.current_co2 + ' ppm';
                if (data.current_temp !== null) {
                    document.getElementById('temp').innerText = data.current_temp.toFixed(1) + ' °C';
                } else {
                    document.getElementById('temp').innerText = '-- °C';
                }
                chart.data.labels = data.labels;
                chart.data.datasets[0].data = data.co2_values;
                chart.update();
            })
            .catch(error => {
                console.error('Error fetching data:', error);
                setTimeout(fetchData, 5000);
            });
    }
    fetchData();
    setInterval(fetchData, 10000);
</script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleApiData() {
  StaticJsonDocument<2048> doc;
  doc["current_co2"] = currentPpm;
  doc["current_temp"] = (config.temp_enabled && currentTemp > -100.0) ? currentTemp : (float)NULL;
  JsonArray labels = doc.createNestedArray("labels");
  JsonArray co2Arr = doc.createNestedArray("co2_values");
  // Формируем метки времени (можно просто индексы)
  for (int i = 0; i < historyCount; i++) {
    int idx = (historyIndex - historyCount + i + HISTORY_SIZE) % HISTORY_SIZE;
    labels.add(i + 1);
    co2Arr.add(co2History[idx]);
  }
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleConfig() {
  String modeInfo = apMode ? " (AP mode)" : "";
  String html = R"rawliteral(
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
            max-width: 900px;
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
        .tabs-wrapper {
            overflow-x: auto;
            overflow-y: hidden;
            -webkit-overflow-scrolling: touch;
            scrollbar-width: thin;
            background: #ecf0f1;
            border-bottom: 1px solid #ddd;
        }
        .tabs {
            display: flex;
            flex-wrap: nowrap;
            min-width: min-content;
            white-space: nowrap;
        }
        .tab-btn {
            background: none;
            border: none;
            padding: 12px 20px;
            cursor: pointer;
            font-size: 0.9rem;
            transition: 0.2s;
            color: #2c3e50;
            white-space: nowrap;
        }
        .tab-btn:hover {
            background: #d5dbdb;
        }
        .tab-btn.active {
            background: white;
            border-bottom: 2px solid #2c3e50;
            font-weight: bold;
        }
        .tab-content {
            display: none;
            padding: 20px;
        }
        .tab-content.active {
            display: block;
        }
        .section {
            margin-bottom: 20px;
        }
        .section h2 {
            margin: 0 0 15px 0;
            font-size: 1.2rem;
            color: #2c3e50;
            border-left: 4px solid #2c3e50;
            padding-left: 10px;
        }
        .form-group {
            margin-bottom: 12px;
            display: flex;
            flex-wrap: wrap;
            align-items: center;
        }
        .form-group label {
            flex: 1;
            min-width: 140px;
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
            .tab-btn {
                padding: 10px 15px;
                font-size: 0.85rem;
            }
        }
        @media (max-width: 480px) {
            .tab-btn {
                padding: 8px 12px;
                font-size: 0.8rem;
            }
        }
    </style>
</head>
<body>
<div class="container">
    <div class="header">
        <h1>CO₂ Monitor)rawliteral" + modeInfo + R"rawliteral(</h1>
        <div class="version">Версия )rawliteral" + String(FW_VERSION) + R"rawliteral( | Сборка: )rawliteral" + String(BUILD_TIMESTAMP) + R"rawliteral(</div>
    </div>

    <div class="tabs-wrapper">
        <div class="tabs">
            <button class="tab-btn active" data-tab="wifi">Wi-Fi</button>
            <button class="tab-btn" data-tab="mqtt">MQTT</button>
            <button class="tab-btn" data-tab="ntp">NTP</button>
            <button class="tab-btn" data-tab="schedule">Расписание</button>
            <button class="tab-btn" data-tab="button">Кнопка</button>
            <button class="tab-btn" data-tab="co2">CO₂ пороги</button>
            <button class="tab-btn" data-tab="temp">Температура</button>
            <button class="tab-btn" data-tab="display">Дисплей</button>
        </div>
    </div>

    <form method="POST" action="/save">
)rawliteral";

  // ---- Wi-Fi ----
  html += R"rawliteral(
        <div class="tab-content active" id="wifi">
            <div class="section">
                <h2>Настройки Wi-Fi</h2>
                <div class="form-group"><label>SSID:</label><input name='ssid' value=')rawliteral" + String(config.ssid) + R"rawliteral('></div>
                <div class="form-group"><label>Пароль:</label><input name='password' type='password' value=')rawliteral" + String(config.password) + R"rawliteral('></div>
            </div>
        </div>
)rawliteral";

  // ---- MQTT ----
  html += R"rawliteral(
        <div class="tab-content" id="mqtt">
            <div class="section">
                <h2>Настройки MQTT</h2>
                <div class="form-group"><label>Сервер:</label><input name='mqtt_server' value=')rawliteral" + String(config.mqtt_server) + R"rawliteral('></div>
                <div class="form-group"><label>Порт:</label><input name='mqtt_port' value=')rawliteral" + String(config.mqtt_port) + R"rawliteral('></div>
                <div class="form-group"><label>Логин:</label><input name='mqtt_user' value=')rawliteral" + String(config.mqtt_user) + R"rawliteral('></div>
                <div class="form-group"><label>Пароль:</label><input name='mqtt_pass' type='password' value=')rawliteral" + String(config.mqtt_pass) + R"rawliteral('></div>
                <div class="form-group"><label>Топик CO₂:</label><input name='mqtt_topic_co2' value=')rawliteral" + String(config.mqtt_topic_co2) + R"rawliteral('></div>
                <div class="form-group"><label>Топик температуры:</label><input name='mqtt_topic_temp' value=')rawliteral" + String(config.mqtt_topic_temp) + R"rawliteral('></div>
            </div>
        </div>
)rawliteral";

  // ---- NTP ----
  html += R"rawliteral(
        <div class="tab-content" id="ntp">
            <div class="section">
                <h2>Настройки NTP</h2>
                <div class="form-group"><label>Сервер:</label><input name='ntp_server' value=')rawliteral" + String(config.ntp_server) + R"rawliteral('></div>
                <div class="form-group"><label>Часовой пояс (часы):</label><input name='timezone' value=')rawliteral" + String(config.timezone) + R"rawliteral('></div>
            </div>
        </div>
)rawliteral";

  // ---- Расписание ----
  html += R"rawliteral(
        <div class="tab-content" id="schedule">
            <div class="section">
                <h2>Расписание выключения экрана</h2>
                <div class="form-group"><small>Формат: HH:MM-HH:MM,маска_дней (маска: биты 0-6, бит0=пн...бит6=вс). Пример: 0:00-6:00,127</small></div>
                <div class="schedule-group"><input name='schedule0' value=')rawliteral" + String(config.schedule[0]) + R"rawliteral(' placeholder='Интервал 1'></div>
                <div class="schedule-group"><input name='schedule1' value=')rawliteral" + String(config.schedule[1]) + R"rawliteral(' placeholder='Интервал 2'></div>
                <div class="schedule-group"><input name='schedule2' value=')rawliteral" + String(config.schedule[2]) + R"rawliteral(' placeholder='Интервал 3'></div>
            </div>
        </div>
)rawliteral";

  // ---- Кнопка ----
  String btnChecked = config.button_enabled ? "checked" : "";
  html += R"rawliteral(
        <div class="tab-content" id="button">
            <div class="section">
                <h2>Кнопка пробуждения</h2>
                <div class="form-group"><label>Включена:</label><input type='checkbox' name='button_enabled' )rawliteral" + btnChecked + R"rawliteral('></div>
                <div class="form-group"><label>GPIO:</label><input name='button_gpio' value=')rawliteral" + String(config.button_gpio) + R"rawliteral('></div>
                <div class="form-group"><label>Время пробуждения (сек):</label><input name='button_wake_time' value=')rawliteral" + String(config.button_wake_time) + R"rawliteral('></div>
                <div class="form-group"><small>* Краткое нажатие = пробуждение на указанное время. Долгое удержание = пробуждение на время удержания.</small></div>
            </div>
        </div>
)rawliteral";

  // ---- CO₂ пороги ----
  html += R"rawliteral(
        <div class="tab-content" id="co2">
            <div class="section">
                <h2>Пороги CO₂</h2>
)rawliteral";
  for (int i = 0; i < 4; i++) {
    html += "<div class='threshold-group'>";
    html += "<input name='threshold" + String(i) + "' value='" + String(config.thresholds[i]) + "' placeholder='Порог " + String(i+1) + "' size='5'>";
    html += "<input name='levelname" + String(i) + "' value='" + String(config.level_names[i]) + "' placeholder='Название уровня' style='flex:2'>";
    html += "</div>";
  }
  html += R"rawliteral(
            </div>
        </div>
)rawliteral";

  // ---- Температура ----
  String tempChecked = config.temp_enabled ? "checked" : "";
  html += R"rawliteral(
        <div class="tab-content" id="temp">
            <div class="section">
                <h2>Датчик температуры DS18B20</h2>
                <div class="form-group"><label>Включен:</label><input type='checkbox' name='temp_enabled' )rawliteral" + tempChecked + R"rawliteral('></div>
                <div class="form-group"><label>GPIO:</label><input name='temp_gpio' value=')rawliteral" + String(config.temp_gpio) + R"rawliteral('></div>
            </div>
        </div>
)rawliteral";

  // ---- Дисплей ----
  html += R"rawliteral(
        <div class="tab-content" id="display">
            <div class="section">
                <h2>Настройки дисплея</h2>
                <div class="form-group"><label>Размер шрифта PPM (1-3):</label><select name='font_scale_co2'>
)rawliteral";
  for (int i = 1; i <= 3; i++) {
    html += "<option value='" + String(i) + "'" + (config.font_scale_co2 == i ? " selected" : "") + ">" + String(i) + "</option>";
  }
  html += R"rawliteral(
                </select></div>
                <div class="form-group"><label>Размер шрифта температуры (1-3):</label><select name='font_scale_temp'>
)rawliteral";
  for (int i = 1; i <= 3; i++) {
    html += "<option value='" + String(i) + "'" + (config.font_scale_temp == i ? " selected" : "") + ">" + String(i) + "</option>";
  }
  html += R"rawliteral(
                </select></div>
                <div class="form-group"><label>Размер шрифта инфо (1-3):</label><select name='font_scale_info'>
)rawliteral";
  for (int i = 1; i <= 3; i++) {
    html += "<option value='" + String(i) + "'" + (config.font_scale_info == i ? " selected" : "") + ">" + String(i) + "</option>";
  }
  String animChecked = config.disable_animation ? "checked" : "";
  String moveChecked = config.disable_movement ? "checked" : "";
  html += R"rawliteral(
                </select></div>
                <div class="form-group"><label>Отключить анимацию (пузырьки):</label><input type='checkbox' name='disable_animation' )rawliteral" + animChecked + R"rawliteral('></div>
                <div class="form-group"><label>Отключить перемещение (дрейф, рамка):</label><input type='checkbox' name='disable_movement' )rawliteral" + moveChecked + R"rawliteral('></div>
                <div class="form-group"><small>При отключении перемещения цифры фиксируются по центру, рамка не рисуется, уровень CO₂ всегда слева внизу.</small></div>
            </div>
        </div>
)rawliteral";

  // ---- PIN и кнопка сохранения ----
  html += R"rawliteral(
        <div class="pin-group">
            <div class="form-group">
                <label>ПИН-код для сохранения:</label>
                <input type="password" name="pin" placeholder="0000" required>
            </div>
        </div>
        <button type="submit" class="btn">Сохранить и перезагрузить</button>
        <div class="error" id="pinError" style="display:none;">Неверный PIN-код</div>
    </form>

    <div class="footer">
        <a href="/">Вернуться к дашборду</a> | <a href="https://github.com/xorkrus/ESP32-C3-MH-Z19C" target="_blank">Исходный код на GitHub</a>
    </div>
</div>

<script>
    document.querySelectorAll('.tab-btn').forEach(btn => {
        btn.addEventListener('click', () => {
            const tabId = btn.getAttribute('data-tab');
            document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
            btn.classList.add('active');
            document.querySelectorAll('.tab-content').forEach(content => content.classList.remove('active'));
            document.getElementById(tabId).classList.add('active');
        });
    });

    document.querySelector('form').addEventListener('submit', function(e) {
        var pin = document.querySelector('input[name="pin"]').value;
        if (pin !== "0000") {
            e.preventDefault();
            document.getElementById('pinError').style.display = 'block';
        }
    });
</script>
</body>
</html>
)rawliteral";

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
  config.temp_enabled = server.hasArg("temp_enabled");
  if (server.hasArg("temp_gpio")) config.temp_gpio = server.arg("temp_gpio").toInt();

  if (server.hasArg("font_scale_co2")) config.font_scale_co2 = server.arg("font_scale_co2").toInt();
  if (server.hasArg("font_scale_temp")) config.font_scale_temp = server.arg("font_scale_temp").toInt();
  if (server.hasArg("font_scale_info")) config.font_scale_info = server.arg("font_scale_info").toInt();
  config.disable_animation = server.hasArg("disable_animation");
  config.disable_movement = server.hasArg("disable_movement");

  if (config.font_scale_co2 < 1) config.font_scale_co2 = 1;
  if (config.font_scale_co2 > 3) config.font_scale_co2 = 3;
  if (config.font_scale_temp < 1) config.font_scale_temp = 1;
  if (config.font_scale_temp > 3) config.font_scale_temp = 3;
  if (config.font_scale_info < 1) config.font_scale_info = 1;
  if (config.font_scale_info > 3) config.font_scale_info = 3;

  saveConfig();
  server.send(200, "text/html", "<html><body><h2>Настройки сохранены, перезагрузка...</h2></body></html>");
  delay(1000);
  ESP.restart();
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/api/data", HTTP_GET, handleApiData);
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
      if (pinState == LOW) {
        btnState = BUTTON_PRESSED;
        buttonPressTime = now;
        buttonLongPressHandled = false;
      }
      break;

    case BUTTON_PRESSED:
      if (pinState == HIGH) {
        if (!buttonLongPressHandled && (now - buttonPressTime) < LONG_PRESS_MS) {
          // короткое нажатие
          buttonWakeUntil = now + config.button_wake_time * 1000UL;
          scheduleEnabled = false;
        }
        btnState = BUTTON_IDLE;
      } else if ((now - buttonPressTime) >= LONG_PRESS_MS && !buttonLongPressHandled) {
        buttonLongPressHandled = true;
        buttonWakeUntil = now + 0xFFFFFFFF; // очень большое значение
        scheduleEnabled = false;
        setDisplayPower(true);
      }
      break;
  }

  if (buttonLongPressHandled && digitalRead(config.button_gpio) == HIGH) {
    buttonWakeUntil = 0;
    scheduleEnabled = true;
    setDisplayPower(shouldDisplayOn());
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

  // Настройка датчика температуры
  if (config.temp_enabled) {
    oneWire = new OneWire(config.temp_gpio);
    sensors = new DallasTemperature(oneWire);
    sensors->begin();
  }

  // Настройка пинов
  pinMode(PWM_PIN, INPUT);
  if (config.button_enabled) {
    pinMode(config.button_gpio, INPUT_PULLUP);
  }

  // Wi-Fi (попытка подключиться к сохранённой сети, при неудаче - AP)
  setupWiFi();

  // MQTT (если не в AP режиме)
  if (!apMode) {
    mqttClient.setServer(config.mqtt_server, config.mqtt_port);
  }

  // NTP (только если не AP режим)
  if (!apMode) {
    timeClient.begin();
    timeClient.setTimeOffset(config.timezone * 3600);
    timeClient.update();
  }

  // Анимация
  initBubbles();
  randomSeed(analogRead(0));

  // История
  for (int i = 0; i < HISTORY_SIZE; i++) {
    co2History[i] = 0;
    tempHistory[i] = -127.0;
  }

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

  // Wi-Fi reconnect только если не в AP режиме
  if (!apMode && WiFi.status() != WL_CONNECTED) {
    setupWiFi();
  }

  // MQTT reconnect & loop (только если не AP режим)
  if (!apMode) {
    if (!mqttClient.connected()) {
      reconnectMQTT();
    }
    mqttClient.loop();

    // NTP update каждые 5 секунд (только если не AP)
    static unsigned long lastTimeUpdate = 0;
    if (now - lastTimeUpdate >= 5000) {
      lastTimeUpdate = now;
      timeClient.update();
    }
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
    if (currentTemp > -100.0) {
      Serial.print("Temp: ");
      Serial.println(currentTemp);
    }
  }

  // Добавление в историю
  if (now - lastHistoryAdd >= HISTORY_ADD_INTERVAL) {
    lastHistoryAdd = now;
    addToHistory(currentPpm, currentTemp);
  }

  // Чередование позиции уровня CO2 (только если не отключено перемещение)
  if (!config.disable_movement && (now - lastLevelPosChange >= LEVEL_POS_INTERVAL)) {
    lastLevelPosChange = now;
    levelNamePos = (levelNamePos + 1) % 2;
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
      // Дрейф только если не отключено перемещение
      if (!config.disable_movement) {
        numberOffsetX = random(-3, 4);
        numberOffsetY = random(-3, 4);
      } else {
        numberOffsetX = 0;
        numberOffsetY = 0;
      }
      updateDisplayContent();
    }
  }

  // MQTT отправка раз в минуту (только если не AP)
  if (!apMode && (now - lastMqttSend >= MQTT_INTERVAL)) {
    lastMqttSend = now;
    if (currentPpm > 0 || (config.temp_enabled && currentTemp > -100.0 && currentTemp < 100.0)) {
      sendMqtt();
    }
  }

  server.handleClient();
  delay(10);
}
