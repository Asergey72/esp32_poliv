/* Измерение влажност, температуры и давления ESP32.
Передача данных через mosquitto и вывод в HTML
Создание проверки подключения к WiFi и MQTT
Для подключения к точке WiFi добовлены  номер канала и bssid
Добавлены задержки между отправками данных в mosquitto
Изменено name платы на ESP32_new_Test
Проба синхронизации по времени
Вывод времени в монитор порта при каждой публикации в Mosquitto
Считывание датчика влажности перенесено перед отправкой данных в MQTT
Удалена функция int readMoisturePercent() как избыточная.
Питание датчика влажности сделано постоянным от платы 3,3в. Удалены сопутствующие операторы питания датчика влажности.
Это вариант с удаленным выводом на HTML.
*/

#define MQTT_MAX_PACKET_SIZE 512  // Увеличиваем буфер со 128 до 512 байт
#define MQTT_KEEPALIVE 60         // Пинг брокера каждые 60 секунд вместо 15
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>
#include "time.h"
#include "esp_sntp.h"  // <-- ДОБАВИТЬ ЭТУ СТРОКУ в самый верх кода


// ------- Wi-Fi -------
const char* WIFI_SSID = "air";
const char* WIFI_PASSWORD = "hhPFqwLbk7JNhj7";

// Заменяем pool.ntp.org на прямые IP-адреса надежных серверов
const char* ntpServer1 = "216.239.35.0";  // ://google.com (IP)
const char* ntpServer2 = "1.1.1.1";       // Cloudflare NTP (IP)
const char* ntpServer3 = "://windows.com";

WiFiClient espClient;
PubSubClient mqtt(espClient);

const char* mqtt_server = "192.168.0.126";
const int mqtt_port = 1883;
const char* mqtt_user = "Suser";
const char* mqtt_password = "853211";

unsigned long lastMQTT = 0;
unsigned long MQTT_INTERVAL = 120000;  // каждые 2 минуты

// ------- Pins -------
const int PIN_MOISTURE_ADC = 34;  // Показания датчика

const int PIN_RELAY = 26;  // сигнал на реле
const bool RELAY_ACTIVE_LOW = false;

//---------BMP180 -------------
#include <Wire.h>
#include <Adafruit_BMP085.h>

Adafruit_BMP085 bmp;

// ------- Settings -------
enum Mode : uint8_t {
  MODE_AUTO = 0,
  MODE_MANUAL = 1
};

Mode mode = MODE_AUTO;
int moist;
int threshold_pct = 85;
int hysteresis_pct = 5;

// Временные интервалы
unsigned long cooldown_ms = 30UL * 60UL * 1000UL;  // 30 минут
unsigned long max_on_ms = 30UL * 1000UL;          // 30 секунд

unsigned long lastMQTTAttemp = 0;
const unsigned long mqttRetryInterval = 5000;  // Проверка MQTT раз в 5 секунд

// ------- Calibration -------
int ADC_DRY = 3373;
int ADC_WET = 1323;

// ------- Runtime -------


bool pumpOn = false;

unsigned long pumpStart = 0;
unsigned long lastWaterStop = 0;

const int AVG_N = 8;
/*
// ------- Average filter -------
const int AVG_N = 8;

int samples[AVG_N];
int sp = 0;
bool bufFilled = false;
*/

// =====================================================
// Pump control
// =====================================================

void pumpWrite(bool on) {
  pumpOn = on;

  if (RELAY_ACTIVE_LOW)
    digitalWrite(PIN_RELAY, on ? LOW : HIGH);
  else
    digitalWrite(PIN_RELAY, on ? HIGH : LOW);

  if (on)
    pumpStart = millis();
  else
    lastWaterStop = millis();
}

bool reconnectMQTT() {
  Serial.print("Попытка подключения к MQTT ...");

  // Генерируем уникальный Client ID на основе MAC-адреса ESP32
  // На выходе получится что-то вроде: ESP32_Garden_A1B2C3D4E5F6
  String clientID = "ESP32_Garden_" + WiFi.macAddress();
  clientID.replace(":", ""); // Убираем двоеточия для красоты

  // Подключаемся, используя уникальный ID
  if (mqtt.connect(clientID.c_str(), mqtt_user, mqtt_password)) {
    Serial.println("Успешно!");

    return true;
  } else {
    Serial.print("Ошибка state = ");
    Serial.println(mqtt.state());
    return false;
  }
}

// =====================================================
// Moisture
// =====================================================

// =====================================================
// Read ADC
// =====================================================

int readADC() {

  long sum = 0;

  for (int i = 0; i < AVG_N; i++) {
    sum += analogRead(PIN_MOISTURE_ADC);
    delay(100);
  }

  return adcToPercent(sum / AVG_N);
}

// =====================================================
// ADC -> Percent
// =====================================================

int adcToPercent(int raw) {
  long p = map(raw, ADC_DRY, ADC_WET, 0, 100);

  if (p < 0) p = 0;
  if (p > 100) p = 100;

  return (int)p;
}


// =============== Функция WiFi подключение ==============
void connected_WiFi(const char* name, const char* code_pass) {
  uint8_t bssid[] = { 0x78, 0x8A, 0x20, 0x8D, 0x5C, 0xD1 };

  WiFi.mode(WIFI_STA);

  Serial.print("Connecting to WiFi");
  WiFi.begin(name, code_pass, 6, bssid);

  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected!");

  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  Serial.print("BSSID: ");
  Serial.println(WiFi.BSSIDstr());

  Serial.print("RSSI: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");

  Serial.print("Channel: ");
  Serial.println(WiFi.channel());

  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

// Настройка времени
const char* htpServer = "pool.htp.org";
const long gmtOffset_sec = 3 * 3600;  // Смещение пояса в секундах (например, +3 часа = 10800)
const int daylightOffet_sec = 0;      // Смещение для летнего времени (0, если нет перехода)

// Функция фиксации времени события
void logEvent() {
  struct tm timeinfo;

  // getLocalTime забирает время из внутреннего программного RTC ESP32
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Ошибка: не удалось получить локальное время");
    return;
  }

  // Создаем буфер для красивого форматирования строки
  char timeStringBuff[30];
  // Формат: ГГГГ-ММ-ДД ЧЧ:ММ:СС
  strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d %H:%M:%S", &timeinfo);

  // Выводим результат
  Serial.print("[СОБЫТИЕ] Зафиксировано в: ");
  Serial.println(timeStringBuff);
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);

  pinMode(PIN_RELAY, OUTPUT);
  pumpWrite(false);

  // Если датчик не обнаружен, останавливаем выполнение
  if (!bmp.begin()) {
    Serial.println("Не найден датчик BMP085/BMP180!");
    while (1) {}
  }

  analogReadResolution(12);

  // Вызов функции подключения WiFi. Начальное подключение по WiFi
  connected_WiFi(WIFI_SSID, WIFI_PASSWORD);

  // WiFi.setSleep(false); // Отключает режим сна Wi-Fi. Модуль работает стабильно на полную мощность


  // ------- MQTT -------

  mqtt.setServer(mqtt_server, mqtt_port);

  // configTime в ESP32 может принимать до 3-х серверов для надежности
  configTime(gmtOffset_sec, daylightOffet_sec, ntpServer1, ntpServer2, ntpServer3);


  // Ожидание синхронизации
  struct tm timeinfo;
  int retryCounter = 0;

  while (!getLocalTime(&timeinfo)) {
    Serial.print("Ожидание синхронизации времени... Попытка ");
    Serial.println(++retryCounter);

    // Если не может синхронизироваться более 15 секунд, выведем предупреждение
    if (retryCounter > 15) {
      Serial.println("\n[ОШИБКА] Роутер или провайдер блокирует UDP порт 123 (NTP)!");
      Serial.println("Попробуйте раздать Wi-Fi с мобильного телефона для проверки.");
      retryCounter = 0;  // Сбрасываем счетчик и продолжаем пытаться
    }
    delay(1000);
  }
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  // 1. ЕДИНСТВЕННЫЙ вызов loop для MQTT (обрабатывает пинги и входящие пакеты)
  mqtt.loop();

  // Создаем переменную времени один раз для всего цикла loop
  unsigned long now = millis();

  // 2. БЕЗБЛОКИРОВОЧНОЕ ПЕРЕПОДКЛЮЧЕНИЕ К MQTT
  if (!mqtt.connected()) {
    if (now - lastMQTTAttemp >= mqttRetryInterval) {
      lastMQTTAttemp = now;

      // Проверяем Wi-Fi. Если он отпал — пробуем поднять
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Wi-Fi ОТКЛЮЧЕН! Переподключение Wi-Fi...");
        connected_WiFi(WIFI_SSID, WIFI_PASSWORD);
      }
      // ЕСЛИ Wi-Fi на месте, только тогда пробуем подключиться к MQTT
      else {
        Serial.println("Попытка подключения к MQTT...");
        if (reconnectMQTT()) {
          Serial.println("MQTT подключен успешно!");
          lastMQTTAttemp = 0;  // Сброс таймера при успехе
        }
      }
    }
  }

  // ------- AUTO MODE -------
  if (mode == MODE_AUTO) {
    bool cooldownOK = (now - lastWaterStop >= cooldown_ms);

    if (!pumpOn) {
      if (cooldownOK && (moist <= (threshold_pct - hysteresis_pct))) {
        pumpWrite(true);
        Serial.print("Насос включен");
        logEvent();
      }
    } else {
      bool reached = moist >= (threshold_pct + hysteresis_pct);
      bool overtime = now - pumpStart >= max_on_ms;

      if (reached || overtime) {
        pumpWrite(false);
        Serial.print("Насос выключен");
        logEvent();
      }
    }
  }

  // ------- MQTT ПУБЛИКАЦИЯ (Раз в 2 минуты) -------
  if (now - lastMQTT >= MQTT_INTERVAL) {
    lastMQTT = now;
    
    // Считываем датчик влажности
    moist = readADC();

    // Публикуем данные, только если успешно подключены к брокеру
    if (mqtt.connected()) {
      float temp = bmp.readTemperature();
      float press = bmp.readPressure() / 133.3223;
      float level = WiFi.RSSI();
      
      // Публикуем всё "пачкой" БЕЗ delay() и БЕЗ промежуточных mqtt.loop()
      mqtt.publish("garden/moisture", String(moist).c_str(), false);
      //mqtt.publish("garden/pump", pumpOn ? "ON" : "OFF", true);
      mqtt.publish("garden/temperature", String(temp, 1).c_str(), false);
      mqtt.publish("garden/pressure", String(press, 1).c_str(), false);
      //mqtt.publish("garden/mode", mode == MODE_AUTO ? "AUTO" : "MANUAL", true);
      //mqtt.publish("garden/threshold", String(threshold_pct).c_str(), false);
      // mqtt.publish("garden/RSSI", String(level, 1).c_str(), false);

      Serial.println("MQTT published");
    } else {
      Serial.println("Пропуск публикации: нет соединения с MQTT");
    }
  }

  yield();
}

