#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <DHT.h>
#include <SFE_BMP180.h>
#include <LiquidCrystal_I2C.h>

// Конфигурация датчиков
#define DHTPIN D4
#define DHTTYPE DHT11
#define ALTITUDE 150.0
#define CONFIG_AP_SSID "ESP_Config"
#define CONFIG_AP_PASS "config123"
#define EEPROM_SIZE 512

// Структура для хранения настроек
struct Config {
  char wifi_ssid[32];
  char wifi_pass[32];
  char mqtt_server[40];
  char mqtt_port[6] = "1883";
  char base_topic[32] = "Hrodno";
  char uid[32] = "";
  bool configured = false;
};

const char* mqtt_client_id = "ESP8266Client";
unsigned long apModeStartTime = 0;
bool inAPMode = false;

String full_topic ="Hrodno";

// Инициализация устройств
DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);
SFE_BMP180 pressure;
Config config;

WiFiClient espClient;
PubSubClient mqttClient(espClient);
ESP8266WebServer webServer(80);

unsigned long lastMsg = 0;
#define MSG_BUFFER_SIZE 50
char msg[MSG_BUFFER_SIZE];

void setup() {
  Serial.begin(115200);
  pinMode(D8, OUTPUT);
  
  // Инициализация устройств
  dht.begin();
  lcd.init();
  lcd.backlight();
  pressure.begin();
  EEPROM.begin(EEPROM_SIZE);
  
  // Загрузка конфигурации
  loadConfig();

  full_topic = String(config.uid) + "/";
  Serial.println("uid:" + String(config.uid));
  Serial.println("\nFull topic: " + full_topic);
  if (strlen(config.uid) == 0) {
    String newUID = generateUID();
    newUID.toCharArray(config.uid, sizeof(config.uid));
    saveConfig();
  }
  
  // Попытка подключения к WiFi
  if (strlen(config.wifi_ssid) > 0) {
    connectToWiFi();
  }
  
  // Если не удалось подключиться - запускаем AP
  if (WiFi.status() != WL_CONNECTED) {
    
    startConfigAP();
  }
  
  // Настройка MQTT если есть данные
  if (strlen(config.mqtt_server) > 0) {
    mqttClient.setServer(config.mqtt_server, atoi(config.mqtt_port));
    mqttClient.setCallback(mqttCallback);
  }
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    // Работа с MQTT
    if (!mqttClient.connected()) {
      mqttReconnect();
    }
    mqttClient.loop();
    
    // Публикация данных
    publishSensorData();
  } else {
    // Режим конфигурации
    webServer.handleClient();
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN)); // Мигаем LED
    
    // Проверяем, прошло ли 30 секунд в режиме AP
    if (inAPMode && millis() - apModeStartTime > 30000) {
      inAPMode = false;
      lcd.clear();
      lcd.print("Waiting for");
      lcd.setCursor(0, 1);
      lcd.print("connection...");
    }
    
    delay(500);
  }
}

// ========== WiFi функции ==========
void connectToWiFi() {
  Serial.printf("Подключение к %s...\n", config.wifi_ssid);
  
  // Выводим на LCD информацию о подключении
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connecting to:");
  lcd.setCursor(0, 1);
  lcd.print(config.wifi_ssid);
  
  WiFi.begin(config.wifi_ssid, config.wifi_pass);
  
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 30000) {
    delay(500);
    Serial.print(".");
    
    // Мигаем подсветкой LCD во время подключения
    static bool backlightState = true;
    backlightState = !backlightState;
    if (backlightState) {
      lcd.backlight();
    } else {
      lcd.noBacklight();
    }
  }
  
  // После подключения (или ошибки) фиксируем состояние
  lcd.backlight(); // Включаем подсветку обратно
  lcd.clear();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nПодключено!");
    Serial.print("IP адрес: ");
    Serial.println(WiFi.localIP());
    
    lcd.setCursor(0, 0);
    lcd.print("WiFi connected!");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
    
    digitalWrite(LED_BUILTIN, LOW); // LED ON
  } else {
    Serial.println("\nОшибка подключения!");
    Serial.println("\nUid:"+String(config.uid ));
    
    lcd.setCursor(0, 0);
    lcd.print("Connection");
    lcd.setCursor(0, 1);
    lcd.print("FAILED!");
  }
}

void startConfigAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(CONFIG_AP_SSID, CONFIG_AP_PASS);
  
  Serial.println("Режим конфигурации AP");
  Serial.print("SSID: ");
  Serial.println(CONFIG_AP_SSID);
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());

  // Вывод на LCD и запоминаем время старта AP
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("AP Mode:");
  lcd.setCursor(0, 1);
  lcd.print("IP:");
  lcd.print(WiFi.softAPIP().toString());
  
  apModeStartTime = millis();
  inAPMode = true;

  // Настройка веб-сервера
  webServer.on("/", handleRoot);
  webServer.on("/save", handleSave);
  webServer.begin();
}

// ========== MQTT функции ==========
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("MQTT сообщение [");
  Serial.print(topic);
  Serial.print("]: ");
  
  char message[length + 1];
  for (int i = 0; i < length; i++) {
    message[i] = (char)payload[i];
  }
  message[length] = '\0';
  Serial.println(message);

  String full_LED = full_topic + "LED";
  // Обработка команд
  if (strcmp(topic, full_LED.c_str()) == 0) {
    digitalWrite(D8, (char)payload[0] == '0' ? LOW : HIGH);
  }
}

void mqttReconnect() {
  while (!mqttClient.connected()) 
  {
    Serial.print("Подключение к MQTT...");
    Serial.print("uid: " + String(config.uid));
    Serial.print("Подключение к: " + String(config.mqtt_server));
    
    if (mqttClient.connect(mqtt_client_id)) {
      Serial.println("успешно");
      String full_LED = full_topic + "LED";
      // Подписка на топики
      mqttClient.subscribe(full_LED.c_str());
      
      // Публикация статуса
      mqttClient.publish(full_topic.c_str(), "online");
    } else {
      Serial.print("ошибка, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" пробуем через 5 сек");
      delay(5000);
    }
  }
}

void publishSensorData() {
  static unsigned long lastPublish = 0;
  if (millis() - lastPublish < 5000) return;
  lastPublish = millis();

  // Буфер для сообщений об ошибках
  char errorMsg[20] = {0};
  
  // Чтение данных с DHT датчика
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  
  // Проверка ошибок DHT
  if (isnan(h) || isnan(t)) {
    strncpy(errorMsg, "DHT Error!", sizeof(errorMsg)-1);
    Serial.println("Ошибка чтения DHT датчика");
  }

  // Чтение данных с барометра
  double T = 0, P = 0, p0 = 0;
  char status = pressure.startTemperature();
  if (status == 0) {
    strncpy(errorMsg, "BMP Temp Error!", sizeof(errorMsg)-1);
    Serial.println("Ошибка старта измерения температуры BMP180");
  } else {
    delay(status);
    status = pressure.getTemperature(T);
    if (status == 0) {
      strncpy(errorMsg, "BMP Read Error!", sizeof(errorMsg)-1);
      Serial.println("Ошибка чтения температуры BMP180");
    } else {
      status = pressure.startPressure(3);
      if (status == 0) {
        strncpy(errorMsg, "BMP Press Error!", sizeof(errorMsg)-1);
        Serial.println("Ошибка старта измерения давления BMP180");
      } else {
        delay(status);
        status = pressure.getPressure(P, T);
        if (status == 0) {
          strncpy(errorMsg, "BMP Press Read Err", sizeof(errorMsg)-1);
          Serial.println("Ошибка чтения давления BMP180");
        } else {
          p0 = pressure.sealevel(P, ALTITUDE);
        }
      }
    }
  }

  // Публикация данных в MQTT

  if (!isnan(t)) {
    snprintf(msg, MSG_BUFFER_SIZE, "%.1f", t);
    mqttClient.publish((full_topic + "T").c_str(), msg);
  }
  
  if (!isnan(h)) {
    snprintf(msg, MSG_BUFFER_SIZE, "%.1f", h);
    mqttClient.publish((full_topic + "H").c_str(), msg);
  }
  
  if (p0 != 0) {
    snprintf(msg, MSG_BUFFER_SIZE, "%.1f", p0*0.75);
    mqttClient.publish((full_topic + "P").c_str(), msg);
  }

  // Вывод на LCD
  lcd.clear();
  
  if (strlen(errorMsg) > 0) {
    // Вывод сообщения об ошибке
    lcd.setCursor(0, 0);
    lcd.print(errorMsg);
  } else {
    // Вывод нормальных данных
    lcd.setCursor(0, 0);
    lcd.print("T:"); lcd.print(t, 1);
    lcd.print("C H:"); lcd.print(h, 1);
    lcd.setCursor(0, 1);
    lcd.print("P:"); lcd.print(p0*0.75, 1);
    lcd.print("mmHg");
  }
}

// ========== Веб-интерфейс ==========
void handleRoot() {
  String html = "<html><body><h1>Deviec settings</h1>"
                "<form action='/save'>"
                "<h3>WiFi Settings</h3>"
                "SSID: <input name='ssid' value='" + String(config.wifi_ssid) + "'><br>"
                "Password: <input name='pass' type='password' value='" + String(config.wifi_pass) + "'><br>"
                "<h3>MQTT Settings</h3>"
                "Server IP: <input name='mqtt_server' value='" + String(config.mqtt_server) + "'><br>"
                "Port: <input name='mqtt_port' value='" + String(config.mqtt_port) + "'><br>"
                "Base topic: <input name='base_topic' value='" + String(config.base_topic) + "'><br>"
                "<input type='submit' value='Save'></form></body></html>";
  webServer.send(200, "text/html", html);
}

void handleSave() {
  // Сохранение новых настроек
  strncpy(config.wifi_ssid, webServer.arg("ssid").c_str(), sizeof(config.wifi_ssid));
  strncpy(config.wifi_pass, webServer.arg("pass").c_str(), sizeof(config.wifi_pass));
  strncpy(config.mqtt_server, webServer.arg("mqtt_server").c_str(), sizeof(config.mqtt_server));
  strncpy(config.mqtt_port, webServer.arg("mqtt_port").c_str(), sizeof(config.mqtt_port));
  strncpy(config.base_topic, webServer.arg("base_topic").c_str(), sizeof(config.base_topic));
  config.configured = true;
  
  saveConfig();
  
 String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                "<style>"
                "body { font-family: Arial, sans-serif; max-width: 600px; margin: 0 auto; padding: 20px; text-align: center; }"
                ".uid { font-size: 24px; font-weight: bold; margin: 20px 0; padding: 10px; background-color: #f0f0f0; }"
                "</style>"
                "</head>"
                "<body>"
                "<h1>Settings Saved</h1>"
                "<p>Your device UID:</p>"
                "<p>Use it for adding in the App:</p>"
                "<div class='uid'>" + String(config.uid) + "</div>"
                "<p>Device will restart in 10 seconds...</p>"
                "<script>setTimeout(function(){ window.location.href='/'; }, 5000);</script>"
                "</body></html>";
  
  webServer.send(200, "text/html", html);
  delay(10000);
  ESP.restart();
}

String generateUID() {
  uint32_t chipId = ESP.getChipId();
  randomSeed(micros());
  
  // Формат: ESP-XXXX-XXXX (15 символов)
  String uid = "ESP-" + 
              String(chipId >> 16, HEX) + "-" +  // Берем старшие 16 бит
              String(random(0xffff), HEX);       // Случайное число
  
  uid.toUpperCase();
  return uid;
}

// ========== Работа с EEPROM ==========
void loadConfig() {
  EEPROM.get(0, config);
  if (!config.configured) {
    // Настройки по умолчанию
    memset(&config, 0, sizeof(config));
    strcpy(config.mqtt_port, "1883");
    strcpy(config.base_topic, "Hrodno");
  }
}

void saveConfig() {
  EEPROM.put(0, config);
  EEPROM.commit();
}