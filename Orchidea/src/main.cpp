#include <Arduino.h>

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "time.h"
#include <WebServer.h>
#include "esp_sntp.h"  //синхронизация системного времени микроконтроллеров ESP32 по протоколу SNTP (Simple Network Time Protocol) через интернет


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
const int PIN_RELAY = 26;         // сигнал на реле
const bool RELAY_ACTIVE_LOW = false;

//---------BMP180 -------------
// #include <Wire.h>
Adafruit_BMP085 bmp;

// DS18B20 на пине D4
const int oneWireBus = 4;
OneWire oneWire(oneWireBus);
DallasTemperature dsSensors(&oneWire);

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
unsigned long max_on_ms = 30UL * 1000UL;

unsigned long lastMQTTAttemp = 0;
const unsigned long mqttRetryInterval = 5000;  // Проверка MQTT раз в 5 секунд

// ------- Calibration -------
int ADC_DRY = 3373;
int ADC_WET = 1323;

// ------- Runtime -------
WebServer server(80);

bool pumpOn = false;

unsigned long pumpStart = 0;
unsigned long lastWaterStop = 0;

// ------- Average filter -------
const int AVG_N = 8;

//int samples[AVG_N];
//int sp = 0;
//bool bufFilled = false;

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
  clientID.replace(":", "");  // Убираем двоеточия для красоты

  // Подключаемся, используя уникальный ID
  if (mqtt.connect(clientID.c_str(), mqtt_user, mqtt_password)) {

    return true;
  } else {
    Serial.print("Ошибкаб state= ");
    Serial.println(mqtt.state());
    return false;
  }
}
// =====================================================
// Moisture
// =====================================================
// =====================================================
// ADC -> Percent
// =====================================================

int adcToPercent(int raw) {
  long p = map(raw, ADC_DRY, ADC_WET, 0, 100);

  if (p < 0) p = 0;
  if (p > 100) p = 100;

  return (int)p;
}
// =====================================================
// Read ADC
// =====================================================

int readADC() {

  long sum = 0;

  for (int i = 0; i < AVG_N; i++) {
    sum += analogRead(PIN_MOISTURE_ADC);
    delay(5);
  }

  return adcToPercent(sum / AVG_N);
}


// =====================================================
// HTML
// =====================================================

String htmlPage() {
  String s;
  s.reserve(6000);

  s += F("<!DOCTYPE html>");
  s += F("<html>");
  s += F("<head>");
  s += F("<meta charset='utf-8'>");
  s += F("<meta name='viewport' content='width=device-width, initial-scale=1'>");

  s += F("<title>ESP32 Smart Irrigation</title>");

  s += F("<style>");

  s += F("body{");
  s += F("font-family:Verdana,Arial,sans-serif;");
  s += F("max-width:780px;");
  s += F("margin:24px auto;");
  s += F("line-height:1.6;");
  s += F("}");

  s += F("h1{font-size:20px}");

  s += F(".box{");
  s += F("border:1px solid #ddd;");
  s += F("padding:12px;");
  s += F("border-radius:8px;");
  s += F("margin:12px 0;");
  s += F("}");

  s += F(".row{");
  s += F("display:flex;");
  s += F("gap:12px;");
  s += F("align-items:center;");
  s += F("flex-wrap:wrap;");
  s += F("}");

  s += F("button,input,select{");
  s += F("font-size:14px;");
  s += F("padding:8px;");
  s += F("}");

  s += F(".chip{");
  s += F("display:inline-block;");
  s += F("padding:4px 10px;");
  s += F("border-radius:999px;");
  s += F("color:#fff;");
  s += F("font-weight:bold;");
  s += F("}");

  s += F(".ok{background:#2e7d32}");
  s += F(".warn{background:#ef6c00}");
  s += F(".off{background:#9e9e9e}");
  s += F(".mono{font-family:monospace}");

  s += F("</style>");
  s += F("</head>");

  s += F("<body>");

  // =====================================================
  // Заголовок
  // =====================================================

  s += F("<h1>Умный полив на ESP32</h1>");

  // =====================================================
  // Статус
  // =====================================================

  s += F("<div class='box'>");
  s += F("<h3>Статус</h3>");

  s += F("<div class='row'>");

  s += F("<div>");
  s += F("Влажность: ");
  s += F("<span id='moist' class='mono'>—</span> %");
  s += F("</div>");

  s += F("<div>");
  s += F("Насос: ");
  s += F("<span id='pump' class='chip off'>—</span>");
  s += F("</div>");

  s += F("<div>");
  s += F("Режим: ");
  s += F("<span id='mode' class='chip off'>—</span>");
  s += F("</div>");

  s += F("<div>");
  s += F("Порог: ");
  s += F("<span id='thr' class='mono'>—</span> %");
  s += F("</div>");

  s += F("<div>");
  s += F("Давление: ");
  s += F("<span id='press' class='mono'>—</span> мм рт. ст.");
  s += F("</div>");

  s += F("</div>");
  s += F("</div>");

  // =====================================================
  // Настройки
  // =====================================================

  s += F("<div class='box'>");
  s += F("<h3>Настройки</h3>");

  s += F("<div class='row'>");

  s += F("<label>");
  s += F("Режим: ");
  s += F("<select id='selmode' onchange='setMode()'>");
  s += F("<option value='auto'>Auto</option>");
  s += F("<option value='manual'>Manual</option>");
  s += F("</select>");
  s += F("</label>");

  s += F("<label>");
  s += F("Порог, %: ");
  s += F("<input id='thrIn' type='number' min='1' max='99' value='85' onblur='setThr()'>");
  s += F("</label>");

  s += F("<label>");
  s += F("Cooldown, c: ");
  s += F("<input id='coolIn' type='number' min='0' max='86400' value='1800' onblur='setCool()'>");
  s += F("</label>");

  s += F("<label>");
  s += F("Max ON, c: ");
  s += F("<input id='maxIn' type='number' min='1' max='3600' value='30' onblur='setMax()'>");
  s += F("</label>");

  s += F("</div>");
  s += F("</div>");

  // =====================================================
  // Ручное управление
  // =====================================================

  s += F("<div class='box'>");
  s += F("<h3>Ручное управление</h3>");

  s += F("<div class='row'>");

  s += F("<button onclick='pump(1)'>");
  s += F("Включить насос");
  s += F("</button>");

  s += F("<button onclick='pump(0)'>");
  s += F("Выключить насос");
  s += F("</button>");

  s += F("</div>");
  s += F("</div>");

  // =====================================================
  // JavaScript
  // =====================================================

  s += F("<script>");

  // -----------------------------------------------------
  // CSS class
  // -----------------------------------------------------

  s += F("function cls(el,c){");
  s += F("el.className='chip '+c;");
  s += F("}");

  // -----------------------------------------------------
  // Получение состояния
  // -----------------------------------------------------

  s += F("function upd(){");

  s += F("fetch('/status')");
  s += F(".then(r=>r.json())");

  s += F(".then(data=>{");

  // Влажность
  s += F("document.getElementById('moist').textContent=data.moist_pct;");

  // Порог
  s += F("document.getElementById('thr').textContent=data.thr;");

  // Давление
  s += F("document.getElementById('press').textContent=data.press;");

  // Режим
  s += F("document.getElementById('selmode').value=");
  s += F("data.mode?'manual':'auto';");

  s += F("const m=document.getElementById('mode');");

  s += F("m.textContent=data.mode?'MANUAL':'AUTO';");

  s += F("cls(m,data.mode?'warn':'ok');");

  // Насос
  s += F("const p=document.getElementById('pump');");

  s += F("p.textContent=data.pump?'ON':'OFF';");

  s += F("cls(p,data.pump?'ok':'off');");

  // Настройки
  s += F("document.getElementById('thrIn').value=data.thr;");

  s += F("document.getElementById('coolIn').value=data.cool_s;");

  s += F("document.getElementById('maxIn').value=data.maxon_s;");

  s += F("})");

  // Ошибка JSON / соединения
  s += F(".catch(e=>{");
  s += F("console.log('STATUS ERROR:',e);");
  s += F("});");

  s += F("}");

  // -----------------------------------------------------
  // Установка режима
  // -----------------------------------------------------

  s += F("function setMode(){");

  s += F("const v=document.getElementById('selmode').value;");

  s += F("fetch('/setmode?mode='+v)");
  s += F(".then(()=>upd())");

  s += F("}");

  // -----------------------------------------------------
  // Установка порога
  // -----------------------------------------------------

  s += F("function setThr(){");

  s += F("const v=document.getElementById('thrIn').value;");

  s += F("fetch('/set?thr='+v)");
  s += F(".then(()=>upd())");

  s += F("}");

  // -----------------------------------------------------
  // Установка cooldown
  // -----------------------------------------------------

  s += F("function setCool(){");

  s += F("const v=document.getElementById('coolIn').value;");

  s += F("fetch('/set?cool='+v)");
  s += F(".then(()=>upd())");

  s += F("}");

  // -----------------------------------------------------
  // Установка Max ON
  // -----------------------------------------------------

  s += F("function setMax(){");

  s += F("const v=document.getElementById('maxIn').value;");

  s += F("fetch('/set?max='+v)");
  s += F(".then(()=>upd())");

  s += F("}");

  // -----------------------------------------------------
  // Управление насосом
  // -----------------------------------------------------

  s += F("function pump(on){");

  s += F("fetch('/pump?cmd='+(on?1:0))");
  s += F(".then(()=>upd())");

  s += F("}");

  // -----------------------------------------------------
  // Первый запуск
  // -----------------------------------------------------

  s += F("upd();");

  // Обновление каждые 10 секунд
  s += F("setInterval(upd,20000);");

  s += F("</script>");

  s += F("</body>");
  s += F("</html>");

  return s;
}

// =====================================================
// Status JSON
// =====================================================

void sendStatus() {
  float press = bmp.readPressure() / 133.3223;
  int moist = readADC();

  String j = "{";

  j += "\"moist_pct\":" + String(moist) + ",";
  j += "\"pump\":" + String(pumpOn ? 1 : 0) + ",";
  j += "\"mode\":" + String((int)mode) + ",";
  j += "\"thr\":" + String(threshold_pct) + ",";
  j += "\"cool_s\":" + String(cooldown_ms / 1000) + ",";
  j += "\"maxon_s\":" + String(max_on_ms / 1000) + ",";
  j += "\"press\":" + String(press, 1);

  j += "}";

  server.send(200, "application/json", j);
}

// =====================================================
// Settings
// =====================================================

void handleSet() {
  bool changed = false;
  if (server.hasArg("thr")) {
    int t = server.arg("thr").toInt();
    if (t < 1) t = 1;
    if (t > 98) t = 98;
    threshold_pct = t;
    changed = true;
  }
  if (server.hasArg("cool")) {
    long s = server.arg("cool").toInt();
    if (s < 0) s = 0;
    if (s > 86400) s = 86400;
    cooldown_ms = (unsigned long)s * 1000UL;
    changed = true;
  }
  if (server.hasArg("max")) {
    long s = server.arg("max").toInt();
    if (s < 1) s = 1;
    if (s > 3600) s = 3600;
    max_on_ms = (unsigned long)s * 1000UL;
    changed = true;
  }

  sendStatus();
}

// =====================================================
// Mode
// =====================================================

void handleSetMode() {
  String m = server.arg("mode");
  if (m == "auto") mode = MODE_AUTO;
  else mode = MODE_MANUAL;
  sendStatus();
}

// =================Pump====================================

void handlePump() {
  int cmd = server.arg("cmd").toInt();
  pumpWrite(cmd ? true : false);
  sendStatus();
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

  // Отправка в MQTT
  if (mqtt.connected()) {
    mqtt.publish("garden/datetime", timeStringBuff);
  }
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

  dsSensors.begin();

  analogReadResolution(12);

  // Вызов функции подключения WiFi. Начальное подключение по WiFi
  connected_WiFi(WIFI_SSID, WIFI_PASSWORD);

  // WiFi.setSleep(false); // Отключает режим сна Wi-Fi. Модуль работает стабильно на полную мощность


  // ------- MQTT -------

  mqtt.setServer(mqtt_server, mqtt_port);

  // ------- HTTP -------
  server.on("/", []() {
    server.send(200, "text/html; charset=utf-8", htmlPage());
  });
  server.on("/status", []() {
    sendStatus();
  });
  server.on("/set", []() {
    handleSet();
  });
  server.on("/setmode", []() {
    handleSetMode();
  });
  server.on("/pump", []() {
    handlePump();
  });
  server.begin();
  Serial.println("HTTP server started");


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
  // 1. Обслуживаем веб-сервер
  server.handleClient();

  // 2. Обрабатываем внутренние задачи MQTT
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
      if (cooldownOK && moist <= (threshold_pct - hysteresis_pct))  // Проверка условия время задержки или влажность меньше или равно
      {
        pumpWrite(true);
        // Вызов функции фиксации времени
        logEvent();
      }
    } else {
      bool reached = moist >= (threshold_pct + hysteresis_pct);
      bool overtime = now - pumpStart >= max_on_ms;

      if (reached || overtime) {
        pumpWrite(false);
      }
    }
  }

  // ------- MQTT ПУБЛИКАЦИЯ -------
  if (now - lastMQTT >= MQTT_INTERVAL) {
    lastMQTT = now;
    // Считываем датчик влажности в каждом круге
    moist = readADC();
    
    // Публикуем данные, только если успешно подключены к брокеру
    if (mqtt.connected()) {
      dsSensors.requestTemperatures();
      float tempDS = dsSensors.getTempCByIndex(0);
      float temp = bmp.readTemperature();
      float press = bmp.readPressure() / 133.3223;
      float level = (WiFi.RSSI());

      mqtt.publish("garden/moisture", String(moist).c_str(), false);
      // mqtt.publish("garden/pump", pumpOn ? "ON" : "OFF", true);
      mqtt.publish("garden/temperature", String(temp, 1).c_str(), false);
      mqtt.publish("garden/pressure", String(press, 1).c_str(), false);
      // mqtt.publish("garden/mode", mode == MODE_AUTO ? "AUTO" : "MANUAL", true);
      mqtt.publish("garden/threshold", String(threshold_pct).c_str(), false);
      //mqtt.publish("garden/RSSI", String(level, 1).c_str(), false);
      Serial.println("MQTT published");
      Serial.print("Temp_Out = ");
      Serial.println(tempDS);
    } else {
      Serial.println("Пропуск публикации: нет соединения с MQTT");
    }
  }

  yield();
}
