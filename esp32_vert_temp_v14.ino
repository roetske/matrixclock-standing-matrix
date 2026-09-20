/*
  ESP32 LantaarnKlok - v12.7
  - WiFiManager voor netwerkinstelling (192.168.4.1)
  - NTP Tijdssynchronisatie
  - DS18B20 Dallas Temperatuursensor op GPIO 16 (Async / Non-blocking)
  - Buzzer op GPIO 4 (0.5s AAN / 0.5s UIT richtingaanwijzer-ritme)
  - Drukknop op GPIO 13 (Kort = Alarm stoppen | >5 sec indrukken = Alarm deactiveren/inschakelen)
  - Hardware Task Watchdog Timer
  - VISUELE ALARMINDICATOR: Kolom 0 op Blok 0
  - FIX v12.7: Formulierverwerking gecorrigeerd. 'Instellingen Opslaan' en vinkje 
    sturen beide betrouwbaar de tijd + alarmstatus naar /save met automatische herlaad.
*/

#include <MD_MAX72xx.h>
#include <SPI.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "time.h"
#include <esp_task_wdt.h>

String version = "v14 (Form Save & Checkbox Fix)";

// --- HARDWARE CONFIGURATIE ---
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW 
#define MAX_DEVICES 4 

#define DATA_PIN       23  
#define CLK_PIN        18  
#define CS_PIN         5   
#define BUZZER_PIN     4   
#define ONE_WIRE_BUS   16  
#define BUTTON_PIN     13  

#define WATCHDOG_TIMEOUT_SEC 8 

// --- TIMING & ALARM CONFIGURATIE ---
#define BUZZER_INTERVAL_MS     500    
#define TEMP_INTERVAL_MS     10000    
#define TEMP_WEERGAVE_START     45    
#define TEMP_WEERGAVE_DUUR       5    
#define LONG_PRESS_TIME_MS    5000    

MD_MAX72XX mx = MD_MAX72XX(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
WebServer server(80);
Preferences preferences;

// --- DALLAS TEMPERATUUR SETUP ---
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
float huidigeTemperatuur = -127.0;
unsigned long laatsteTempMeting = 0;
bool tempAangevraagd = false;

// --- NTP TIJD INSTELLINGEN ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;      
const int   daylightOffset_sec = 3600; 

// --- ALARM VARIABELEN ---
int alarmUur = 7;
int alarmMinuut = 0;
bool alarmActief = false;
bool alarmAfgaan = false;
int afgegaanMinuut = -1;
unsigned long alarmStartTijd = 0;

// KNOP METING VARIABELEN
unsigned long knopIndrukTijd = 0;
bool knopWasIngedrukt = false;
bool langIngedruktGehandhaafd = false;

bool vorigSchermWasTemp = false;

// Cijfers (0 t/m 9)
const uint64_t PROGMEM cijfersHex[] = {
  0x003c666e7666663c, 0x001818381818187e, 0x003c66060c30607e, 0x003c66061c06663c, 
  0x000c1c2c4c7e0c0c, 0x007e607c0606663c, 0x003c66607c66663c, 0x007e660c0c181818, 
  0x003c66663c66663c, 0x003c66663e06663c
};

const uint64_t PROGMEM cijfersMetDotHex[] = {
  0x813c666e7666663c, 0x811818381818187e, 0x813c66060c30607e, 0x813c66061c06663c, 
  0x810c1c2c4c7e0c0c, 0x817e607c0606663c, 0x813c66607c66663c, 0x817e660c0c181818, 
  0x813c66663c66663c, 0x813c66663e06663c
};

const uint64_t PROGMEM letterCHex = 0x003c66606060663c;
const uint64_t PROGMEM leegHex = 0x0000000000000000;

String getResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  switch (reason) {
    case ESP_RST_POWERON:   return "Power-On / Reset Knop";
    case ESP_RST_SW:        return "Software Reset (ESP.restart)";
    case ESP_RST_PANIC:     return "Software Crash (Panic / Exception)";
    case ESP_RST_INT_WDT:   return "Interrupt Watchdog Reset";
    case ESP_RST_TASK_WDT:  return "Task Watchdog Reset (System Hung)";
    case ESP_RST_WDT:       return "Other Watchdog Reset";
    case ESP_RST_DEEPSLEEP: return "Deep Sleep Wakeup";
    case ESP_RST_BROWNOUT:  return "Brownout Reset (Spanningsdip!)";
    default:                return "Onbekende oorzaak";
  }
}

void toonBitmapOpBlok(uint8_t deviceId, uint64_t image) {
  for (uint8_t col = 0; col < 8; col++) {
    uint8_t kolomData = (image >> (col * 8)) & 0xFF;
    mx.setColumn(deviceId, col, kolomData);
  }
}

void toonCijferOpBlok(uint8_t deviceId, uint8_t getal, bool metDot) {
  uint64_t image;
  if (metDot) {
    memcpy_P(&image, &cijfersMetDotHex[getal], sizeof(uint64_t));
  } else {
    memcpy_P(&image, &cijfersHex[getal], sizeof(uint64_t));
  }
  
  if (deviceId == 0 && alarmActief) {
    image |= 0x00000000000000FFULL; 
  }

  toonBitmapOpBlok(deviceId, image);
}

void toonStatusLeds(uint8_t b3, uint8_t b2, uint8_t b1, uint8_t b0) {
  toonCijferOpBlok(3, b3, false);
  toonCijferOpBlok(2, b2, false);
  toonCijferOpBlok(1, b1, false);
  toonCijferOpBlok(0, b0, false);
  mx.update();
}

// JSON STATUS ENDPOINT VOOR AUTOMATISCHE WEBPAGINA SYNC
void handleStatus() {
  String tempStr = (huidigeTemperatuur > -50.0 && huidigeTemperatuur < 125.0) ? String(huidigeTemperatuur, 1) : "N/A";
  
  String json = "{";
  json += "\"actief\":" + String(alarmActief ? "true" : "false") + ",";
  json += "\"afgaan\":" + String(alarmAfgaan ? "true" : "false") + ",";
  json += "\"uur\":" + String(alarmUur) + ",";
  json += "\"min\":" + String(alarmMinuut) + ",";
  json += "\"temp\":\"" + tempStr + "\"";
  json += "}";
  
  server.send(200, "application/json", json);
}

// INSTELLINGEN OPSLAAN
void handleSave() {
  if (server.method() == HTTP_POST) {
    if (server.hasArg("stop")) {
      alarmAfgaan = false;
      digitalWrite(BUZZER_PIN, LOW);
      Serial.println("[WEB] Alarm gestopt via webpagina.");
    }

    if (server.hasArg("tijd")) {
      String tijdVal = server.arg("tijd");
      if (tijdVal.length() >= 5) {
        alarmUur = tijdVal.substring(0, 2).toInt();
        alarmMinuut = tijdVal.substring(3, 5).toInt();
      }
      
      alarmActief = server.hasArg("actief");
      
      preferences.putInt("uur", alarmUur);
      preferences.putInt("min", alarmMinuut);
      preferences.putBool("actief", alarmActief);

      Serial.printf("[WEB] Instellingen opgeslagen: %02d:%02d (Actief: %s)\n", 
                    alarmUur, alarmMinuut, alarmActief ? "JA" : "NEE");
    }
  }
  // Redirect naar hoofdpagina om herhaalde posts te voorkomen
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>"; 
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:Arial, sans-serif; text-align:center; margin-top:20px; background:#111; color:#fff;}";
  html += "input, button{font-size:18px; padding:12px; margin:10px; border-radius:8px;}";
  html += ".btn-stop{background-color:#e74c3c; color:white; border:none; width:80%; font-weight:bold; cursor:pointer;}";
  html += ".btn-save{background-color:#2ecc71; color:white; border:none; cursor:pointer;}";
  html += ".info-box{background:#222; padding:12px; border-radius:8px; display:inline-block; margin-bottom:12px; min-width:250px;}";
  html += ".network-info{font-size:14px; color:#aaa; margin-top:4px;}";
  html += "</style>";

  html += "<script>";
  html += "let isEditing = false;";
  html += "function updateStatus(){";
  html += "  if(isEditing) return;";
  html += "  fetch('/status').then(r => r.json()).then(d => {";
  html += "    document.getElementById('chkActief').checked = d.actief;";
  html += "    document.getElementById('tempVal').innerText = d.temp + ' °C';";
  html += "    document.getElementById('stopDiv').style.display = d.afgaan ? 'block' : 'none';";
  html += "  });";
  html += "}";
  html += "setInterval(updateStatus, 2000);"; 
  html += "</script></head><body>";
  
  html += "<h2>LantaarnKlok Status</h2>";

  // Temperatuur Weergave
  html += "<div class='info-box'><h3>Temperatuur: <span id='tempVal'>";
  if (huidigeTemperatuur > -50.0 && huidigeTemperatuur < 125.0) {
    html += String(huidigeTemperatuur, 1) + " &deg;C";
  } else {
    html += "Meting bezig...";
  }
  html += "</span></h3></div><br>";

  // Netwerk Info
  html += "<div class='info-box'><div class='network-info'>";
  html += "<strong>WiFi Netwerk:</strong> " + WiFi.SSID() + "<br>";
  html += "<strong>IP-Adres:</strong> " + WiFi.localIP().toString();
  html += "</div></div><br>";

  // Alarm Stop Knop
  html += "<div id='stopDiv' style='display:" + String(alarmAfgaan ? "block" : "none") + ";'>";
  html += "<form action='/save' method='POST'>";
  html += "<button class='btn-stop' type='submit' name='stop' value='1'>ALARM STOPPEN</button>";
  html += "</form><hr style='border-color:#333;'></div>";

  // Hoofdformulier voor Instellingen
  html += "<form action='/save' method='POST'>";
  
  char tijdStr[6];
  sprintf(tijdStr, "%02d:%02d", alarmUur, alarmMinuut);
  
  html += "<label>Alarm Tijd:</label><br>";
  html += "<input type='time' name='tijd' value='" + String(tijdStr) + "' onfocus='isEditing=true' onblur='isEditing=false' required><br><br>";
  
  html += "<input type='checkbox' id='chkActief' name='actief' " + String(alarmActief ? "checked" : "") + "> Alarm Ingeschakeld<br><br>";
  
  html += "<button class='btn-save' type='submit'>Instellingen Opslaan</button>";
  html += "</form></body></html>";

  server.send(200, "text/html; charset=utf-8", html);
}

void setup() {
  Serial.begin(9600);
  delay(500);

  Serial.println("\n==========================================");
  Serial.println("   ESP32 LANTAARNKLOK - BOOT DIAGNOSTICS");
  Serial.println("   " + version);
  Serial.println("==========================================");
  
  WiFi.mode(WIFI_STA);
  
  Serial.printf("[BOOT] Reset Oorzaak:   %s\n", getResetReason().c_str());
  Serial.printf("[BOOT] CPU Frequentie:  %d MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("[BOOT] Vrij Geheugen:   %d Bytes\n", ESP.getFreeHeap());
  Serial.printf("[BOOT] MAC Adres:       %s\n", WiFi.macAddress().c_str());

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  digitalWrite(BUZZER_PIN, HIGH);
  delay(1000);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  sensors.begin();
  sensors.setWaitForConversion(false);

  mx.begin();
  mx.control(MD_MAX72XX::INTENSITY, 2); 
  mx.clear();
  toonStatusLeds(0, 0, 0, 0);

  preferences.begin("alarm", false);
  alarmUur = preferences.getInt("uur", 7);
  alarmMinuut = preferences.getInt("min", 0);
  alarmActief = preferences.getBool("actief", false);

  WiFiManager wifiManager;
  if (!wifiManager.autoConnect("LantaarnKlok_AP")) {
    delay(10000);
    ESP.restart();
  }

  Serial.println("[WIFI] Succesvol verbonden!");
  Serial.printf("[WIFI] IP-Adres: %s\n", WiFi.localIP().toString().c_str());

  if (MDNS.begin("lantaarnklok")) {
    Serial.println("[mDNS] Gestart op http://lantaarnklok.local");
  }

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.on("/status", handleStatus);
  server.begin();

  sensors.requestTemperatures();
  laatsteTempMeting = millis();
  tempAangevraagd = true;

#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t wdt_config = {
      .timeout_ms = WATCHDOG_TIMEOUT_SEC * 1000,
      .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
      .trigger_panic = true
  };
  esp_task_wdt_reconfigure(&wdt_config);
  esp_task_wdt_add(NULL);
#else
  esp_task_wdt_init(WATCHDOG_TIMEOUT_SEC, true);
  esp_task_wdt_add(NULL);
#endif

  Serial.println("==========================================\n");
}

void loop() {
  esp_task_wdt_reset();
  server.handleClient();

  // --- ASYNCHRONE TEMPERATUUR AFHANDELING ---
  if (millis() - laatsteTempMeting >= TEMP_INTERVAL_MS && !tempAangevraagd) {
    sensors.requestTemperatures();
    laatsteTempMeting = millis();
    tempAangevraagd = true;
  }

  if (tempAangevraagd && (millis() - laatsteTempMeting >= 1000UL)) {
    float tempRead = sensors.getTempCByIndex(0);
    if (tempRead > -50.0 && tempRead < 125.0) {
      huidigeTemperatuur = tempRead;
    }
    tempAangevraagd = false;
  }

  // --- DRUKKNOP LOGICA (HARDWARE IS MASTER) ---
  int knopStatus = digitalRead(BUTTON_PIN);

  if (knopStatus == LOW) { 
    if (!knopWasIngedrukt) {
      knopIndrukTijd = millis();
      knopWasIngedrukt = true;
      langIngedruktGehandhaafd = false;
    } 
    else if (!langIngedruktGehandhaafd && (millis() - knopIndrukTijd >= LONG_PRESS_TIME_MS)) {
      alarmActief = !alarmActief; 
      preferences.putBool("actief", alarmActief); 
      
      if (alarmAfgaan) {
        alarmAfgaan = false;
        digitalWrite(BUZZER_PIN, LOW);
      }

      if (alarmActief) {
        digitalWrite(BUZZER_PIN, HIGH); delay(100); digitalWrite(BUZZER_PIN, LOW); delay(100);
        digitalWrite(BUZZER_PIN, HIGH); delay(100); digitalWrite(BUZZER_PIN, LOW);
      } else {
        digitalWrite(BUZZER_PIN, HIGH); delay(600); digitalWrite(BUZZER_PIN, LOW);
      }

      Serial.printf("[BUTTON] Alarm via knop 5s %sgeschakeld!\n", alarmActief ? "IN" : "UIT");

      langIngedruktGehandhaafd = true; 
    }
  } 
  else { 
    if (knopWasIngedrukt) {
      if (!langIngedruktGehandhaafd && alarmAfgaan) {
        alarmAfgaan = false;
        digitalWrite(BUZZER_PIN, LOW);
        Serial.println("[BUTTON] Alarm gestopt via korte knopdruk.");
      }
      
      knopWasIngedrukt = false;
      langIngedruktGehandhaafd = false;
    }
  }

  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    toonStatusLeds(1, 1, 1, 1);
    delay(1000);
    return;
  }

  uint8_t uren    = timeinfo.tm_hour;
  uint8_t minuten  = timeinfo.tm_min;
  uint8_t seconden = timeinfo.tm_sec;

  // --- ALARM TRIGGER ---
  if (alarmActief && uren == alarmUur && minuten == alarmMinuut && afgegaanMinuut != minuten && !alarmAfgaan) {
    alarmAfgaan = true;
    alarmStartTijd = millis();
    afgegaanMinuut = minuten;
  }

  // --- ALARM PIEP LOGICA ---
  if (alarmAfgaan) {
    if (millis() - alarmStartTijd >= 1200000UL) { 
      alarmAfgaan = false;
      digitalWrite(BUZZER_PIN, LOW);
    } else {
      if ((millis() / BUZZER_INTERVAL_MS) % 2 == 0) {
        digitalWrite(BUZZER_PIN, HIGH);
      } else {
        digitalWrite(BUZZER_PIN, LOW);
      }
    }
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }

  // --- DISPLAY WEERGAVE ---
  if (seconden >= TEMP_WEERGAVE_START && seconden < (TEMP_WEERGAVE_START + TEMP_WEERGAVE_DUUR) && huidigeTemperatuur > -50.0 && huidigeTemperatuur < 125.0) {
    int tempAfgerond = (int)round(huidigeTemperatuur);
    if (tempAfgerond < 0) tempAfgerond = 0;

    toonCijferOpBlok(3, tempAfgerond / 10, false); 
    toonCijferOpBlok(2, tempAfgerond % 10, false);  
    toonBitmapOpBlok(1, letterCHex);               
    
    uint64_t blok0Image = alarmActief ? 0x00000000000000FFULL : leegHex;
    toonBitmapOpBlok(0, blok0Image);
  } 
  else {
    // Normale Tijdweergave (HH:MM)
    toonCijferOpBlok(3, uren / 10, false);
    toonCijferOpBlok(2, uren % 10, false);
    toonCijferOpBlok(1, minuten / 10, (seconden % 2 == 0)); 
    toonCijferOpBlok(0, minuten % 10, false);
  }

  mx.update(); 
  delay(150);  
}