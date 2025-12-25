#include <WiFi.h>
#include <WebServer.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <time.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>

// ============================================================
// KONFIGURACJA DLA ESP32-C3 SUPERMINI
// ============================================================
//Your site is live at https://jaguda85.github.io/wyswietlacz-app/

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4

// ESP32-C3 SuperMini - piny SPI
#define CLK_PIN   2   // SCK (GPIO2)
#define DATA_PIN  7   // MOSI (GPIO7)
#define CS_PIN    10  // CS (GPIO10)

MD_Parola myDisplay = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
Preferences preferences;

// Konfiguracja WiFi - Access Point
const char* ap_ssid = "tekstowy";
const char* ap_password = "jagoooda";

// Konfiguracja WiFi - połączenie z internetem
const char* sta_ssid = "-BaKeR-2";
const char* sta_password = "xocwac-4Rannu-daxmep";

// Konfiguracja OpenWeatherMap
const char* weatherApiKey = "80c2ac35e59be25f3bf4d97c19ffa213";
const char* weatherCity = "Gniezno";
const char* weatherCountry = "PL";

// ============================================================
// KONFIGURACJA MQTT
// ============================================================

const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* mqtt_topic_cmd = "esp32/tekstowy/gniezno2024/cmd";      // ZMIEŃ NA SWÓJ UNIKALNY!
const char* mqtt_topic_status = "esp32/tekstowy/gniezno2024/status"; // ZMIEŃ NA SWÓJ UNIKALNY!
String mqtt_client_id_base = "ESP32_Gniezno_";
char mqtt_client_id[50];

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Serwer NTP i strefa czasowa dla Polski
const char* ntpServer = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";
const char* timeZone = "CET-1CEST,M3.5.0,M10.5.0/3";

WebServer server(80);

// Polskie nazwy miesięcy (skróty)
const char* monthNamesShort[] = {
  "STY", "LUT", "MAR", "KWI", "MAJ", "CZE",
  "LIP", "SIE", "WRZ", "PAZ", "LIS", "GRU"
};

// Polskie nazwy miesięcy (pełne)
const char* monthNamesFull[] = {
  "Stycznia", "Lutego", "Marca", "Kwietnia", "Maja", "Czerwca",
  "Lipca", "Sierpnia", "Wrzesnia", "Pazdziernika", "Listopada", "Grudnia"
};

// Polskie nazwy dni tygodnia (skróty)
const char* dayNamesShort[] = {
  "NIE", "PON", "WTO", "SRO", "CZW", "PIA", "SOB"
};

// Polskie nazwy dni tygodnia (pełne)
const char* dayNamesFull[] = {
  "Niedziela", "Poniedzialek", "Wtorek", "Sroda", 
  "Czwartek", "Piatek", "Sobota"
};

// Zmienne globalne
String displayText = "";
bool displayingUserText = false;
bool scrolling = false;
uint8_t scrollSpeed = 50;
uint8_t displayIntensity = 5;
unsigned long stopTime = 0;
bool waitingAfterStop = false;

// ZMIENNA - kontrola włączenia/wyłączenia
bool displayEnabled = true;

// Tryb wyświetlania daty: 0 = NIERUCHOMA, 1 = SCROLLOWANA
uint8_t dateDisplayMode = 0;

// Stany wyświetlacza
enum DisplayState {
  STATE_TIME,
  STATE_DAYNAME,
  STATE_DATE,
  STATE_WEATHER
};

DisplayState currentState = STATE_TIME;
unsigned long lastSwitch = 0;
const unsigned long TIME_DISPLAY_DURATION = 30000;
const unsigned long DAY_DISPLAY_DURATION = 5000;
const unsigned long DATE_DISPLAY_DURATION = 5000;

// Zmienna dla mrugającego dwukropka
bool colonVisible = true;
unsigned long lastColonBlink = 0;
const unsigned long COLON_BLINK_INTERVAL = 500;

// Zmienne dla scrollowania
bool isScrollingDate = false;
bool isScrollingWeather = false;
char scrollBuffer[300];

bool weatherPending = false;

// Zmienne pogodowe
unsigned long lastWeatherUpdate = 0;
const unsigned long WEATHER_UPDATE_INTERVAL = 180000;
float currentTemp = 0;
float feelsLike = 0;
int humidity = 0;
int pressure = 0;
int clouds = 0;
float windSpeed = 0;
String weatherDesc = "";
String weatherMain = "";
float rain1h = 0;
float snow1h = 0;
bool weatherDataValid = false;
String weatherAlert = "";

struct WeatherSettings {
  bool showTemp = true;
  bool showFeelsLike = false;
  bool showClouds = true;
  bool showRain = true;
  bool showHumidity = false;
  bool showPressure = false;
  bool showWind = false;
  bool showDescription = true;
};

WeatherSettings weatherSettings;

// ============================================================
// FUNKCJE ZAPISU/ODCZYTU USTAWIEŃ
// ============================================================

void loadSettings() {
  preferences.begin("display", false);
  
  scrollSpeed = preferences.getUChar("speed", 50);
  displayIntensity = preferences.getUChar("brightness", 5);
  dateDisplayMode = preferences.getUChar("dateMode", 0);
  
  // WCZYTAJ STAN ON/OFF (domyślnie WŁĄCZONY)
  displayEnabled = preferences.getBool("enabled", true);
  
  weatherSettings.showTemp = preferences.getBool("w_temp", true);
  weatherSettings.showFeelsLike = preferences.getBool("w_feels", false);
  weatherSettings.showClouds = preferences.getBool("w_clouds", true);
  weatherSettings.showRain = preferences.getBool("w_rain", true);
  weatherSettings.showHumidity = preferences.getBool("w_humid", false);
  weatherSettings.showPressure = preferences.getBool("w_press", false);
  weatherSettings.showWind = preferences.getBool("w_wind", false);
  weatherSettings.showDescription = preferences.getBool("w_desc", true);
  
  preferences.end();
  
  Serial.println("=== Wczytane ustawienia ===");
  Serial.printf("Predkosc: %d\n", scrollSpeed);
  Serial.printf("Jasnosc: %d\n", displayIntensity);
  Serial.printf("Stan: %s\n", displayEnabled ? "WLACZONY" : "WYLACZONY");
}

void saveSettings() {
  preferences.begin("display", false);
  
  preferences.putUChar("speed", scrollSpeed);
  preferences.putUChar("brightness", displayIntensity);
  preferences.putUChar("dateMode", dateDisplayMode);
  
  // ZAPISZ STAN ON/OFF
  preferences.putBool("enabled", displayEnabled);
  
  preferences.putBool("w_temp", weatherSettings.showTemp);
  preferences.putBool("w_feels", weatherSettings.showFeelsLike);
  preferences.putBool("w_clouds", weatherSettings.showClouds);
  preferences.putBool("w_rain", weatherSettings.showRain);
  preferences.putBool("w_humid", weatherSettings.showHumidity);
  preferences.putBool("w_press", weatherSettings.showPressure);
  preferences.putBool("w_wind", weatherSettings.showWind);
  preferences.putBool("w_desc", weatherSettings.showDescription);
  
  preferences.end();
  
  Serial.println("=== Zapisano ustawienia ===");
  Serial.printf("Stan: %s\n", displayEnabled ? "WLACZONY" : "WYLACZONY");
}

// ============================================================
// FUNKCJE POBIERANIA POGODY
// ============================================================

void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Brak WiFi - nie moge pobrac pogody");
    return;
  }
  
  HTTPClient http;
  String url = "http://api.openweathermap.org/data/2.5/weather?q=" + 
               String(weatherCity) + "," + String(weatherCountry) + 
               "&appid=" + String(weatherApiKey) + 
               "&units=metric&lang=pl";
  
  Serial.println("Pobieram pogode...");
  http.begin(url);
  http.setTimeout(5000);
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      currentTemp = doc["main"]["temp"];
      feelsLike = doc["main"]["feels_like"];
      humidity = doc["main"]["humidity"];
      pressure = doc["main"]["pressure"];
      clouds = doc["clouds"]["all"];
      windSpeed = doc["wind"]["speed"];
      weatherDesc = doc["weather"][0]["description"].as<String>();
      weatherMain = doc["weather"][0]["main"].as<String>();
      
      rain1h = doc["rain"]["1h"] | 0.0;
      snow1h = doc["snow"]["1h"] | 0.0;
      
      weatherDataValid = true;
      
      Serial.println("=== Pogoda ===");
      Serial.printf("Temp: %.1f C\n", currentTemp);
      Serial.printf("Chmury: %d%%\n", clouds);
      
      fetchForecast();
    } else {
      Serial.println("Blad JSON");
    }
  } else {
    Serial.printf("HTTP error: %d\n", httpCode);
  }
  
  http.end();
}

void fetchForecast() {
  HTTPClient http;
  String url = "http://api.openweathermap.org/data/2.5/forecast?q=" + 
               String(weatherCity) + "," + String(weatherCountry) + 
               "&appid=" + String(weatherApiKey) + 
               "&units=metric&lang=pl&cnt=1";
  
  http.begin(url);
  http.setTimeout(5000);
  int httpCode = http.GET();
  
  weatherAlert = "";
  
  if (httpCode == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      float forecastRain = doc["list"][0]["rain"]["3h"] | 0.0;
      float forecastSnow = doc["list"][0]["snow"]["3h"] | 0.0;
      String forecastMain = doc["list"][0]["weather"][0]["main"].as<String>();
      
      if (forecastRain > 0 && rain1h == 0) {
        weatherAlert = "Za 3h przewidywane opady deszczu";
      }
      else if (forecastSnow > 0 && snow1h == 0) {
        weatherAlert = "Za 3h przewidywany snieg";
      }
      else if (forecastMain != weatherMain) {
        weatherAlert = "Za 3h zmiana pogody";
      }
    }
  }
  
  http.end();
}

String getWeatherText() {
  if (!weatherDataValid) {
    return "Brak danych pogodowych";
  }
  
  String weatherText = "";
  
  if (weatherSettings.showTemp) {
    weatherText += String((int)round(currentTemp)) + "st.C";
  }
  
  if (weatherSettings.showFeelsLike) {
    if (weatherText.length() > 0) weatherText += "  ->  ";
    weatherText += "Odczuw. " + String((int)round(feelsLike)) + "st.C";
  }
  
  if (weatherSettings.showClouds) {
    if (weatherText.length() > 0) weatherText += "  ->  ";
    if (clouds < 20) weatherText += "Bezchmurnie";
    else if (clouds < 50) weatherText += "Malo chmur " + String(clouds) + "%";
    else if (clouds < 80) weatherText += "Zachmurzenie " + String(clouds) + "%";
    else weatherText += "Bardzo pochmurno " + String(clouds) + "%";
  }
  
  if (weatherSettings.showRain) {
    if (rain1h > 0) {
      if (weatherText.length() > 0) weatherText += "  ->  ";
      weatherText += "Deszcz " + String(rain1h, 1) + "mm";
    }
    if (snow1h > 0) {
      if (weatherText.length() > 0) weatherText += "  ->  ";
      weatherText += "Snieg " + String(snow1h, 1) + "mm";
    }
    if (rain1h == 0 && snow1h == 0) {
      if (weatherText.length() > 0) weatherText += "  ->  ";
      weatherText += "Brak opadow";
    }
  }
  
  if (weatherSettings.showHumidity) {
    if (weatherText.length() > 0) weatherText += "  ->  ";
    weatherText += "Wilgotnosc " + String(humidity) + "%";
  }
  
  if (weatherSettings.showPressure) {
    if (weatherText.length() > 0) weatherText += "  ->  ";
    weatherText += "Cisnienie " + String(pressure) + "hPa";
  }
  
  if (weatherSettings.showWind) {
    if (weatherText.length() > 0) weatherText += "  ->  ";
    weatherText += "Wiatr " + String(windSpeed, 1) + "m/s";
  }
  
  if (weatherSettings.showDescription) {
    if (weatherText.length() > 0) weatherText += "  ->  ";
    weatherText += weatherDesc;
  }
  
  if (weatherAlert.length() > 0) {
    weatherText += "  ->  !!! " + weatherAlert + " !!!";
  }
  
  return weatherText;
}

void startWeatherDisplay() {
  String weatherText = getWeatherText();
  weatherText.toCharArray(scrollBuffer, sizeof(scrollBuffer));
  
  isScrollingWeather = true;
  isScrollingDate = false;
  currentState = STATE_TIME;
  lastSwitch = millis();
  
  myDisplay.displayClear();
  myDisplay.displayText(scrollBuffer, PA_CENTER, scrollSpeed, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  
  weatherPending = false;
  Serial.println(">>> Wyswietlam pogode");
}

// ============================================================
// FUNKCJE POMOCNICZE
// ============================================================

String getTimeString(bool showColon) {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return "??:??";
  char timeStr[10];
  strftime(timeStr, sizeof(timeStr), showColon ? "%H:%M" : "%H %M", &timeinfo);
  return String(timeStr);
}

String getDayNameShort() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return "???";
  return String(dayNamesShort[timeinfo.tm_wday]);
}

String getDateShort() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return "?? ???";
  char dayStr[3];
  sprintf(dayStr, "%02d", timeinfo.tm_mday);
  return String(dayStr) + " " + String(monthNamesShort[timeinfo.tm_mon]);
}

String getDateFull() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return "Brak czasu";
  char dayStr[3];
  sprintf(dayStr, "%02d", timeinfo.tm_mday);
  int year = timeinfo.tm_year + 1900;
  return String(dayNamesFull[timeinfo.tm_wday]) + " " + String(dayStr) + " " + 
         String(monthNamesFull[timeinfo.tm_mon]) + " " + String(year) + "r";
}

// ============================================================
// FUNKCJE MQTT
// ============================================================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.print("📩 MQTT: ");
  Serial.println(message);
  
  // WYŁĄCZ WYŚWIETLACZ - PRAWDZIWE WYŁĄCZENIE
  if (message == "OFF") {
    displayEnabled = false;
    displayingUserText = false;
    scrolling = false;
    isScrollingDate = false;
    isScrollingWeather = false;
    weatherPending = false;
    myDisplay.displayClear();
    myDisplay.displayShutdown(true); // WYŁĄCZ HARDWARE
    
    // ZAPISZ STAN DO PAMIĘCI
    preferences.begin("display", false);
    preferences.putBool("enabled", false);
    preferences.end();
    
    mqttClient.publish(mqtt_topic_status, "OFF - wylaczony");
    Serial.println("✓ Wyswietlacz WYLACZONY (hardware off) - zapisano");
  }
  
  // WŁĄCZ WYŚWIETLACZ
  else if (message == "ON") {
    displayEnabled = true;
    myDisplay.displayShutdown(false); // WŁĄCZ HARDWARE
    myDisplay.setIntensity(displayIntensity);
    myDisplay.displayClear();
    currentState = STATE_TIME;
    lastSwitch = millis();
    displayingUserText = false;
    scrolling = false;
    isScrollingDate = false;
    isScrollingWeather = false;
    weatherPending = false;
    
    // ZAPISZ STAN DO PAMIĘCI
    preferences.begin("display", false);
    preferences.putBool("enabled", true);
    preferences.end();
    
    mqttClient.publish(mqtt_topic_status, "ON - wlaczony");
    Serial.println("✓ Wyswietlacz WLACZONY - zapisano");
  }
  
  // POKAŻ DATĘ
  else if (message == "DATE") {
    if (!displayEnabled) {
      mqttClient.publish(mqtt_topic_status, "Blad: wyswietlacz wylaczony");
      return;
    }
    
    displayingUserText = false;
    scrolling = false;
    waitingAfterStop = false;
    isScrollingWeather = false;
    weatherPending = false;
    
    if (dateDisplayMode == 0) {
      isScrollingDate = false;
      currentState = STATE_DAYNAME;
      lastSwitch = millis();
      String dayName = getDayNameShort();
      myDisplay.displayClear();
      myDisplay.setTextAlignment(PA_CENTER);
      myDisplay.print(dayName.c_str());
    } else {
      String fullDate = getDateFull();
      fullDate.toCharArray(scrollBuffer, sizeof(scrollBuffer));
      isScrollingDate = true;
      currentState = STATE_TIME;
      lastSwitch = millis();
      myDisplay.displayClear();
      myDisplay.displayText(scrollBuffer, PA_CENTER, scrollSpeed, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
    }
    mqttClient.publish(mqtt_topic_status, "DATE");
    Serial.println("✓ Pokazuje DATE");
  }
  
  // POKAŻ POGODĘ
  else if (message == "WEATHER") {
    if (!displayEnabled) {
      mqttClient.publish(mqtt_topic_status, "Blad: wyswietlacz wylaczony");
      return;
    }
    
    displayingUserText = false;
    scrolling = false;
    waitingAfterStop = false;
    isScrollingDate = false;
    weatherPending = false;
    
    fetchWeather();
    startWeatherDisplay();
    lastWeatherUpdate = millis();
    
    mqttClient.publish(mqtt_topic_status, "WEATHER");
    Serial.println("✓ Pokazuje WEATHER");
  }
  
  // TEKST: + treść
  else if (message.startsWith("TEXT:")) {
    if (!displayEnabled) {
      mqttClient.publish(mqtt_topic_status, "Blad: wyswietlacz wylaczony");
      return;
    }
    
    String text = message.substring(5);
    displayText = text;
    displayingUserText = true;
    scrolling = true;
    waitingAfterStop = false;
    isScrollingDate = false;
    isScrollingWeather = false;
    weatherPending = false;
    myDisplay.displayClear();
    myDisplay.setSpeed(scrollSpeed);
    myDisplay.displayText(displayText.c_str(), PA_CENTER, scrollSpeed, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
    
    String response = "TEXT: " + text;
    mqttClient.publish(mqtt_topic_status, response.c_str());
    Serial.println("✓ TEXT: " + text);
  }
  
  // JASNOŚĆ: BRIGHTNESS:10
  else if (message.startsWith("BRIGHTNESS:")) {
    int brightness = message.substring(11).toInt();
    if (brightness >= 0 && brightness <= 15) {
      displayIntensity = brightness;
      if (displayEnabled) {
        myDisplay.setIntensity(displayIntensity);
      }
      String response = "BRIGHTNESS:" + String(brightness);
      mqttClient.publish(mqtt_topic_status, response.c_str());
      Serial.println("✓ Jasnosc: " + String(brightness));
    }
  }
  
  // PRĘDKOŚĆ: SPEED:50
  else if (message.startsWith("SPEED:")) {
    int speed = message.substring(6).toInt();
    if (speed >= 10 && speed <= 150) {
      scrollSpeed = speed;
      myDisplay.setSpeed(scrollSpeed);
      String response = "SPEED:" + String(speed);
      mqttClient.publish(mqtt_topic_status, response.c_str());
      Serial.println("✓ Predkosc: " + String(speed));
    }
  }
  
  // ZAPISZ USTAWIENIA
  else if (message == "SAVE") {
    saveSettings();
    mqttClient.publish(mqtt_topic_status, "SAVED");
    Serial.println("✓ SAVE - zapisano ustawienia");
  }
  
  // STATUS
  else if (message == "STATUS") {
    String status = String(displayEnabled ? "ON" : "OFF") + 
                    " B:" + String(displayIntensity) + 
                    " S:" + String(scrollSpeed) +
                    " T:" + String((int)currentTemp) + "C";
    mqttClient.publish(mqtt_topic_status, status.c_str());
    Serial.println("✓ STATUS: " + status);
  }
}

void mqttReconnect() {
  if (mqttClient.connected()) return;
  
  Serial.print("Laczenie z MQTT...");
  
  if (mqttClient.connect(mqtt_client_id)) {
    Serial.println(" OK!");
    mqttClient.subscribe(mqtt_topic_cmd);
    mqttClient.publish(mqtt_topic_status, "ONLINE");
    Serial.print("Topic: ");
    Serial.println(mqtt_topic_cmd);
  } else {
    Serial.print(" BLAD rc=");
    Serial.println(mqttClient.state());
  }
}

// ============================================================
// STRONA HTML
// ============================================================

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pl">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Wyświetlacz LED - C3</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            padding: 15px;
        }
        .container {
            max-width: 700px;
            margin: 0 auto;
            background: white;
            border-radius: 15px;
            box-shadow: 0 10px 40px rgba(0,0,0,0.3);
            overflow: hidden;
        }
        .header {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            padding: 20px;
            text-align: center;
        }
        .header h1 { font-size: 22px; font-weight: 600; }
        .header small { display: block; margin-top: 5px; opacity: 0.9; font-size: 12px; }
        .content { padding: 20px; }
        .section { margin-bottom: 20px; }
        .section-title {
            font-size: 14px;
            font-weight: 600;
            color: #666;
            margin-bottom: 10px;
            text-transform: uppercase;
        }
        
        /* LED DISPLAY VISUALIZATION */
        .led-display-container {
            background: #1a1a1a;
            border-radius: 10px;
            padding: 15px;
            margin-bottom: 20px;
            overflow-x: auto;
        }
        .led-matrix {
            display: inline-grid;
            grid-template-columns: repeat(32, 1fr);
            gap: 2px;
            background: #0a0a0a;
            padding: 10px;
            border-radius: 8px;
            border: 2px solid #333;
        }
        .led {
            width: 8px;
            height: 8px;
            background: #2a2a2a;
            border-radius: 50%;
            transition: all 0.3s;
        }
        .led.on {
            background: #ff4444;
            box-shadow: 0 0 8px #ff4444, 0 0 12px #ff0000;
        }
        .display-text {
            text-align: center;
            color: #999;
            font-size: 12px;
            margin-top: 10px;
        }
        
        textarea {
            width: 100%;
            min-height: 80px;
            padding: 12px;
            border: 2px solid #e0e0e0;
            border-radius: 10px;
            font-size: 16px;
            resize: vertical;
            font-family: inherit;
        }
        textarea:focus { outline: none; border-color: #667eea; }
        .char-count { text-align: right; color: #999; font-size: 13px; margin-top: 5px; }
        .btn-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 10px; }
        .btn-grid.three-col { grid-template-columns: repeat(3, 1fr); }
        .btn-grid.four-col { grid-template-columns: repeat(4, 1fr); }
        button {
            padding: 15px;
            font-size: 15px;
            font-weight: 600;
            border: none;
            border-radius: 10px;
            cursor: pointer;
            transition: all 0.3s;
            text-transform: uppercase;
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
        }
        button:active { transform: translateY(2px); }
        .btn-send { background: #4CAF50; color: white; }
        .btn-stop { background: #ff9800; color: white; }
        .btn-clear { background: #f44336; color: white; }
        .btn-date { background: #2196F3; color: white; }
        .btn-weather { background: #00BCD4; color: white; }
        .btn-save { background: #9C27B0; color: white; }
        .slider-box {
            background: #f8f9fa;
            padding: 15px;
            border-radius: 10px;
            border: 2px solid #e0e0e0;
            margin-bottom: 15px;
        }
        .slider-header {
            display: flex;
            justify-content: space-between;
            margin-bottom: 10px;
        }
        .slider-value {
            background: #667eea;
            color: white;
            padding: 4px 12px;
            border-radius: 20px;
            font-weight: 600;
        }
        input[type="range"] {
            width: 100%;
            height: 6px;
            background: #ddd;
            outline: none;
            -webkit-appearance: none;
        }
        input[type="range"]::-webkit-slider-thumb {
            -webkit-appearance: none;
            width: 24px;
            height: 24px;
            border-radius: 50%;
            background: #667eea;
            cursor: pointer;
        }
        .status {
            padding: 12px;
            border-radius: 10px;
            text-align: center;
            font-weight: 600;
            display: none;
            margin-top: 15px;
        }
        .status.show { display: block; }
        .status.success { background: #d4edda; color: #155724; }
        .info-box {
            background: #e3f2fd;
            padding: 12px;
            border-radius: 10px;
            border-left: 4px solid #2196F3;
            font-size: 13px;
            color: #0d47a1;
        }
        .power-indicator {
            display: inline-block;
            width: 12px;
            height: 12px;
            border-radius: 50%;
            background: #4CAF50;
            margin-right: 8px;
            animation: pulse 2s infinite;
        }
        .power-indicator.off {
            background: #f44336;
        }
        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.5; }
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>🔤 Wyświetlacz LED</h1>
            <small>ESP32-C3 SuperMini + MAX7219</small>
        </div>
        
        <div class="content">
            <!-- LED DISPLAY VISUALIZATION -->
            <div class="section">
                <div class="section-title">
                    <span class="power-indicator" id="powerIndicator"></span>
                    Podgląd wyświetlacza
                </div>
                <div class="led-display-container">
                    <div class="led-matrix" id="ledMatrix"></div>
                    <div class="display-text" id="displayText">Oczekiwanie na dane...</div>
                </div>
            </div>
            
            <div class="section">
                <div class="section-title">Tekst do wyświetlenia</div>
                <textarea id="textInput" maxlength="250" placeholder="Wpisz tekst..."></textarea>
                <div class="char-count"><span id="charCount">0</span> / 250</div>
                
                <div class="btn-grid four-col">
                    <button class="btn-send" onclick="sendText()">Wyślij</button>
                    <button class="btn-stop" onclick="stopDisplay()">Stop</button>
                    <button class="btn-clear" onclick="clearText()">Wyczyść</button>
                    <button class="btn-date" onclick="showDateNow()">📅 Data</button>
                </div>
                <div class="btn-grid" style="margin-top: 10px;">
                    <button class="btn-weather" onclick="showWeatherNow()">🌤️ Pogoda</button>
                    <button class="btn-save" onclick="saveSettings()">💾 Zapisz</button>
                </div>
            </div>
            
            <div class="section">
                <div class="section-title">Ustawienia</div>
                <div class="slider-box">
                    <div class="slider-header">
                        <span>⚡ Prędkość</span>
                        <span class="slider-value" id="speedValue">50</span>
                    </div>
                    <input type="range" id="speedSlider" min="10" max="150" value="50" oninput="updateSpeed(this.value)">
                </div>
                
                <div class="slider-box">
                    <div class="slider-header">
                        <span>💡 Jasność</span>
                        <span class="slider-value" id="brightnessValue">5</span>
                    </div>
                    <input type="range" id="brightnessSlider" min="0" max="15" value="5" oninput="updateBrightness(this.value)">
                </div>
            </div>
            
            <div id="status" class="status"></div>
            
            <div class="section">
                <div class="info-box">
                    ℹ️ ESP32-C3 SuperMini<br>
                    Piny: CLK=2, DATA=7, CS=10<br>
                    MQTT: broker.hivemq.com<br>
                    Stan ON/OFF zapisywany w pamięci
                </div>
            </div>
        </div>
    </div>

    <script>
        const textInput = document.getElementById('textInput');
        const charCount = document.getElementById('charCount');
        const statusDiv = document.getElementById('status');
        const ledMatrix = document.getElementById('ledMatrix');
        const displayText = document.getElementById('displayText');
        const powerIndicator = document.getElementById('powerIndicator');

        // Initialize LED matrix (8 rows x 32 columns)
        for (let i = 0; i < 8 * 32; i++) {
            const led = document.createElement('div');
            led.className = 'led';
            ledMatrix.appendChild(led);
        }

        textInput.addEventListener('input', function() {
            charCount.textContent = this.value.length;
        });

        function showStatus(message, isSuccess) {
            statusDiv.textContent = message;
            statusDiv.className = 'status show ' + (isSuccess ? 'success' : 'error');
            setTimeout(() => { statusDiv.className = 'status'; }, 3000);
        }

        function updateLEDDisplay(text) {
            displayText.textContent = text;
            // Simple text visualization
            const leds = ledMatrix.querySelectorAll('.led');
            leds.forEach(led => led.classList.remove('on'));
            
            // Light up LEDs to show text is displaying
            if (text && text !== 'Oczekiwanie na dane...') {
                for (let i = 0; i < Math.min(text.length * 5, 256); i++) {
                    const randomLed = leds[Math.floor(Math.random() * leds.length)];
                    randomLed.classList.add('on');
                }
            }
        }

        function sendText() {
            const text = textInput.value;
            if (text.trim() === '') { 
                showStatus('⚠️ Wpisz tekst!', false); 
                return; 
            }
            fetch('/send', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: 'text=' + encodeURIComponent(text)
            }).then(() => {
                showStatus('✓ Wysłano!', true);
                updateLEDDisplay(text);
                powerIndicator.classList.remove('off');
            });
        }

        function stopDisplay() {
            fetch('/stop').then(() => {
                showStatus('⏸ Stop', true);
                updateLEDDisplay('---');
                const leds = ledMatrix.querySelectorAll('.led');
                leds.forEach(led => led.classList.remove('on'));
            });
        }

        function clearText() {
            textInput.value = '';
            charCount.textContent = '0';
        }

        function showDateNow() {
            fetch('/showdate').then(() => {
                showStatus('📅 Data', true);
                updateLEDDisplay('Data');
                powerIndicator.classList.remove('off');
            });
        }

        function showWeatherNow() {
            fetch('/showweather').then(() => {
                showStatus('🌤️ Pogoda', true);
                updateLEDDisplay('Pogoda');
                powerIndicator.classList.remove('off');
            });
        }

        function saveSettings() {
            fetch('/savesettings').then(() => showStatus('💾 Zapisano!', true));
        }

        function updateSpeed(value) {
            document.getElementById('speedValue').textContent = value;
            fetch('/speed?value=' + value);
        }

        function updateBrightness(value) {
            document.getElementById('brightnessValue').textContent = value;
            fetch('/brightness?value=' + value);
            
            // Update LED brightness visually
            const leds = ledMatrix.querySelectorAll('.led.on');
            const opacity = value / 15;
            leds.forEach(led => {
                led.style.opacity = Math.max(0.3, opacity);
            });
        }

        // Fetch current settings on load
        window.onload = function() {
            fetch('/settings')
            .then(response => response.json())
            .then(data => {
                document.getElementById('speedSlider').value = data.speed;
                document.getElementById('brightnessSlider').value = data.brightness;
                document.getElementById('speedValue').textContent = data.speed;
                document.getElementById('brightnessValue').textContent = data.brightness;
            });
            
            // Simulate time display on startup
            updateTimeDisplay();
            setInterval(updateTimeDisplay, 1000);
        }

        function updateTimeDisplay() {
            const now = new Date();
            const hours = String(now.getHours()).padStart(2, '0');
            const minutes = String(now.getMinutes()).padStart(2, '0');
            const time = hours + ':' + minutes;
            
            // Don't override if user sent text
            if (displayText.textContent === 'Oczekiwanie na dane...' || 
                displayText.textContent.includes(':')) {
                updateLEDDisplay(time);
                
                // Create clock pattern
                const leds = ledMatrix.querySelectorAll('.led');
                leds.forEach(led => led.classList.remove('on'));
                
                // Simple clock visualization - center area
                const pattern = [
                    100, 101, 102, 103, 104,
                    132, 135,
                    164, 167,
                    100, 135
                ];
                
                pattern.forEach(index => {
                    if (leds[index]) leds[index].classList.add('on');
                });
            }
        }

        // Auto-refresh display status every 5 seconds
        setInterval(() => {
            fetch('/status').then(response => response.text()).then(data => {
                if (data.includes('OFF')) {
                    powerIndicator.classList.add('off');
                } else {
                    powerIndicator.classList.remove('off');
                }
            }).catch(() => {});
        }, 5000);
    </script>
</body>
</html>
)rawliteral";

// ============================================================
// HANDLERY SERWERA
// ============================================================

void handleRoot() {
  server.send(200, "text/html", index_html);
}

void handleSend() {
  if (server.hasArg("text")) {
    if (!displayEnabled) {
      server.send(400, "text/plain", "Wyswietlacz wylaczony");
      return;
    }
    
    displayText = server.arg("text");
    displayingUserText = true;
    scrolling = true;
    waitingAfterStop = false;
    isScrollingDate = false;
    isScrollingWeather = false;
    weatherPending = false;
    myDisplay.displayClear();
    myDisplay.setSpeed(scrollSpeed);
    myDisplay.displayText(displayText.c_str(), PA_CENTER, scrollSpeed, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Brak tekstu");
  }
}

void handleStop() {
  displayingUserText = false;
  scrolling = false;
  isScrollingDate = false;
  isScrollingWeather = false;
  weatherPending = false;
  waitingAfterStop = true;
  stopTime = millis();
  myDisplay.displayClear();
  server.send(200, "text/plain", "OK");
}

void handleShowDate() {
  if (!displayEnabled) {
    server.send(400, "text/plain", "Wyswietlacz wylaczony");
    return;
  }
  
  displayingUserText = false;
  scrolling = false;
  waitingAfterStop = false;
  isScrollingWeather = false;
  weatherPending = false;
  
  if (dateDisplayMode == 0) {
    isScrollingDate = false;
    currentState = STATE_DAYNAME;
    lastSwitch = millis();
    String dayName = getDayNameShort();
    myDisplay.displayClear();
    myDisplay.setTextAlignment(PA_CENTER);
    myDisplay.print(dayName.c_str());
  } else {
    String fullDate = getDateFull();
    fullDate.toCharArray(scrollBuffer, sizeof(scrollBuffer));
    isScrollingDate = true;
    currentState = STATE_TIME;
    lastSwitch = millis();
    myDisplay.displayClear();
    myDisplay.displayText(scrollBuffer, PA_CENTER, scrollSpeed, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  }
  server.send(200, "text/plain", "OK");
}

void handleShowWeather() {
  if (!displayEnabled) {
    server.send(400, "text/plain", "Wyswietlacz wylaczony");
    return;
  }
  
  displayingUserText = false;
  scrolling = false;
  waitingAfterStop = false;
  isScrollingDate = false;
  weatherPending = false;
  
  fetchWeather();
  startWeatherDisplay();
  lastWeatherUpdate = millis();
  
  server.send(200, "text/plain", "OK");
}

void handleSpeed() {
  if (server.hasArg("value")) {
    scrollSpeed = server.arg("value").toInt();
    myDisplay.setSpeed(scrollSpeed);
    server.send(200, "text/plain", "OK");
  }
}

void handleBrightness() {
  if (server.hasArg("value")) {
    displayIntensity = server.arg("value").toInt();
    if (displayEnabled) {
      myDisplay.setIntensity(displayIntensity);
    }
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Brak wartosci");
  }
}

void handleSaveSettings() {
  saveSettings();
  server.send(200, "text/plain", "OK");
}

void handleSettings() {
  String json = "{\"speed\":" + String(scrollSpeed) + 
                ",\"brightness\":" + String(displayIntensity) + "}";
  server.send(200, "application/json", json);
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=================================");
  Serial.println("ESP32-C3 SuperMini LED Display");
  Serial.println("=================================");
  
  // Generuj unikalny Client ID z MAC
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(mqtt_client_id, sizeof(mqtt_client_id), "%s%02X%02X%02X", 
           mqtt_client_id_base.c_str(), mac[3], mac[4], mac[5]);
  Serial.print("MQTT Client ID: ");
  Serial.println(mqtt_client_id);
  
  loadSettings(); // Wczytuje też displayEnabled!
  
  Serial.println("Inicjalizacja wyswietlacza...");
  Serial.printf("Piny: CLK=%d, DATA=%d, CS=%d\n", CLK_PIN, DATA_PIN, CS_PIN);
  
  myDisplay.begin();
  myDisplay.setIntensity(displayIntensity);
  myDisplay.setSpeed(scrollSpeed);
  myDisplay.displayClear();
  myDisplay.setTextAlignment(PA_CENTER);
  myDisplay.print("START");
  
  // ZASTOSUJ ZAPISANY STAN ON/OFF
  delay(1000);
  if (!displayEnabled) {
    myDisplay.displayShutdown(true);
    Serial.println(">>> Przywrocono stan: WYLACZONY");
  } else {
    Serial.println(">>> Przywrocono stan: WLACZONY");
  }
  
  delay(1000);
  
  // WiFi AP
  Serial.println("Uruchamiam Access Point...");
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_password);
  IPAddress apIP = WiFi.softAPIP();
  
  Serial.println("[OK] AP: " + String(ap_ssid));
  Serial.println("IP AP: " + apIP.toString());
  
  if (displayEnabled) {  // Pokaż IP tylko jeśli włączony
    myDisplay.displayClear();
    myDisplay.print(apIP.toString().c_str());
    delay(2000);
  }
  
  // WiFi STA
  Serial.println("Laczenie z WiFi...");
  WiFi.begin(sta_ssid, sta_password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[OK] WiFi polaczone");
    Serial.println("IP STA: " + WiFi.localIP().toString());
    
    // Konfiguracja czasu
    configTzTime(timeZone, ntpServer, ntpServer2);
    delay(2000);
    
    // Pobierz pogodę
    fetchWeather();
    
    // Inicjalizacja MQTT
    Serial.println("Inicjalizacja MQTT...");
    mqttClient.setServer(mqtt_server, mqtt_port);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(60);
    mqttClient.setSocketTimeout(10);
    mqttReconnect();
  } else {
    Serial.println("\n[BLAD] Nie mozna polaczyc z WiFi");
  }
  
  // Serwer WWW
  server.on("/", handleRoot);
  server.on("/send", HTTP_POST, handleSend);
  server.on("/stop", handleStop);
  server.on("/showdate", handleShowDate);
  server.on("/showweather", handleShowWeather);
  server.on("/speed", handleSpeed);
  server.on("/brightness", handleBrightness);
  server.on("/savesettings", handleSaveSettings);
  server.on("/settings", handleSettings);
  server.begin();
  
  Serial.println("[OK] Serwer WWW: http://" + apIP.toString());
  Serial.println("[OK] MQTT topic: " + String(mqtt_topic_cmd));
  Serial.println("=================================\n");
  
  if (displayEnabled) {
    myDisplay.displayClear();
  }
  lastSwitch = millis();
  lastColonBlink = millis();
  lastWeatherUpdate = millis();
}

// ============================================================
// LOOP
// ============================================================

void loop() {
  // Obsługa MQTT
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      static unsigned long lastReconnectAttempt = 0;
      unsigned long now = millis();
      if (now - lastReconnectAttempt > 15000) { // Próba co 15s
        lastReconnectAttempt = now;
        mqttReconnect();
      }
    } else {
      mqttClient.loop();
    }
  }
  
  server.handleClient();
  
  // JEŚLI WYŚWIETLACZ WYŁĄCZONY - NIC NIE RÓB
  if (!displayEnabled) {
    delay(100);
    return;
  }
  
  unsigned long currentMillis = millis();
  
  // Mrugający dwukropek
  if (currentMillis - lastColonBlink >= COLON_BLINK_INTERVAL) {
    lastColonBlink = currentMillis;
    colonVisible = !colonVisible;
  }
  
  // Automatyczne pobieranie pogody co 3 minuty
  if (currentMillis - lastWeatherUpdate >= WEATHER_UPDATE_INTERVAL && !displayingUserText) {
    Serial.println("=== Aktualizacja pogody ===");
    fetchWeather();
    lastWeatherUpdate = currentMillis;
    
    bool isDisplayingDate = (currentState == STATE_DAYNAME || 
                             currentState == STATE_DATE || 
                             isScrollingDate);
    
    if (isDisplayingDate) {
      weatherPending = true;
      Serial.println(">>> Pogoda czeka...");
    } else {
      startWeatherDisplay();
    }
  }
  
  // Sprawdź czy pogoda czeka
  if (weatherPending && currentState == STATE_TIME && !isScrollingDate && !isScrollingWeather) {
    Serial.println(">>> Wyswietlam pogode!");
    startWeatherDisplay();
  }
  
  // STOP
  if (waitingAfterStop && (currentMillis - stopTime > 1000)) {
    waitingAfterStop = false;
    lastSwitch = currentMillis;
    currentState = STATE_TIME;
    isScrollingDate = false;
    isScrollingWeather = false;
    weatherPending = false;
  }
  
  // Tekst użytkownika
  if (displayingUserText && scrolling) {
    if (myDisplay.displayAnimate()) {
      myDisplay.displayReset();
    }
  }
  // Scrollowanie pogody
  else if (isScrollingWeather) {
    if (myDisplay.displayAnimate()) {
      myDisplay.displayReset();
      isScrollingWeather = false;
      currentState = STATE_TIME;
      lastSwitch = currentMillis;
      myDisplay.displayClear();
    }
  }
  // Scrollowanie daty
  else if (isScrollingDate) {
    if (myDisplay.displayAnimate()) {
      myDisplay.displayReset();
      isScrollingDate = false;
      currentState = STATE_TIME;
      lastSwitch = currentMillis;
      myDisplay.displayClear();
    }
  }
  // Normalny tryb zegara/daty
  else if (!displayingUserText && !waitingAfterStop && !isScrollingDate && !isScrollingWeather) {
    if (dateDisplayMode == 0) {
      // NIERUCHOMA
      if (currentState == STATE_TIME && (currentMillis - lastSwitch >= TIME_DISPLAY_DURATION)) {
        currentState = STATE_DAYNAME;
        lastSwitch = currentMillis;
        String dayName = getDayNameShort();
        myDisplay.displayClear();
        myDisplay.setTextAlignment(PA_CENTER);
        myDisplay.print(dayName.c_str());
      }
      else if (currentState == STATE_DAYNAME && (currentMillis - lastSwitch >= DAY_DISPLAY_DURATION)) {
        currentState = STATE_DATE;
        lastSwitch = currentMillis;
        String dateStr = getDateShort();
        myDisplay.displayClear();
        myDisplay.setTextAlignment(PA_CENTER);
        myDisplay.print(dateStr.c_str());
      }
      else if (currentState == STATE_DATE && (currentMillis - lastSwitch >= DATE_DISPLAY_DURATION)) {
        currentState = STATE_TIME;
        lastSwitch = currentMillis;
      }
      
      if (currentState == STATE_TIME) {
        static bool lastColonState = true;
        if (lastColonState != colonVisible) {
          lastColonState = colonVisible;
          String timeStr = getTimeString(colonVisible);
          myDisplay.displayClear();
          myDisplay.setTextAlignment(PA_CENTER);
          myDisplay.print(timeStr.c_str());
        }
      }
    }
    else {
      // SCROLLOWANA
      if (currentMillis - lastSwitch >= TIME_DISPLAY_DURATION) {
        String fullDate = getDateFull();
        fullDate.toCharArray(scrollBuffer, sizeof(scrollBuffer));
        isScrollingDate = true;
        lastSwitch = currentMillis;
        myDisplay.displayClear();
        myDisplay.displayText(scrollBuffer, PA_CENTER, scrollSpeed, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
      }
      else {
        static bool lastColonState = true;
        if (lastColonState != colonVisible) {
          lastColonState = colonVisible;
          String timeStr = getTimeString(colonVisible);
          myDisplay.displayClear();
          myDisplay.setTextAlignment(PA_CENTER);
          myDisplay.print(timeStr.c_str());
        }
      }
    }
  }
  
  delay(10);
}