#include <Preferences.h>
#include <WiFi.h>
#include <Button2.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <GyverSegment.h>
#include "src/FastBot/FastBot.h"

/* INITIALIZATION CONSTANTS */

#define VERSION "0.9"
#define HOSTNAME "HeaterMonitor"

/***************************/

// PINS CONFIGURATION

#define LED_PIN 2                                                   // embedded LED on GPIO2
#define BUTTON_PIN 21                                               // button on GPIO21
#define DISPLAY_DIO_PIN 25                                          // display DIO (data IO) pin
#define DISPLAY_CLK_PIN 27                                          // display SCLK (Shift Clock) pin
#define DISPLAY_LAT_PIN 26                                          // display RCLK (Register Clock / Latch) pin
#define PRESSURE_SENSOR_PIN 36                                      // GPIO pin of pressure sensor
#define TEMP_SENSOR_PIN 17                                          // GPIO pin of temperature sensor

/* https://symbl.cc/en */

#define EMOJI_ALERT "\xE2\x9A\xA0"
#define EMOJI_PRESSURE "\xF0\x9F\x8D\xBE"
#define EMOJI_TEMPERATURE "\xF0\x9F\x8C\xA1"

/* Global app configuration */
struct Config {
  String networkName;
  String networkPassword;
  String botToken;
  String botAdmin;
  String otaUpdatePassword;
  bool thingSpeakEnabled;
  String thingSpeakApiKey;
  int thingSpeakReportInterval;
};

/* Settings that can be edited from Telegram bot */
struct Settings {
  bool pressureAlert;
  bool temperatureAlert;
  float pressureAlertMinLevel;
  float pressureAlertMaxLevel;
  float temperatureAlertMinLevel;
  float temperatureAlertMaxLevel;
};

String telegramChatId = "";

// Preference instance
Preferences p;

// Settings instance
Settings settings;

// Configuration instance
Config config;

// HTTP client instance
HTTPClient http;

// AsyncWebServer instance on 80 port
AsyncWebServer server(80);

// Telegram bot
FastBot bot;

// Button instance
Button2 button;

// Temperature sensor
OneWire oneWire(TEMP_SENSOR_PIN);
DallasTemperature tempSensor(&oneWire);

// 7-segment 4-digit display
Disp595_4 disp(DISPLAY_DIO_PIN, DISPLAY_CLK_PIN, DISPLAY_LAT_PIN);

// time of bot initialization
uint32_t startUnix;

// timestamp when pressure has 0 value
unsigned long pressureZeroLastTimestamp;

// current pressure value
float pressure = -1;

// current temperature value
float temperature = -1;

// flag indicating pressure alerted
bool pressureAlerted = false;

// flag indicating temperature alerted
bool temperatureAlerted = false;

// time of last report to ThingSpeak
unsigned long lastReportTimestamp = 0;

// timestamp of network connection
unsigned long networkConnectionTimestamp = 0;

// App mode state machine

#define MODE_WORK                       0
#define MODE_CONFIG                     100
#define MODE_CONFIG_VALIDATION          101
#define MODE_CONFIG_VALIDATION_COMPLETE 102

// App mode
int mode = MODE_WORK;

// Display modes

#define DISPLAY_MIXED                   0
#define DISPLAY_TEMPERATURE             1
#define DISPLAY_PRESSURE                2

int disp_mode = DISPLAY_MIXED;
int disp_mode_mixed = DISPLAY_TEMPERATURE;

bool flagChangeMode = false;

// config sections

#define MENU_SECTION_NONE                       0
#define MENU_SECTION_CHANGE                     1
#define MENU_SECTION_CHANGE_PRESSURE            2
#define MENU_SECTION_CHANGE_PRESSURE_ALERTS     3
#define MENU_SECTION_CHANGE_PRESSURE_MIN        4
#define MENU_SECTION_CHANGE_PRESSURE_MAX        5

#define MENU_SECTION_CHANGE_TEMPERATURE         6
#define MENU_SECTION_CHANGE_TEMPERATURE_ALERTS  7
#define MENU_SECTION_CHANGE_TEMPERATURE_MIN     8
#define MENU_SECTION_CHANGE_TEMPERATURE_MAX     9

int menuId = 0;
int menuSection = 0;

// validation message displayed in config mode
String validationMessage = "";

TaskHandle_t taskDisplayLoop;
TaskHandle_t taskButtonLoop;

void setup() {
  
  // init serial port to write debug info
  Serial.begin(9600);
  while(!Serial);

  Serial.println("Starting " HOSTNAME " " VERSION);

  // create display loop
  xTaskCreatePinnedToCore(displayLoop, "displayLoop", 10000, NULL, 1, &taskDisplayLoop, 1);

  // create button loop
  xTaskCreatePinnedToCore(buttonLoop, "buttonLoop", 10000, NULL, 1, &taskButtonLoop, 1);

  disp.setCursor(0);
  disp.print("boot");
  disp.update();
  disp.delay(1000);

  // button init
  button.begin(BUTTON_PIN);
  button.setClickHandler(onButtonClick);
  button.setLongClickDetectedHandler(onButtonLongClick);
  button.setDoubleClickHandler(onButtonDoubleClick);
  button.setDebounceTime(100);     // 1 ms
  button.setLongClickTime(5000);   // 5 s
  button.setDoubleClickTime(500);  // 0.5 s

  // sensor init
  tempSensor.begin();
  
  // file system init
  if(!SPIFFS.begin(true)){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  // LED init
  pinMode(LED_PIN, OUTPUT);   // Set LED pin mode
  digitalWrite(LED_PIN, LOW); // Switch off LED

  // load app settings
  loadSettings();

  // enter work mode by default
  setWorkMode();

  initWebServer();

  delay(1000);

  bot.skipUpdates();
  startUnix = bot.getUnix();
  bot.attach(onReceiveTelegramMessage);

  lastReportTimestamp = millis();
}

void displayLoop(void *parameters) {
  while(true) {
    disp.tick();
    delay(1);
  }
}

void buttonLoop(void *parameters) {
  while(true) {
    button.loop();
    delay(1);
  }
}

void loop() {

  if (flagChangeMode) {
    
    flagChangeMode = false;
    
    if (mode == MODE_CONFIG) {
      mode = MODE_WORK;    
      setWorkMode();
      return;
    }
    
    if (mode == MODE_WORK) { 
      mode = MODE_CONFIG;
      setConfigMode();
      return;
    }
  }
 
  if (mode == MODE_CONFIG_VALIDATION) {
    validateConfig();
  }
  else if (mode == MODE_WORK) {
    getSensors();
    checkWiFi();    
    bot.tick();  
    alert();
    report();
    delay(50);
    disp_mode_mixed = (millis() / 3000) % 2 + 1;
  }
}

void validateConfig() {
  validationMessage = "Check Wi-Fi connection...";
  if (!validateWiFiConfig()) {
    validationMessage = "Unable to connect to Wi-Fi access point. Check the access point name and password are correct.";
    mode = MODE_CONFIG_VALIDATION_COMPLETE;
    return;
  }

  validationMessage = "Check Telegram bot connection...";
  if (!validateBotConfig()) {
    mode = MODE_CONFIG_VALIDATION_COMPLETE;
    validationMessage = "Unable to connect to Telegram bot. Check the bot token is valid.";
    return;
  }

  if (config.thingSpeakEnabled) {
    validationMessage = "Check ThingSpeak connection...";
    if (!validateThingSpeakConfig()) {
      mode = MODE_CONFIG_VALIDATION_COMPLETE;
      validationMessage = "Unable to connect to ThingSpeak. Check the API key is valid.";
      return;
    }
  }

  // checks is OK, saving parameters
  validationMessage = "Saving configuration...";
  saveConfiguration();
  mode = MODE_CONFIG_VALIDATION_COMPLETE;
  validationMessage = "Configuration has been saved. Now you can reboot the device to return to work mode.";
}

void notFound(AsyncWebServerRequest *request) {
  Serial.println("Server: 404");
  request->send(404, "text/plain", "Not found");
}

void initWebServer() {

  // Route for root / web page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Server: GET /");
    request->send(SPIFFS, "/index.html", String(), false, processor);
  });
  server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Server: GET /index.html");
    request->send(SPIFFS, "/index.html", String(), false, processor);
  });
  
  // Route to load style.css file
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
    Serial.println("Server: GET /style.css");
    request->send(SPIFFS, "/style.css", "text/css");
  });

  // Route save/process config changes
  server.on("/", HTTP_POST, [](AsyncWebServerRequest *request){    

    Serial.println("Server: POST /");
    
    if (mode == MODE_CONFIG) {
      config.networkName = request->getParam("networkName", "")->value();
      config.networkPassword = request->getParam("networkPassword", "")->value();
      config.botToken = request->getParam("botToken", "")->value();
      config.botAdmin = request->getParam("botAdmin", "")->value();
      config.otaUpdatePassword = request->getParam("otaUpdatePassword", "")->value();
      config.thingSpeakEnabled = request->getParam("thingSpeakEnabled", "")->value() == "1";
      config.thingSpeakApiKey = request->getParam("thingSpeakApiKey", "")->value();
      config.thingSpeakReportInterval = (request->getParam("thingSpeakReportInterval", "60")->value()).toInt();
      validationMessage = "";
      mode = MODE_CONFIG_VALIDATION;
      request->send(201, "text/plain", validationMessage);
      return;
    }

    if (mode == MODE_CONFIG_VALIDATION) {
      request->send(201, "text/plain", validationMessage);
      return;
    }

    if (mode == MODE_CONFIG_VALIDATION_COMPLETE) {
      request->send(200, "text/plain", validationMessage);
      validationMessage = "";
      mode = MODE_CONFIG;
      return;
    }
    
  });

  // Route to reboot
  server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", "Rebooting...");
    ESP.restart();
  });

  server.onNotFound(notFound);
}

void setConfigMode() {

  disp.setCursor(0);
  disp.print("CONF");
  disp.update();
  
  digitalWrite(LED_PIN, LOW);

  WiFi.disconnect(true);
  WiFi.enableAP(true); 
  WiFi.mode(WIFI_AP_STA);
  IPAddress ip(192, 168, 4, 1);
  WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
  WiFi.softAP(HOSTNAME);
  delay(2000); // !important
  IPAddress myIP = WiFi.softAPIP();
  Serial.print("WiFi AP on address: ");
  Serial.println(WiFi.softAPIP());
  Serial.println("Entering CONFIG mode");
  server.begin();
  mode = MODE_CONFIG;
}

void setWorkMode() {

  // reload configuration from preferences
  loadConfiguration();

  // if network name is not set, assume it's a first launch 
  // and config mode is required
  if (config.networkName == "") {
    setConfigMode();
    return;
  }

  // init WORK mode
  disp.setCursor(0);
  disp.print("----");
  disp.update();

  WiFi.disconnect(true);
  WiFi.setHostname(HOSTNAME);
  WiFi.enableAP(false);
  WiFi.mode(WIFI_STA);
  Serial.println("Entering WORK mode");
  server.end();
  bot.setToken(config.botToken);
  mode = MODE_WORK;
}

void blink(int ms) {
  digitalWrite(LED_PIN, LOW); 
  delay(ms);
  digitalWrite(LED_PIN, HIGH);
  delay(ms);
}

void onButtonClick(Button2& btn) {
  if (mode == MODE_WORK) {
    Serial.println("Button clicked");
    disp_mode = (disp_mode + 1) % 3;
  }
}

void onButtonDoubleClick(Button2& btn) {
  if (mode == MODE_WORK) {
    Serial.println("Button double clicked");
    writePreference("display", "mode", disp_mode);
  }
}

void onButtonLongClick(Button2& btn) {  
  Serial.println("Button long clicked");
  flagChangeMode = true;
}

void connectWiFi() {  
  delay(2000);
  Serial.print("Connecting to WiFi network " + config.networkName + " ");  
  WiFi.begin(config.networkName, config.networkPassword);

  int retryCount = 0;
  while (WiFi.status() != WL_CONNECTED && retryCount < 15) { 
    Serial.print(".");
    blink(250);
    retryCount++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    networkConnectionTimestamp = millis();
    Serial.println(" OK!");
    digitalWrite(LED_PIN, HIGH);
  }
  else {
    Serial.println(" Fail!");
    digitalWrite(LED_PIN, LOW); 
  }
}

bool validateWiFiConfig() {

  Serial.println("Validating WiFi parameters...");
  WiFi.disconnect(true);
  WiFi.begin(config.networkName, config.networkPassword);

  Serial.print("Connecting to WiFi network " + config.networkName + " ");  
  int retryCount = 0;
  while (WiFi.status() != WL_CONNECTED && retryCount < 15) { 
    Serial.print(".");
    blink(250);
    retryCount++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" OK!");
    digitalWrite(LED_PIN, HIGH);
    return true;
  }
  else {
    Serial.println(" Fail!");
    digitalWrite(LED_PIN, LOW); 
    return false;
  }
}

bool validateBotConfig() {
  Serial.println("Validating Telegram config parameters...");    

  int count = 0;
  while (count < 5) {
    delay(1000); 
    String url = "https://api.telegram.org/bot" + config.botToken + "/getMe";
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {     
      Serial.println("Validation is OK!");
      return true;        
    }
    else {
      Serial.println("Validation failed! Telegram response: " + httpCode);
    }
    http.end();
    count++;
 }

 return false;
}

bool validateThingSpeakConfig() {
  Serial.println("Validating ThingSpeak config parameters...");    

  int count = 0;
  while (count < 5) {
    delay(1000); 
    String url = "https://api.thingspeak.com/update?api_key=" + config.thingSpeakApiKey;
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {        
      Serial.println("Validation is OK!");        
      return true;
    }
    else {
      Serial.println("Validation failed!");
    }
    http.end();
    count++;
 }

 return false;
}

// Checks wifi connection, if not connected, connects to.
void checkWiFi() {               
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
}

// Telegram bot message handler
void onReceiveTelegramMessage(FB_msg& msg) {
   if (msg.unix < startUnix) return;

   if (config.botAdmin != msg.username) {
      bot.sendMessage("Вы не авторизованы.", msg.chatID);
      return;
   }

   if (telegramChatId != msg.chatID) {
    telegramChatId = msg.chatID;
    writePreference("telegram", "chatId", telegramChatId);
   }

   // remote update
   if (msg.OTA && msg.text != "" && msg.text == config.otaUpdatePassword) {
     bot.update();
   }
   
   // start message
   else if (msg.text == "/start") {
     bot.sendMessage("Это Heater Monitor Bot. С помощью меня можно удалённо отслеживать температуру и давление теплоносителя в отопительном контуре. Наберите /help чтобы увидеть список доступных команд.", msg.chatID); 
   }
   
   // help request
   else if (msg.text == "/help") {
     bot.sendMessage(
        "/help - получить справку\r\n" 
        "/status - запросить состояние датчиков\r\n" 
        "/config - посмотреть/установить настройки\r\n"
        "/info - служебная информация\r\n"
        "/reboot - перезагрузить устройство",
     msg.chatID); 
   }

   // change config request
   else if (msg.data == "Изменить настройки") {
     bot.editMenu(menuId, "Давление \t Температура \n Отмена");
     menuSection = MENU_SECTION_CHANGE;
   }

   // we are inside config menu
   else if (menuSection == MENU_SECTION_CHANGE) {

     if (msg.data == "Давление") {
       bot.editMenu(menuId, "Уведомления \t Мин. порог \t Макс. порог \n Отмена");
       menuSection = MENU_SECTION_CHANGE_PRESSURE;
     }
     else if (msg.data == "Температура") {
       bot.editMenu(menuId, "Уведомления \t Мин. порог \t Макс. порог \n Отмена");
       menuSection = MENU_SECTION_CHANGE_TEMPERATURE;
     }
     else {
       resetConfigMenu();
     }
   }

   // changing pressure settings
   else if (menuSection == MENU_SECTION_CHANGE_PRESSURE) {
     if (msg.data == "Уведомления") {
       bot.editMenu(menuId, "Включить \t Выключить \n Отмена");
       menuSection = MENU_SECTION_CHANGE_PRESSURE_ALERTS;
     }
     else if (msg.data == "Мин. порог") {
       resetConfigMenu();       
       bot.sendMessage("Укажите мин. порог для давления:", msg.chatID);
       menuSection = MENU_SECTION_CHANGE_PRESSURE_MIN;
     }
     else if (msg.data == "Макс. порог") {
       resetConfigMenu();
       bot.sendMessage("Укажите макс. порог для давления:", msg.chatID);
       menuSection = MENU_SECTION_CHANGE_PRESSURE_MAX;
     }
     else {
       resetConfigMenu();
     }
   }

   // change pressure alerts
   else if (menuSection == MENU_SECTION_CHANGE_PRESSURE_ALERTS) {
     if (msg.data == "Включить") {
       settings.pressureAlert = true;     
       bot.editMessage(menuId, buildConfigMessage(), msg.chatID);
       writePreference("alerts", "pressAlert", true);
       resetConfigMenu();
     }
     else if (msg.data == "Выключить") {
       settings.pressureAlert = false;
       bot.editMessage(menuId, buildConfigMessage(), msg.chatID);
       writePreference("alerts", "pressAlert", false);
       resetConfigMenu();
     }
     else {
       resetConfigMenu();
     }
   }

   // change pressure min 
   else if (menuSection == MENU_SECTION_CHANGE_PRESSURE_MIN) {
     float p = msg.text.toFloat();
     if (p > 0) {
       settings.pressureAlertMinLevel = p;
       writePreference("alerts", "pressMin", p);
       bot.editMessage(menuId, buildConfigMessage(), msg.chatID);
       bot.sendMessage("Установлен мин. порог по давлению: " + String(p) + " bar", msg.chatID); 
     }
     else {
       bot.sendMessage("Неверный мин. порог по давлению: " + msg.text, msg.chatID); 
     }
     resetConfigMenu();
   }

   // change pressure max
   else if (menuSection == MENU_SECTION_CHANGE_PRESSURE_MAX) {
     float p = msg.text.toFloat();
     if (p > 0) {
       settings.pressureAlertMaxLevel = p;
       writePreference("alerts", "pressMax", p);
       bot.editMessage(menuId, buildConfigMessage(), msg.chatID);
       bot.sendMessage("Установлен макс. порог по давлению: " + String(p) + " bar", msg.chatID); 
     }
     else {
       bot.sendMessage("Неверный макс. порог по давлению: " + msg.text, msg.chatID); 
     }
     resetConfigMenu();
   }

   // changing temperature settings
   else if (menuSection == MENU_SECTION_CHANGE_TEMPERATURE) {
     if (msg.data == "Уведомления") {
       bot.editMenu(menuId, "Включить \t Выключить \n Отмена");
       menuSection = MENU_SECTION_CHANGE_TEMPERATURE_ALERTS;
     }
     else if (msg.data == "Мин. порог") {
       resetConfigMenu();       
       bot.sendMessage("Укажите мин. порог для температуры:", msg.chatID);
       menuSection = MENU_SECTION_CHANGE_TEMPERATURE_MIN;
     }
     else if (msg.data == "Макс. порог") {
       resetConfigMenu();
       bot.sendMessage("Укажите макс. порог для температуры:", msg.chatID);
       menuSection = MENU_SECTION_CHANGE_TEMPERATURE_MAX;
     }
     else {
       resetConfigMenu();
     }
   }

   // change temperature alerts
   else if (menuSection == MENU_SECTION_CHANGE_TEMPERATURE_ALERTS) {
     if (msg.data == "Включить") {
       settings.temperatureAlert = true;     
       bot.editMessage(menuId, buildConfigMessage(), msg.chatID);
       writePreference("alerts", "tempAlert", true);
       resetConfigMenu();
     }
     else if (msg.data == "Выключить") {
       settings.temperatureAlert = false;
       bot.editMessage(menuId, buildConfigMessage(), msg.chatID);
       writePreference("alerts", "tempAlert", false);
       resetConfigMenu();
     }
     else {
       resetConfigMenu();
     }
   }

   // change temperature min
   else if (menuSection == MENU_SECTION_CHANGE_TEMPERATURE_MIN) {
     float t = msg.text.toFloat();
     if (t > 0) {
       settings.temperatureAlertMinLevel = t;
       writePreference("alerts", "tempMin", t);
       bot.editMessage(menuId, buildConfigMessage(), msg.chatID);
       bot.sendMessage("Установлен мин. порог по температуре: " + String(t) + "°C", msg.chatID); 
     }
     else {
       bot.sendMessage("Неверный мин. порог по температуре: " + msg.text, msg.chatID); 
     }
     resetConfigMenu();
   }

   // change temperature max
   else if (menuSection == MENU_SECTION_CHANGE_TEMPERATURE_MAX) {
     float t = msg.text.toFloat();
     if (t > 0) {
       settings.temperatureAlertMaxLevel = t;
       writePreference("alerts", "tempMax", t);
       bot.editMessage(menuId, buildConfigMessage(), msg.chatID);
       bot.sendMessage("Установлен макс. порог по температуре: " + String(t) + "°C", msg.chatID); 
     }
     else {
       bot.sendMessage("Неверный макс. порог по температуре: " + msg.text, msg.chatID); 
     }
     resetConfigMenu();
   }

   else if (msg.data == "Отмена") {
     resetConfigMenu();
   }

   else if (msg.text == "/status") {
     String strPressure = pressure >= 0 ? String(pressure) + " bar" : "[датчик отключен]";
     String strTempreature = temperature >= 0 ? String(temperature) + "°C" : "[датчик отключен]";
    
     bot.sendMessage(
      EMOJI_PRESSURE 
      " Давление: " + strPressure + "\r\n"
      EMOJI_TEMPERATURE
      " Температура: " + strTempreature, msg.chatID); 
   }
   else if (msg.text == "/config") {
      String configMessage = buildConfigMessage();
      bot.inlineMenu(configMessage, "Изменить настройки", msg.chatID);
      menuId = bot.lastBotMsg();
   }
   else if (msg.text == "/info") {
    bot.sendMessage("Версия: "
      VERSION
      "\r\n"
      "Время с момента запуска: " + String(timeToString(millis())) + "\r\n"
      "Подключено к WiFi сети: " + WiFi.SSID() + "\r\n"
      "IP адрес: " + WiFi.localIP().toString() + "\r\n"
      "MAC адрес: " + WiFi.macAddress() + "\r\n"
      "Уровень сигнала: " + getWiFiStrength() + "\r\n"
      "Время в сети: " + String(timeToString(millis() - networkConnectionTimestamp)),
      msg.chatID
    );
   }
   else if (msg.text == "/reboot") {
     bot.sendMessage("Перезагрузка...", msg.chatID); 
     delay(1000); 
     bot.tickManual();
     ESP.restart();
   }
   else {     
     resetConfigMenu();
     bot.sendMessage("Неизвестная команда. Используйте /help для справки по доступным командам.", msg.chatID); 
   }
}

String buildConfigMessage() {
  String message = 
    EMOJI_PRESSURE
    " Давление\r\n"
    "Уведомления: " + String(settings.pressureAlert ? "ВКЛ" : "ВЫКЛ") + "\r\n"
    "Мин. порог: " + String(settings.pressureAlertMinLevel) + " bar" + "\r\n"
    "Макс. порог: " + String(settings.pressureAlertMaxLevel) + " bar" + "\r\n"
    EMOJI_TEMPERATURE
    " Температура\r\n"
    "Уведомления: " + String(settings.temperatureAlert ? "ВКЛ" : "ВЫКЛ") + "\r\n"
    "Мин. порог: " + String(settings.temperatureAlertMinLevel) + "°C" + "\r\n"
    "Макс. порог: " + String(settings.temperatureAlertMaxLevel) + "°C";
  return message;
}

void resetConfigMenu() {
  bot.editMenu(menuId, "Изменить настройки");
  menuSection = 0;
}

// Gets values from sensors
void getSensors() {
  getTemperature();
  getPressure();
}

// Gets temperature sensor value
void getTemperature() {

  // request sensor
  tempSensor.requestTemperatures();

  // raw value from sensor (Celsius)
  // do not need to smooth (because sensor is digital)
  temperature = tempSensor.getTempCByIndex(0);

  if (mode == MODE_WORK) {
    if (disp_mode == DISPLAY_TEMPERATURE || 
      (disp_mode == DISPLAY_MIXED && disp_mode_mixed == DISPLAY_TEMPERATURE)) { 
      disp.setCursor(0);
      if (temperature > 0) {
        disp.print(String(temperature, 0) + "*C");
      }
      else {
        disp.print("--*C");
      }
      disp.update();
    }
  }
}

// Gets pressure sensor value
void getPressure() {
  
  // analog value from sensor (0...4095)
  int analogValue = analogRead(PRESSURE_SENSOR_PIN);
  
  // raw value from sensor
  float pressureRaw = floatMap(analogValue, 0, 4095, 0.0, 5.0);

  if (pressureRaw == 0) {
    pressureZeroLastTimestamp = millis();
  }

  unsigned long currentTime = millis();

  if (currentTime - pressureZeroLastTimestamp > 10000) {
    // smoothing required, because sensor is analog
    pressure = expRunningAverage(pressureRaw, pressure, 0.05);
  }
  else {
    pressure = -1;
  }

  if (mode == MODE_WORK) {
    if (disp_mode == DISPLAY_PRESSURE || 
      (disp_mode == DISPLAY_MIXED && disp_mode_mixed == DISPLAY_PRESSURE)) {
      disp.setCursor(0);
      if (pressure > 0) {
        disp.print("P " + String(pressure, 1));
      } 
      else {
        disp.print("P --");
      }
      disp.update();
    }
  }
}

// Alerts about limits exceeding, if required
void alert() {

 // do not send alerts first 2 minutes (due to average flattening of sensors values)
 if (millis() < 2 * 60 * 1000) return;

 // if chatId is not set, skip
 if (telegramChatId == "") return;

 // set chat id for alerts
 bot.setChatID(telegramChatId);
 
 // flush alert flags if measurements back to normal
 if (pressureAlerted && pressure >= settings.pressureAlertMinLevel && pressure <= settings.pressureAlertMaxLevel) 
 {
   bot.sendMessage("Давление снова в норме.");
   pressureAlerted = false;
 }

 if (temperatureAlerted && temperature >= settings.temperatureAlertMinLevel && temperature <= settings.temperatureAlertMaxLevel) {
   bot.sendMessage("Температура снова в норме.");
   temperatureAlerted = false;
 }

 if (pressure > 0 && settings.pressureAlert) {
  
  // high
  if (pressure > settings.pressureAlertMaxLevel && !pressureAlerted) {
    bot.sendMessage(
      EMOJI_ALERT
      " Высокое давление > " + String(settings.pressureAlertMaxLevel) + " bar");
    pressureAlerted = true;
  }

  // low
  if (pressure < settings.pressureAlertMinLevel && !pressureAlerted) {
    bot.sendMessage(
      EMOJI_ALERT
      " Низкое давление < " + String(settings.pressureAlertMinLevel) + " bar");
    pressureAlerted = true;
  }
 }

 if (temperature > 0 && settings.temperatureAlert) {
  
  // high
  if (temperature > settings.temperatureAlertMaxLevel && !temperatureAlerted) {
    bot.sendMessage(
      EMOJI_ALERT
      " Высокая температура > " + String(settings.temperatureAlertMaxLevel) + "°C");
    temperatureAlerted = true;
  }

  // low
  if (temperature < settings.temperatureAlertMinLevel && !temperatureAlerted) {
    bot.sendMessage(
      EMOJI_ALERT
      " Низкая температура < " + String(settings.temperatureAlertMinLevel) + "°C");
    temperatureAlerted = true;
  }
 }
}

// Reports data to ThingSpeak server
void report() {  
  
  if (!config.thingSpeakEnabled) return;

  unsigned long currentTime = millis();
  unsigned long diff = currentTime - lastReportTimestamp;
  bool needSendReport = diff > config.thingSpeakReportInterval * 1000;
  
  if (needSendReport && (pressure >= 0 || temperature >= 0)) {

    String strPressure = pressure >= 0 ? "&field1=" + String(pressure) : "";
    String strTemperature = temperature >= 0 ? "&field2=" + String(temperature) : "";
    
    String url = "https://api.thingspeak.com/update?api_key=" + config.thingSpeakApiKey + strPressure + strTemperature;
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      blink(50);
      Serial.println("Reported to ThingSpeak successfully.");
      lastReportTimestamp = currentTime;
    }
    else {
      String payload = http.getString();
      Serial.println("Report to ThingSpeak failed. Status: " + String(httpCode) + ". Answer: " + payload);
    }
    http.end();
  }
}

void loadSettings() {
  Serial.print("Loading settings...");  
  p.begin("alerts", false);
  settings.pressureAlert = p.getBool("pressAlert", true);
  settings.pressureAlertMinLevel = p.getFloat("pressMin", 0.5);
  settings.pressureAlertMaxLevel = p.getFloat("pressMax", 2.4);
  settings.temperatureAlert = p.getBool("tempAlert", true);
  settings.temperatureAlertMinLevel = p.getFloat("tempMin", 10);
  settings.temperatureAlertMaxLevel = p.getFloat("tempMax", 80);
  p.end();

  p.begin("telegram");
  telegramChatId = p.getString("chatId", "");
  p.end();

  p.begin("display");
  disp_mode = p.getInt("mode", 0);
  p.end();
  
  Serial.println(" Done."); 
}

void loadConfiguration() {
  Serial.print("Loading configuration...");  
  p.begin("config", false);
  config.networkName = p.getString("networkName", "");
  config.networkPassword = p.getString("networkPassword", "");
  config.botToken = p.getString("botToken", "");
  config.botAdmin = p.getString("botAdmin", "");
  config.otaUpdatePassword = p.getString("otaPass", "");
  config.thingSpeakEnabled = p.getBool("tsEnabled", false);
  config.thingSpeakApiKey = p.getString("tsApiKey", "");
  config.thingSpeakReportInterval = p.getInt("tsInterval", 60);
  p.end();
  Serial.println(" Done.");
}

void saveConfiguration() {
  Serial.print("Saving configuration...");  
  p.begin("config", false);
  p.putString("networkName", config.networkName);
  p.putString("networkPassword", config.networkPassword);
  p.putString("botToken", config.botToken);
  p.putString("botAdmin", config.botAdmin);
  p.putString("otaPass", config.otaUpdatePassword);
  p.putBool("tsEnabled", config.thingSpeakEnabled);
  p.putString("tsApiKey", config.thingSpeakApiKey);
  p.putInt("tsInterval", config.thingSpeakReportInterval);
  p.end();
  Serial.println(" Done.");
}

String processor(const String& var){
  if (var == "NETWORK_NAME")
    return config.networkName;
  else if (var == "NETWORK_PASSWORD") 
    return config.networkPassword;
  else if (var == "BOT_TOKEN") 
    return config.botToken;
  else if (var == "BOT_ADMIN")
    return config.botAdmin;
  else if (var == "OTA_UPDATE_PASSWORD")
    return config.otaUpdatePassword;  
  else if (var == "THINGSPEAK_API_KEY")
    return config.thingSpeakApiKey;
  else if (var == "THINGSPEAK_ENABLED")
    return config.thingSpeakEnabled ? "checked" : "";
  else if (var == "THINGSPEAK_API_KEY_DISABLED")
    return config.thingSpeakEnabled ? "" : "disabled";
  else if (var == "THINGSPEAK_REPORT_INTERVAL")
    return String(config.thingSpeakReportInterval);
  else
    return String();
}

float floatMap(float x, float in_min, float in_max, float out_min, float out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

float expRunningAverage(float newVal, float filVal, float k) {
  filVal += (newVal - filVal) * k;
  return filVal;
}

char* timeToString(unsigned long timeInMs) {
  unsigned long milli = timeInMs;
  static char _return[32];
  unsigned long secs = milli/1000, mins=secs/60;
  unsigned int hours = mins/60, days=hours/24;
  milli -= secs * 1000;
  secs -= mins * 60;
  mins -= hours * 60;
  hours -= days * 24;
  sprintf(_return,"%d дн. %2.2d:%2.2d:%2.2d", (byte)days, (byte)hours, (byte)mins, (byte)secs);
  return _return;
}

String getWiFiStrength(){
  int points = 10;
  long rssi = 0;
  long averageRSSI = 0;
  for (int i = 0; i < points; i++){
      rssi += WiFi.RSSI();
      delay(20);
  }
  averageRSSI = rssi/points;   
  String strength = String(averageRSSI) + " dBm ";
  if (averageRSSI >= -30) strength += "(отличный)";
  else if (averageRSSI < -30 && averageRSSI >= -55) strength += "(очень хороший)";
  else if (averageRSSI < -55 && averageRSSI >= -67) strength += "(хороший)";
  else if (averageRSSI < -67 && averageRSSI >= -70) strength += "(удовлетворительный)";
  else if (averageRSSI < -70 && averageRSSI >= -80) strength += "(слабый)";
  else if (averageRSSI < -80 && averageRSSI >= -90) strength += "(плохой)";  
  else strength += "(негодный)";
  return strength;
}

void writePreference(const char* section, const char* name, String val) {
  p.begin(section, false);
  p.putString(name, val);
  p.end();
}

void writePreference(const char* section, const char* name, float val) {
  p.begin(section, false);
  p.putFloat(name, val);
  p.end();
}

void writePreference(const char* section, const char* name, bool val) {
  p.begin(section, false);
  p.putBool(name, val);
  p.end();
}

void writePreference(const char* section, const char* name, int val) {
  p.begin(section, false);
  p.putInt(name, val);
  p.end();
}
