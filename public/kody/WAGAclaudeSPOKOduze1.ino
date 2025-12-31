#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "HX711.h"
#include <Preferences.h>
#include <math.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// ====== Piny / Sprzęt ======
const int HX_DOUT_PIN = 4;
const int HX_SCK_PIN  = 5;
const int BTN1_PIN    = 32;
const int BTN2_PIN    = 33;

const int  BUZZER_PIN   = 26;
const bool BEEP_ENABLE  = true;


// OLED SH1106 128x64 I2C (ESP32: SDA=21, SCL=22)
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
//U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

HX711 scale;

// ====== NVS ======
Preferences prefs;
const char* PREF_NAMESPACE = "waga";
const char* PREF_KEY_CAL     = "cal";
const char* PREF_KEY_DIR     = "dir";
const char* PREF_KEY_UNIT    = "unit";
const char* PREF_KEY_RES     = "res";
const char* PREF_KEY_HOLD    = "hold";
const char* PREF_KEY_BRIGHT  = "bright";
const char* PREF_KEY_BEEP    = "beep";
const char* PREF_KEY_ROT     = "rot";
const char* PREF_KEY_MSPEED  = "mspeed";
const char* PREF_KEY_TARA_BEEP = "taraBeep";
const char* PREF_KEY_LARGE_D   = "largeD";

float calibrationFactor = NAN;
int   directionSign     = +1;

// ====== Jednostki / Rozdzielczość ======
enum { UNIT_G = 0, UNIT_DKG = 1, UNIT_KG = 2 };
int   unitMode    = UNIT_G;
bool  fine01gMode = true;

// ====== Auto-Hold ======
unsigned long ahStableMs = 1200;
bool          ahEnabled  = true;
bool          ahActive = false;
bool          ahHasCandidate = false;
long          ahCandidateDeci = 0;
unsigned long ahStartMs = 0;
long          ahValueDeci = 0;

// ====== Filtr / Rysowanie ======
const uint8_t SAMPLES_PER_READ = 1;
const float   IIR_ALPHA         = 0.22f;
const unsigned long DRAW_MIN_INTERVAL_MS = 100;
const unsigned long NOT_READY_TIMEOUT_MS = 2000;

unsigned long lastReadyMs = 0;
unsigned long lastDrawMs  = 0;
bool  haveLastWeight = false;
long  lastWeightDeci = 0;

bool  filterInit = false;
float filteredGrams = 0.0f;

// ====== AP + WWW ======
WebServer server(80);
const char* AP_SSID = "waga";
const char* AP_PASS = "12345678";

// Marquee
const uint8_t* MARQUEE_FONT = u8g2_font_unifont_tr;
char marqueeText[251] = "";
bool marqueeActive = false;
unsigned long marqueeStartMs = 0;
int marqueeX = 0;
int marqueeW = 0;
int marqueeSpeedPx = 2;
const unsigned long MARQUEE_MAX_MS = 60000;

// ====== Kalibracja ======
enum Mode { MODE_NORMAL, MODE_CAL_SELECT, MODE_CAL_TARE, MODE_CAL_PLACE, MODE_CAL_DONE };
Mode mode = MODE_NORMAL;

const int CAL_WEIGHTS[] = {200, 250, 500, 1000, 2000, 5000};
const uint8_t CAL_WEIGHTS_COUNT = sizeof(CAL_WEIGHTS)/sizeof(CAL_WEIGHTS[0]);
uint8_t selWeightIdx = 3;

volatile bool calBusy = false;

// ====== Przyciski ======
enum { BTN_NONE = 0, BTN_SHORT = 1, BTN_LONG = 2 };
const unsigned long DEBOUNCE_MS   = 30;
const unsigned long LONG_PRESS_MS = 1500;
const unsigned long DOUBLE_CLICK_WINDOW_MS = 350;

// ====== OLED ======
int oledBrightness = 255;
int oledRotation   = 0;
bool largeDigitsMode = false;
unsigned long lastActivityMs = 0;

// ====== Dźwięk ======
bool beepOnHold = true;
bool beepOnTare = true;
bool beepPending = false;
unsigned long beepEndMs = 0;

// ====== Pomocnicze ======
void touchActivity() {
  lastActivityMs = millis();
  u8g2.setPowerSave(0);
}

void buzzerInit() {
  if (BUZZER_PIN < 0) return;
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
}

void beepTrigger(uint16_t ms = 40) {
  if (BUZZER_PIN < 0 || !beepOnHold || !BEEP_ENABLE) return;
  digitalWrite(BUZZER_PIN, HIGH);
  beepPending = true;
  beepEndMs = millis() + ms;
}

void beepUpdate() {
  if (!beepPending) return;
  if (millis() < beepEndMs) return;
  digitalWrite(BUZZER_PIN, LOW);
  beepPending = false;
}

void beepBeep() {
  if (BUZZER_PIN < 0 || !BEEP_ENABLE || !beepOnTare) return;
  auto pulse = [&](uint16_t onMs, uint16_t offMs){
    digitalWrite(BUZZER_PIN, HIGH); delay(onMs);
    digitalWrite(BUZZER_PIN, LOW);  if (offMs) delay(offMs);
  };
  pulse(50, 70);
  pulse(50, 0);
}

bool hasNonASCII(const char* s) {
  if (!s) return false;
  for (const unsigned char* p = (const unsigned char*)s; *p; ++p) if (*p >= 0x80) return true;
  return false;
}

void applyOrientation() {
  switch (oledRotation) {
    case 0: u8g2.setDisplayRotation(U8G2_R0); break;
    case 1: u8g2.setDisplayRotation(U8G2_R1); break;
    case 2: u8g2.setDisplayRotation(U8G2_R2); break;
    case 3: u8g2.setDisplayRotation(U8G2_R3); break;
    default: u8g2.setDisplayRotation(U8G2_R0); oledRotation = 0; break;
  }
}

void applyOledSettings() {
  u8g2.setContrast((uint8_t)oledBrightness);
  applyOrientation();
}

void autoHoldReset() {
  ahActive = false;
  ahHasCandidate = false;
  ahStartMs = 0;
}

void autoHoldUpdate(long currentDeci, int enterDeltaDeci, int exitDeltaDeci) {
  if (!ahEnabled) { ahActive = false; return; }
  unsigned long now = millis();
  if (!ahActive) {
    if (!ahHasCandidate) {
      ahCandidateDeci = currentDeci;
      ahStartMs = now;
      ahHasCandidate = true;
    } else {
      if (labs(currentDeci - ahCandidateDeci) <= enterDeltaDeci) {
        if (now - ahStartMs >= ahStableMs) {
          ahActive = true;
          ahValueDeci = ahCandidateDeci;
          beepTrigger(40);
        }
      } else {
        ahCandidateDeci = currentDeci;
        ahStartMs = now;
      }
    }
  } else {
    if (labs(currentDeci - ahValueDeci) >= exitDeltaDeci) {
      ahActive = false;
      ahHasCandidate = true;
      ahCandidateDeci = currentDeci;
      ahStartMs = now;
    }
  }
}

uint8_t readButton1Event() {
  static bool lastStable = HIGH, lastRead = HIGH;
  static unsigned long lastChange = 0;
  static bool pressed = false, longSent = false;
  static unsigned long pressStart = 0;

  bool raw = digitalRead(BTN1_PIN);
  unsigned long now = millis();

  if (raw != lastRead) { lastRead = raw; lastChange = now; }

  if ((now - lastChange) > DEBOUNCE_MS) {
    if (lastStable != lastRead) {
      lastStable = lastRead;
      if (lastStable == LOW) { pressed = true; pressStart = now; longSent = false; touchActivity(); }
      else { if (pressed) { pressed = false; if (!longSent) return BTN_SHORT; } }
    }
    if (pressed && !longSent && (now - pressStart >= LONG_PRESS_MS)) {
      longSent = true; return BTN_LONG;
    }
  }
  return BTN_NONE;
}

bool doubleClickBtn1(uint8_t ev) {
  static unsigned long lastShortMs = 0;
  unsigned long now = millis();
  if (ev == BTN_SHORT) {
    if (now - lastShortMs <= DOUBLE_CLICK_WINDOW_MS) { lastShortMs = 0; return true; }
    lastShortMs = now;
  }
  return false;
}

uint8_t readButton2Event() {
  static bool lastStable = HIGH, lastRead = HIGH;
  static unsigned long lastChange = 0;
  static bool pressed = false, longSent = false;
  static unsigned long pressStart = 0;

  bool raw = digitalRead(BTN2_PIN);
  unsigned long now = millis();

  if (raw != lastRead) { lastRead = raw; lastChange = now; }

  if ((now - lastChange) > DEBOUNCE_MS) {
    if (lastStable != lastRead) {
      lastStable = lastRead;
      if (lastStable == LOW) { pressed = true; pressStart = now; longSent = false; touchActivity(); }
      else { if (pressed) { pressed = false; if (!longSent) return BTN_SHORT; } }
    }
    if (pressed && !longSent && (now - pressStart >= LONG_PRESS_MS)) {
      longSent = true; return BTN_LONG;
    }
  }
  return BTN_NONE;
}

void drawCenteredText(const char* line1, const char* line2 = nullptr) {
  const int dispW = u8g2.getDisplayWidth();
  const int dispH = u8g2.getDisplayHeight();
  const uint8_t* fonts[] = { u8g2_font_ncenB10_tr, u8g2_font_helvR10_tr, u8g2_font_6x10_tr };

  auto chooseFont = [&](const char* s)->const uint8_t* {
    if (!s || !*s) return fonts[0];
    if (hasNonASCII(s)) return u8g2_font_unifont_tr;
    for (size_t i=0; i<sizeof(fonts)/sizeof(fonts[0]); ++i) {
      u8g2.setFont(fonts[i]);
      if (u8g2.getStrWidth(s) <= dispW - 2) return fonts[i];
    }
    return u8g2_font_unifont_tr;
  };

  const uint8_t* f1 = chooseFont(line1);
  const uint8_t* f2 = chooseFont(line2);

  u8g2.clearBuffer();

  int h1 = 0, h2 = 0, w1 = 0, w2 = 0;
  bool l1utf = false, l2utf = false;

  if (line1 && *line1) {
    u8g2.setFont(f1);
    h1 = u8g2.getMaxCharHeight();
    l1utf = (f1 == u8g2_font_unifont_tr) || hasNonASCII(line1);
    w1 = l1utf ? u8g2.getUTF8Width(line1) : u8g2.getStrWidth(line1);
  }
  if (line2 && *line2) {
    u8g2.setFont(f2);
    h2 = u8g2.getMaxCharHeight();
    l2utf = (f2 == u8g2_font_unifont_tr) || hasNonASCII(line2);
    w2 = l2utf ? u8g2.getUTF8Width(line2) : u8g2.getStrWidth(line2);
  }

  int spacing = (line1 && *line1 && line2 && *line2) ? 6 : 0;
  int totalH = h1 + spacing + h2;
  int y = (dispH - totalH)/2;

  if (line1 && *line1) {
    u8g2.setFont(f1);
    int x1 = (dispW - w1)/2;
    if (l1utf) u8g2.drawUTF8(x1, y + h1, line1);
    else       u8g2.drawStr (x1, y + h1, line1);
  }
  if (line2 && *line2) {
    u8g2.setFont(f2);
    int x2 = (dispW - w2)/2;
    if (l2utf) u8g2.drawUTF8(x2, y + h1 + spacing + h2, line2);
    else       u8g2.drawStr (x2, y + h1 + spacing + h2, line2);
  }

  u8g2.sendBuffer();
}

const char* unitToStr(int u) {
  if (u == UNIT_DKG) return "dkg";
  if (u == UNIT_KG)  return "kg";
  return "g";
}

void startMarquee(const String& s) {
  if (largeDigitsMode) return;
  String t = s;
  t.replace("\r", ""); t.replace("\n", " ");
  if (t.length() > 250) t.remove(250);
  t.trim();
  t.toCharArray(marqueeText, sizeof(marqueeText));

  u8g2.setFont(MARQUEE_FONT);
  marqueeW = u8g2.getUTF8Width(marqueeText);
  marqueeX = u8g2.getDisplayWidth();
  marqueeStartMs = millis();
  marqueeActive = (marqueeText[0] != '\0');
  touchActivity();
}

void stopMarquee() {
  marqueeActive = false;
  marqueeText[0] = '\0';
  touchActivity();
}

// ====== NOWY INTERFACE WWW ======
const char MAIN_PAGE[] PROGMEM =
"<!doctype html>\n"
"<html lang='pl'>\n"
"<head>\n"
"<meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>\n"
"<title>WAGA by JagOOda</title>\n"
"<style>\n"
"* { margin: 0; padding: 0; box-sizing: border-box; }\n"
"body {\n"
"  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, sans-serif;\n"
"  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);\n"
"  min-height: 100vh;\n"
"  padding: 15px;\n"
"}\n"
".wrap {\n"
"  max-width: 600px;\n"
"  margin: 0 auto;\n"
"  display: grid;\n"
"  gap: 16px;\n"
"}\n"
".card {\n"
"  background: white;\n"
"  border-radius: 15px;\n"
"  padding: 20px;\n"
"  box-shadow: 0 10px 40px rgba(0,0,0,0.3);\n"
"}\n"
".header {\n"
"  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);\n"
"  color: white;\n"
"  padding: 25px 20px;\n"
"  border-radius: 15px;\n"
"  text-align: center;\n"
"  box-shadow: 0 10px 40px rgba(0,0,0,0.3);\n"
"}\n"
".header h1 { font-size: 32px; font-weight: 700; margin: 0; letter-spacing: 1px; }\n"
".header .author { font-size: 12px; opacity: 0.85; margin-top: 8px; letter-spacing: 2px; font-weight: 300; }\n"
".section-title {\n"
"  font-size: 14px;\n"
"  font-weight: 600;\n"
"  color: #666;\n"
"  margin-bottom: 10px;\n"
"  text-transform: uppercase;\n"
"  letter-spacing: 0.5px;\n"
"}\n"
".val {\n"
"  font-size: 60px;\n"
"  font-weight: 900;\n"
"  letter-spacing: 0.5px;\n"
"  line-height: 1;\n"
"  color: #333;\n"
"}\n"
".val small { font-size: 20px; color: #999; font-weight: 600; }\n"
"\n"
"textarea {\n"
"  width: 100%;\n"
"  min-height: 100px;\n"
"  padding: 12px;\n"
"  border: 2px solid #e0e0e0;\n"
"  border-radius: 10px;\n"
"  font-size: 16px;\n"
"  resize: vertical;\n"
"  font-family: inherit;\n"
"  transition: border-color 0.3s;\n"
"}\n"
"textarea:focus {\n"
"  outline: none;\n"
"  border-color: #667eea;\n"
"}\n"
".char-count {\n"
"  text-align: right;\n"
"  color: #999;\n"
"  font-size: 13px;\n"
"  margin-top: 5px;\n"
"}\n"
"\n"
".btn-grid {\n"
"  display: grid;\n"
"  gap: 10px;\n"
"  margin-top: 15px;\n"
"}\n"
".btn-grid.three-col { grid-template-columns: repeat(3, 1fr); }\n"
".btn-grid.full { grid-template-columns: 1fr; }\n"
"\n"
"button, .btn {\n"
"  padding: 15px;\n"
"  font-size: 15px;\n"
"  font-weight: 600;\n"
"  border: none;\n"
"  border-radius: 10px;\n"
"  cursor: pointer;\n"
"  transition: all 0.3s;\n"
"  text-transform: uppercase;\n"
"  letter-spacing: 0.5px;\n"
"  box-shadow: 0 4px 6px rgba(0,0,0,0.1);\n"
"}\n"
"button:active, .btn:active {\n"
"  transform: translateY(2px);\n"
"  box-shadow: 0 2px 3px rgba(0,0,0,0.1);\n"
"}\n"
".btn.block { width: 100%; }\n"
".btn-send { background: #4CAF50; color: white; }\n"
".btn-stop { background: #ff9800; color: white; }\n"
".btn-clear { background: #f44336; color: white; }\n"
".btn-tare { background: #2196F3; color: white; }\n"
"button.active { background: #2ecc71; }\n"
"button.ghost { background: #9aa3b2; color: white; }\n"
"\n"
".row { display: flex; gap: 8px; flex-wrap: wrap; }\n"
".seg { display: flex; gap: 8px; flex-wrap: wrap; }\n"
"\n"
"input[type=text], input[type=number], select {\n"
"  flex: 1;\n"
"  padding: 12px;\n"
"  border: 2px solid #e0e0e0;\n"
"  border-radius: 10px;\n"
"  font-size: 16px;\n"
"  outline: none;\n"
"  min-width: 200px;\n"
"}\n"
"input[type=range] { width: 100%; }\n"
"\n"
"label {\n"
"  font-size: 13px;\n"
"  color: #666;\n"
"  display: block;\n"
"  margin-bottom: 6px;\n"
"}\n"
"\n"
".info-box {\n"
"  background: #e3f2fd;\n"
"  padding: 12px;\n"
"  border-radius: 10px;\n"
"  border-left: 4px solid #2196F3;\n"
"  font-size: 13px;\n"
"  color: #0d47a1;\n"
"  line-height: 1.5;\n"
"  margin-top: 10px;\n"
"}\n"
".info-box.success {\n"
"  background: #d4edda;\n"
"  border-left-color: #2ecc71;\n"
"  color: #155724;\n"
"}\n"
"\n"
".kv { font-size: 14px; color: #666; }\n"
".footer {\n"
"  font-size: 12px;\n"
"  color: white;\n"
"  text-align: center;\n"
"  margin-top: 20px;\n"
"  opacity: 0.8;\n"
"}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class='wrap'>\n"
"\n"
"  <div class='header'>\n"
"    <h1>⚖️ WAGA</h1>\n"
"    <div class='author'>by JagOOda</div>\n"
"  </div>\n"
"\n"
"  <div class='card'>\n"
"    <div class='section-title'>Aktualna waga</div>\n"
"    <div class='val'><span id='w'>--</span> <small id='u'></small> <small id='h'></small></div>\n"
"    <button class='btn btn-tare block' onclick='tare()'>TARA</button>\n"
"  </div>\n"
"\n"
"  <div class='card'>\n"
"    <div class='section-title'>📝 Komunikat na OLED</div>\n"
"    <textarea id='msg' maxlength='250' placeholder='Wpisz komunikat (max 250 znaków)...' oninput='updateCounter()'></textarea>\n"
"    <div class='char-count'><span id='cnt'>0</span> / 250 znaków</div>\n"
"    <div class='btn-grid three-col'>\n"
"      <button class='btn btn-send' onclick='sendMsg()'>Wyślij</button>\n"
"      <button class='btn btn-stop' onclick='stopMsg()'>Stop</button>\n"
"      <button class='btn btn-clear' onclick='clearMsg()'>Wyczyść</button>\n"
"    </div>\n"
"    <div class='info-box' id='msg_info'>Brak aktywnego komunikatu.</div>\n"
"    <label style='margin-top:10px'>Prędkość przewijania: <span id='ms_val'>--</span> px/krok</label>\n"
"    <input id='mspeed' type='range' min='0' max='6' step='1' value='2' oninput='onMspeed(this.value)'>\n"
"  </div>\n"
"\n"
"  <div class='card'>\n"
"    <div class='section-title'>Jednostki i dokładność</div>\n"
"    <label>Jednostka</label>\n"
"    <div id='unitSeg' class='seg'>\n"
"      <button class='btn' data-v='0' onclick='setUnit(0)'>g</button>\n"
"      <button class='btn' data-v='1' onclick='setUnit(1)'>dkg</button>\n"
"      <button class='btn' data-v='2' onclick='setUnit(2)'>kg</button>\n"
"    </div>\n"
"    <label style='margin-top:8px'>Rozdzielczość</label>\n"
"    <div id='resSeg' class='seg'>\n"
"      <button class='btn' data-v='1' onclick='setRes(1)'>0.1 g</button>\n"
"      <button class='btn' data-v='0' onclick='setRes(0)'>1 g</button>\n"
"    </div>\n"
"    <label style='margin-top:8px'>Kierunek</label>\n"
"    <div id='dirSeg' class='seg'>\n"
"      <button class='btn' data-v='1' onclick='setDir(1)'>+1</button>\n"
"      <button class='btn' data-v='-1' onclick='setDir(-1)'>-1</button>\n"
"    </div>\n"
"    <label style='margin-top:8px'>Auto‑HOLD</label>\n"
"    <div id='holdSeg' class='seg'>\n"
"      <button class='btn' data-v='1' onclick='setHold(1)'>ON</button>\n"
"      <button class='btn' data-v='0' onclick='setHold(0)'>OFF</button>\n"
"    </div>\n"
"  </div>\n"
"\n"
"  <div class='card'>\n"
"    <div class='section-title'>Wyświetlacz</div>\n"
"    <label>Jasność: <span id='bright_val'>--</span></label>\n"
"    <input id='bright' type='range' min='0' max='255' step='1' value='255' oninput='onBright(this.value)'>\n"
"    <label style='margin-top:8px'>Orientacja</label>\n"
"    <div id='rotSeg' class='seg'>\n"
"      <button class='btn' data-v='0' onclick='setRot(0)'>0°</button>\n"
"      <button class='btn' data-v='1' onclick='setRot(1)'>90°</button>\n"
"      <button class='btn' data-v='2' onclick='setRot(2)'>180°</button>\n"
"      <button class='btn' data-v='3' onclick='setRot(3)'>270°</button>\n"
"    </div>\n"
"    <label style='margin-top:8px'>Duże cyfry</label>\n"
"    <div id='largeSeg' class='seg'>\n"
"      <button class='btn' data-v='1' onclick='setLarge(1)'>ON</button>\n"
"      <button class='btn' data-v='0' onclick='setLarge(0)'>OFF</button>\n"
"    </div>\n"
"    <label style='margin-top:8px'>Pik przy HOLD</label>\n"
"    <div id='beepSeg' class='seg'>\n"
"      <button class='btn' data-v='1' onclick='setBeep(1)'>ON</button>\n"
"      <button class='btn' data-v='0' onclick='setBeep(0)'>OFF</button>\n"
"    </div>\n"
"    <label style='margin-top:8px'>Dźwięk tarowania</label>\n"
"    <div id='tareBeepSeg' class='seg'>\n"
"      <button class='btn' data-v='1' onclick='setTareBeep(1)'>ON</button>\n"
"      <button class='btn' data-v='0' onclick='setTareBeep(0)'>OFF</button>\n"
"    </div>\n"
"  </div>\n"
"\n"
"  <div class='card'>\n"
"    <div class='section-title'>Kalibracja</div>\n"
"    <div class='kv'>CF: <span id='cf'>--</span>, kierunek: <span id='cdir'>--</span></div>\n"
"    <div class='row' style='margin-top:8px'>\n"
"      <select id='cweight' style='flex:1;padding:12px;border:2px solid #e0e0e0;border-radius:10px;font-size:16px'>\n"
"        <option>200</option><option>250</option><option>500</option><option selected>1000</option><option>2000</option><option>5000</option>\n"
"      </select>\n"
"      <button class='btn ghost' onclick='calTare()'>TARA</button>\n"
"      <button class='btn btn-send' onclick='calMeasure()'>POMIAR</button>\n"
"    </div>\n"
"  </div>\n"
"\n"
"  <div class='footer'>Połącz: http://waga.local lub IP AP. Komunikat przewija się na OLED przez 1 min lub do STOP.</div>\n"
"</div>\n"
"\n"
"<script>\n"
"let BR_TMR=null, MS_TMR=null;\n"
"\n"
"function setActive(id,val){\n"
"  const seg=document.getElementById(id);\n"
"  if(!seg)return;\n"
"  [...seg.querySelectorAll('.btn')].forEach(b=>{\n"
"    b.classList.toggle('active', String(b.dataset.v)===String(val));\n"
"  });\n"
"}\n"
"\n"
"async function postSet(obj){\n"
"  const body=Object.entries(obj).map(([k,v])=>encodeURIComponent(k)+'='+encodeURIComponent(v)).join('&');\n"
"  const r=await fetch('/set',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});\n"
"  try{return await r.json();}catch(e){return {};}\n"
"}\n"
"\n"
"async function setUnit(v){setActive('unitSeg',v);await postSet({unit:v});}\n"
"async function setRes(v){setActive('resSeg',v);await postSet({res:v});}\n"
"async function setDir(v){setActive('dirSeg',v);await postSet({dir:v});}\n"
"async function setHold(v){setActive('holdSeg',v);await postSet({hold:v});}\n"
"function onBright(v){\n"
"  document.getElementById('bright_val').innerText=v;\n"
"  if(BR_TMR)clearTimeout(BR_TMR);\n"
"  BR_TMR=setTimeout(()=>postSet({bright:v}),150);\n"
"}\n"
"async function setBeep(v){setActive('beepSeg',v);await postSet({beep:v});}\n"
"async function setTareBeep(v){setActive('tareBeepSeg',v);await postSet({tareBeep:v});}\n"
"async function setLarge(v){setActive('largeSeg',v);await postSet({largeD:v});}\n"
"function onMspeed(v){\n"
"  document.getElementById('ms_val').innerText=v;\n"
"  if(MS_TMR)clearTimeout(MS_TMR);\n"
"  MS_TMR=setTimeout(()=>postSet({mspeed:v}),150);\n"
"}\n"
"async function setRot(v){setActive('rotSeg',v);await postSet({rot:v});}\n"
"\n"
"function updateCounter(){\n"
"  const el=document.getElementById('msg');\n"
"  const c=(el&&el.value)?el.value.length:0;\n"
"  const cnt=document.getElementById('cnt');\n"
"  if(cnt)cnt.textContent=c;\n"
"}\n"
"\n"
"async function refresh(){\n"
"  try{\n"
"    const r=await fetch('/status'); const j=await r.json();\n"
"    document.getElementById('w').textContent=j.value;\n"
"    document.getElementById('u').textContent=j.unit;\n"
"    document.getElementById('h').textContent=j.hold?'HOLD':'';\n"
"    setActive('unitSeg',j.config.unit);\n"
"    setActive('resSeg',j.config.res);\n"
"    setActive('dirSeg',j.config.dir);\n"
"    setActive('holdSeg',j.config.hold?1:0);\n"
"    setActive('beepSeg',j.sys.beep?1:0);\n"
"    setActive('tareBeepSeg',j.sys.tareBeep?1:0);\n"
"    setActive('largeSeg',j.sys.largeD?1:0);\n"
"    setActive('rotSeg',j.sys.rot);\n"
"    document.getElementById('bright').value=j.sys.bright;\n"
"    document.getElementById('bright_val').innerText=j.sys.bright;\n"
"    document.getElementById('mspeed').value=j.sys.mspeed;\n"
"    document.getElementById('ms_val').innerText=j.sys.mspeed;\n"
"    \n"
"    const msgInfo = document.getElementById('msg_info');\n"
"    if(j.msg.active) {\n"
"      msgInfo.textContent = '✓ Komunikat wyświetlany na OLED';\n"
"      msgInfo.className = 'info-box success';\n"
"    } else {\n"
"      msgInfo.textContent = 'Brak aktywnego komunikatu.';\n"
"      msgInfo.className = 'info-box';\n"
"    }\n"
"    \n"
"    document.getElementById('cf').textContent=j.cal.cf.toFixed(3);\n"
"    document.getElementById('cdir').textContent=j.cal.dir>0?'+1':'-1';\n"
"  }catch(e){}\n"
"}\n"
"setInterval(refresh,1000); refresh(); updateCounter();\n"
"\n"
"async function sendMsg(){\n"
"  const t=document.getElementById('msg').value||'';\n"
"  if(t.trim()===''){\n"
"    alert('⚠️ Wpisz jakiś tekst!');\n"
"    return;\n"
"  }\n"
"  const body='msg='+encodeURIComponent(t);\n"
"  const r=await fetch('/send',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'},body});\n"
"  alert(await r.text());\n"
"  setTimeout(refresh, 300);\n"
"}\n"
"\n"
"async function stopMsg(){\n"
"  const r=await fetch('/stop',{method:'POST'});\n"
"  alert(await r.text());\n"
"  setTimeout(refresh, 300);\n"
"}\n"
"\n"
"function clearMsg(){\n"
"  document.getElementById('msg').value='';\n"
"  updateCounter();\n"
"}\n"
"\n"
"async function tare(){\n"
"  const r=await fetch('/set',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'tare=1'});\n"
"  alert(await r.text());\n"
"  setTimeout(refresh,300);\n"
"}\n"
"\n"
"async function calTare(){\n"
"  if(!confirm('Zdejmij wszystko z wagi i potwierdź'))return;\n"
"  const r=await fetch('/cal_tare',{method:'POST'});\n"
"  alert(await r.text());\n"
"}\n"
"\n"
"async function calMeasure(){\n"
"  const w=parseInt(document.getElementById('cweight').value);\n"
"  if(!confirm('Połóż odważnik '+w+'g i potwierdź'))return;\n"
"  const r=await fetch('/cal_measure?weight='+w,{method:'POST'});\n"
"  alert(await r.text());\n"
"  setTimeout(refresh,500);\n"
"}\n"
"</script>\n"
"</body>\n"
"</html>\n";

// ZMIANA: Poprawione formatowanie kg (2 lub 3 miejsca po przecinku)
String formatWeightValue(long deci, int unit, bool fine01) {
  float grams = deci / 10.0f;
  float val = 0.0f; int decimals = 0;
  if (unit == UNIT_G) { val = grams; decimals = fine01 ? 1 : 0; }
  else if (unit == UNIT_DKG) { val = grams / 10.0f; decimals = fine01 ? 2 : 1; }
  else { val = grams / 1000.0f; decimals = fine01 ? 3 : 2; }  // ZMIANA: 3 lub 2 miejsca
  char fmt[8]; snprintf(fmt, sizeof(fmt), "%%.%df", decimals);
  char buf[32]; snprintf(buf, sizeof(buf), fmt, val);
  return String(buf);
}

void sendStatusJSON() {
  long toShowDeci = ahActive ? ahValueDeci : lastWeightDeci;
  String v = formatWeightValue(toShowDeci, unitMode, fine01gMode);
  String u = unitToStr(unitMode);
  String json = String("{")
    + "\"value\":\"" + v + "\","
    + "\"unit\":\"" + u + "\","
    + "\"hold\":" + (ahActive ? "true" : "false") + ","
    + "\"config\":{"
      + "\"unit\":" + unitMode + ","
      + "\"res\":" + (fine01gMode?1:0) + ","
      + "\"dir\":" + directionSign + ","
      + "\"hold\":" + (ahEnabled?1:0)
    + "},"
    + "\"cal\":{"
      + "\"cf\":" + String(calibrationFactor, 6) + ","
      + "\"dir\":" + directionSign
    + "},"
    + "\"sys\":{"
      + "\"bright\":" + oledBrightness + ","
      + "\"beep\":" + (beepOnHold?1:0) + ","
      + "\"tareBeep\":" + (beepOnTare?1:0) + ","
      + "\"largeD\":" + (largeDigitsMode?1:0) + ","
      + "\"mspeed\":" + marqueeSpeedPx + ","
      + "\"rot\":" + oledRotation
    + "},"
    + "\"msg\":{"
      + "\"active\":" + (marqueeActive?1:0)
    + "}"
  + "}";
  server.sendHeader("Cache-Control","no-cache");
  server.send(200, "application/json; charset=utf-8", json);
}

void handleRoot() {
  touchActivity();
  server.send_P(200, "text/html; charset=utf-8", MAIN_PAGE);
}
void handleStatus() { touchActivity(); sendStatusJSON(); }
void handleWeight() { handleStatus(); }

void handleSend() {
  touchActivity();
  if (largeDigitsMode) {
    server.send(403, "text/plain; charset=utf-8", "Wysyłanie tekstu jest wyłączone w trybie dużych cyfr.");
    return;
  }
  if (!server.hasArg("msg")) { server.send(400, "text/plain; charset=utf-8", "Brak parametru msg"); return; }
  String msg = server.arg("msg");
  if (msg.length() > 250) msg = msg.substring(0, 250);
  startMarquee(msg);
  server.send(200, "text/plain; charset=utf-8", "Wysłano. Tekst pojawi się na OLED.");
}
void handleStop() { touchActivity(); stopMarquee(); server.send(200, "text/plain; charset=utf-8", "Zatrzymano wyświetlanie."); }

void handleSet() {
  touchActivity();
  bool needSave = false;

  if (server.hasArg("unit")) { int v = server.arg("unit").toInt(); if (v>=UNIT_G && v<=UNIT_KG) { unitMode = v; needSave=true; } }
  if (server.hasArg("res"))  { int v = server.arg("res").toInt(); bool nf = (v!=0); if (nf!=fine01gMode){ fine01gMode=nf; filterInit=false; autoHoldReset(); needSave=true; } }
  if (server.hasArg("dir"))  { int v = server.arg("dir").toInt(); v = (v>=0)?+1:-1; if (v!=directionSign){ directionSign=v; autoHoldReset(); needSave=true; } }
  if (server.hasArg("hold")) { int v = server.arg("hold").toInt(); bool en = (v!=0); if (en!=ahEnabled){ ahEnabled=en; if(!ahEnabled) ahActive=false; needSave=true; } }

  if (server.hasArg("bright")) { int b = server.arg("bright").toInt(); b = constrain(b,0,255); if (b!=oledBrightness){ oledBrightness=b; applyOledSettings(); needSave=true; } }
  if (server.hasArg("beep"))   { bool bp = (server.arg("beep").toInt()!=0); if (bp!=beepOnHold){ beepOnHold=bp; needSave=true; } }
  if (server.hasArg("mspeed")) { int ms = server.arg("mspeed").toInt(); ms = constrain(ms,0,6); if (ms!=marqueeSpeedPx){ marqueeSpeedPx=ms; needSave=true; } }
  if (server.hasArg("rot"))    { int r = server.arg("rot").toInt(); r = constrain(r,0,3); if (r!=oledRotation){ oledRotation=r; applyOrientation(); needSave=true; } }
  if (server.hasArg("tareBeep")) { bool tb = (server.arg("tareBeep").toInt()!=0); if (tb!=beepOnTare){ beepOnTare=tb; needSave=true; } }
  if (server.hasArg("largeD")) { bool ld = (server.arg("largeD").toInt()!=0); if (ld!=largeDigitsMode){ largeDigitsMode=ld; needSave=true; } }

  if (server.hasArg("tare")) {
    scale.tare();
    beepBeep();
    autoHoldReset();
    filteredGrams = 0.0f;
    filterInit = true;
    lastWeightDeci = 0;
    haveLastWeight = true;
  }

  if (needSave) {
    prefs.begin(PREF_NAMESPACE, false);
    prefs.putInt(PREF_KEY_UNIT, unitMode);
    prefs.putInt(PREF_KEY_RES,  fine01gMode ? 1 : 0);
    prefs.putInt(PREF_KEY_DIR,  directionSign);
    prefs.putInt(PREF_KEY_HOLD, ahEnabled ? 1 : 0);
    prefs.putInt(PREF_KEY_BRIGHT, oledBrightness);
    prefs.putInt(PREF_KEY_BEEP, beepOnHold ? 1 : 0);
    prefs.putInt(PREF_KEY_ROT, oledRotation);
    prefs.putInt(PREF_KEY_MSPEED, marqueeSpeedPx);
    prefs.putInt(PREF_KEY_TARA_BEEP, beepOnTare ? 1 : 0);
    prefs.putInt(PREF_KEY_LARGE_D, largeDigitsMode ? 1 : 0);
    prefs.end();
  }

  sendStatusJSON();
}

void handleCalTare() {
  touchActivity();
  calBusy = true;
  scale.tare();
  beepBeep();
  autoHoldReset();
  filteredGrams = 0.0f;
  filterInit = true;
  lastWeightDeci = 0;
  haveLastWeight = true;
  calBusy = false;
  server.send(200, "text/plain; charset=utf-8", "Tarowanie OK. Połóż odważnik i naciśnij POMIAR.");
}

void handleCalMeasure() {
  touchActivity();
  if (!server.hasArg("weight")) { server.send(400, "text/plain; charset=utf-8", "Brak parametru weight"); return; }
  int mass = server.arg("weight").toInt();
  bool okWeight = false;
  for (uint8_t i=0;i<sizeof(CAL_WEIGHTS)/sizeof(CAL_WEIGHTS[0]);i++) if (CAL_WEIGHTS[i]==mass) { okWeight = true; break; }
  if (!okWeight) { server.send(400, "text/plain; charset=utf-8", "Nieprawidłowy odważnik"); return; }

  calBusy = true;
  delay(150);
  long raw = scale.get_value(20);
  float newCal = fabsf((float)raw) / (float)mass;
  if (newCal <= 0.000001f) { calBusy = false; server.send(500, "text/plain; charset=utf-8", "Błąd pomiaru (CF=0)."); return; }

  calibrationFactor = newCal;
  scale.set_scale(calibrationFactor);
  directionSign = (raw >= 0) ? +1 : -1;

  prefs.begin(PREF_NAMESPACE, false);
  prefs.putFloat(PREF_KEY_CAL, calibrationFactor);
  prefs.putInt(PREF_KEY_DIR, directionSign);
  prefs.end();

  calBusy = false;

  char out[160];
  snprintf(out, sizeof(out),
           "Kalibracja OK.\nOdważnik=%d g\nraw=%ld\nCF=%.6f counts/g\ndir=%s",
           mass, raw, calibrationFactor, directionSign>0?"+1":"-1");
  server.send(200, "text/plain; charset=utf-8", out);
}

void loadAllPrefs() {
  prefs.begin(PREF_NAMESPACE, true);
  float cal = prefs.getFloat(PREF_KEY_CAL, NAN);
  int dir  = prefs.getInt(PREF_KEY_DIR, +1);
  int unit = prefs.getInt(PREF_KEY_UNIT, UNIT_G);
  int res  = prefs.getInt(PREF_KEY_RES, 1);
  int hold = prefs.getInt(PREF_KEY_HOLD, 1);
  int br   = prefs.getInt(PREF_KEY_BRIGHT, 255);
  int bp   = prefs.getInt(PREF_KEY_BEEP, 1);
  int rot  = prefs.getInt(PREF_KEY_ROT, 0);
  int ms   = prefs.getInt(PREF_KEY_MSPEED, 2);
  int tb   = prefs.getInt(PREF_KEY_TARA_BEEP, 1);
  int ld   = prefs.getInt(PREF_KEY_LARGE_D, 0);
  prefs.end();

  calibrationFactor = isnan(cal) ? 1.0f : cal;
  directionSign     = (dir >= 0) ? +1 : -1;
  unitMode          = (unit >= UNIT_G && unit <= UNIT_KG) ? unit : UNIT_G;
  fine01gMode       = (res != 0);
  ahEnabled         = (hold != 0);
  oledBrightness    = constrain(br, 0, 255);
  beepOnHold        = (bp != 0);
  oledRotation      = constrain(rot, 0, 3);
  marqueeSpeedPx    = constrain(ms, 0, 6);
  beepOnTare        = (tb != 0);
  largeDigitsMode   = (ld != 0);
}

void drawSplash() {
  u8g2.clearBuffer();
  const char* title = "WAGA";
  const char* sub   = "by JagOOda";
  const int dispW = u8g2.getDisplayWidth();
  const int dispH = u8g2.getDisplayHeight();
  const uint8_t* titleFonts[] = { u8g2_font_ncenB24_tr, u8g2_font_ncenB18_tr, u8g2_font_ncenB14_tr, u8g2_font_ncenB10_tr };
  const uint8_t* subFonts[]   = { u8g2_font_helvR12_tr,  u8g2_font_helvR10_tr,  u8g2_font_6x10_tr };
  const uint8_t* chosenTitle = titleFonts[0];
  const uint8_t* chosenSub   = subFonts[0];
  int titleW=0, subW=0, titleH=0, subH=0, spacing=4;
  for (size_t i=0;i<sizeof(titleFonts)/sizeof(titleFonts[0]);++i) {
    u8g2.setFont(titleFonts[i]);
    int w = u8g2.getStrWidth(title);
    int h = u8g2.getMaxCharHeight();
    const uint8_t* subTry = subFonts[0];
    int sw=0, sh=0;
    for (size_t j=0;j<sizeof(subFonts)/sizeof(subFonts[0]);++j) {
      u8g2.setFont(subFonts[j]);
      sw = u8g2.getStrWidth(sub);
      sh = u8g2.getMaxCharHeight();
      if (sw <= dispW - 2) { subTry = subFonts[j]; break; }
    }
    int totalH = h + spacing + sh;
    if (w <= dispW - 2 && totalH <= dispH - 2) {
      chosenTitle = titleFonts[i]; chosenSub = subTry;
      titleW = w; titleH = h; subW = sw; subH = sh;
      break;
    }
    if (i == (sizeof(titleFonts)/sizeof(titleFonts[0]) - 1)) {
      chosenTitle = titleFonts[i]; chosenSub = subFonts[(sizeof(subFonts)/sizeof(subFonts[0]) - 1)];
      u8g2.setFont(chosenTitle); titleW = u8g2.getStrWidth(title); titleH = u8g2.getMaxCharHeight();
      u8g2.setFont(chosenSub);   subW   = u8g2.getStrWidth(sub);   subH   = u8g2.getMaxCharHeight();
    }
  }
  int freeH = dispH - (titleH + subH);
  spacing = max(2, freeH / 3);
  u8g2.setFont(chosenTitle);
  int xTitle = (dispW - titleW)/2;
  int yTitle = (dispH - (titleH + spacing + subH))/2 + titleH;
  u8g2.drawStr(xTitle, yTitle, title);
  u8g2.setFont(chosenSub);
  int xSub = (dispW - subW)/2;
  int ySub = yTitle + spacing + subH;
  u8g2.drawStr(xSub, ySub, sub);
  u8g2.sendBuffer();
}

// ====== ZMIENIONA FUNKCJA RYSOWANIA ======
void drawWeight(long deciGrams, bool hold, int unit, bool fine01) {
  float grams = deciGrams / 10.0f;
  float val = 0.0f; int decimals = 0;
  const char* unitStr = unitToStr(unit);

  if (unit == UNIT_G) { val = grams; decimals = fine01 ? 1 : 0; }
  else if (unit == UNIT_DKG) { val = grams / 10.0f; decimals = fine01 ? 2 : 1; }
  else { val = grams / 1000.0f; decimals = fine01 ? 3 : 2; }  // ZMIANA: 3 lub 2

  char fmt[8]; snprintf(fmt, sizeof(fmt), "%%.%df", decimals);
  char numStr[32]; snprintf(numStr, sizeof(numStr), fmt, val);

  u8g2.clearBuffer();
  const int16_t dispW = u8g2.getDisplayWidth();
  const int16_t dispH = u8g2.getDisplayHeight();
  const int16_t y_bottom = dispH - 1;

  if (largeDigitsMode) {
    // TRYB DUŻYCH CYFR
    int len = strlen(numStr);
    const uint8_t* numFont;
    int baseline;

    if (len <= 4) {
        numFont = u8g2_font_logisoso58_tn; baseline = 58; 
    } else if (len <= 5) {
        numFont = u8g2_font_logisoso46_tn; baseline = 50;
    } else {
        numFont = u8g2_font_logisoso38_tn; baseline = 46;
    }
    
    u8g2.setFont(numFont);
    int16_t wNum = u8g2.getStrWidth(numStr);
    int16_t xNum = (dispW - wNum) / 2;
    if(xNum < 0) xNum = 0;
    u8g2.drawStr(xNum, baseline, numStr);

    // Jednostka w prawym dolnym rogu
    u8g2.setFont(u8g2_font_helvR10_tr);
    int16_t wUnit = u8g2.getStrWidth(unitStr);
    u8g2.drawStr(dispW - wUnit - 2, y_bottom, unitStr);

    // ZMIANA: HOLD w prawym górnym rogu
    if (hold) {
      const char* holdStr = "HOLD";
      u8g2.setFont(u8g2_font_6x10_tr);
      int16_t wHold = u8g2.getStrWidth(holdStr);
      u8g2.setFont(u8g2_font_open_iconic_arrow_1x_t);
      int16_t wArrow = 8;
      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(dispW - wHold - wArrow - 3, 12, holdStr);
      u8g2.setFont(u8g2_font_open_iconic_arrow_1x_t);
      u8g2.drawGlyph(dispW - wArrow - 1, 12, 67); // strzałka w górę
    }

  } else {
    // TRYB NORMALNY
    
    // Górny pasek
    if (marqueeActive && mode == MODE_NORMAL) {
      u8g2.setFont(MARQUEE_FONT);
      if (marqueeX <= -marqueeW) marqueeX = dispW;
      u8g2.drawUTF8(marqueeX, 12, marqueeText);
    } else {
      // ZMIANA: Tylko "Waga:" z lewej, HOLD z prawej
      u8g2.setFont(u8g2_font_helvR10_tr);
      u8g2.drawStr(0, 12, "Waga:");
      
      // ZMIANA: HOLD w prawym górnym rogu
      if (hold) {
        const char* holdStr = "HOLD";
        u8g2.setFont(u8g2_font_6x10_tr);
        int16_t wHold = u8g2.getStrWidth(holdStr);
        u8g2.setFont(u8g2_font_open_iconic_arrow_1x_t);
        int16_t wArrow = 8;
        u8g2.setFont(u8g2_font_6x10_tr);
        u8g2.drawStr(dispW - wHold - wArrow - 3, 12, holdStr);
        u8g2.setFont(u8g2_font_open_iconic_arrow_1x_t);
        u8g2.drawGlyph(dispW - wArrow - 1, 12, 67);
      }
    }

    // ZMIANA: Dolny pasek - USUNIĘTO napis z prawego dolnego rogu

    // Wartość wagi na środku
    const int16_t gap = 4;
    u8g2.setFont(u8g2_font_helvR10_tr);
    int16_t wUnit = u8g2.getStrWidth(unitStr);
    u8g2.setFont(u8g2_font_logisoso28_tn);
    int16_t wNum = u8g2.getStrWidth(numStr);
    
    int16_t totalW = wNum + gap + wUnit;
    int16_t xNum = (dispW - totalW) / 2;
    if (xNum < 0) xNum = 0;
    int16_t xUnit = xNum + wNum + gap;

    u8g2.drawStr(xNum, 45, numStr);
    u8g2.setFont(u8g2_font_helvR10_tr);
    u8g2.drawStr(xUnit, 45, unitStr);
  }

  u8g2.sendBuffer();

  // Marquee
  if (!largeDigitsMode && marqueeActive && mode == MODE_NORMAL) {
    marqueeX -= marqueeSpeedPx;
    if (millis() - marqueeStartMs >= MARQUEE_MAX_MS) {
      stopMarquee();
    }
  }
}

void drawNoHx711() { drawCenteredText("Brak HX711!", "Sprawdź połączenia"); }
void drawInfo(const char* a, const char* b = nullptr) { drawCenteredText(a, b); }

void setup() {
  Serial.begin(115200);

  Wire.begin(21, 22);
  Wire.setClock(400000);
  u8g2.begin();
  u8g2.setFontMode(1);
  u8g2.setFontDirection(0);

  loadAllPrefs();
  applyOledSettings();

  drawSplash();
  delay(800);

  pinMode(BTN1_PIN, INPUT_PULLUP);
  pinMode(BTN2_PIN, INPUT_PULLUP);

  buzzerInit();

  scale.begin(HX_DOUT_PIN, HX_SCK_PIN);
  scale.set_scale(calibrationFactor);
  scale.tare();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP: "); Serial.println(IP);

  if (MDNS.begin("waga")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS OK: http://waga.local");
  } else {
    Serial.println("mDNS FAIL");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/weight", HTTP_GET, handleWeight);
  server.on("/send", HTTP_POST, handleSend);
  server.on("/stop", HTTP_POST, handleStop);
  server.on("/set",  HTTP_POST, handleSet);
  server.on("/cal_tare",    HTTP_POST, handleCalTare);
  server.on("/cal_measure", HTTP_POST, handleCalMeasure);
  server.onNotFound([](){ server.send(404, "text/plain; charset=utf-8", "404 Nie znaleziono"); });
  server.begin();

  lastReadyMs = millis();
  lastDrawMs  = 0;
  haveLastWeight = false;
  filterInit = false;
  autoHoldReset();
  touchActivity();
}

void loop() {
  server.handleClient();
  beepUpdate();

  uint8_t ev1 = readButton1Event();
  uint8_t ev2 = readButton2Event();

  if (mode == MODE_NORMAL && doubleClickBtn1(ev1)) {
    directionSign = -directionSign;
    prefs.begin(PREF_NAMESPACE, false);
    prefs.putInt(PREF_KEY_DIR, directionSign);
    prefs.end();
    autoHoldReset();
    drawInfo("Zmieniono kierunek", directionSign > 0 ? "dir = +1" : "dir = -1");
    delay(500);
    touchActivity();
  }

  if (mode == MODE_NORMAL) {
    if (ev1 == BTN_SHORT) {
      scale.tare();
      beepBeep();
      autoHoldReset();
      filteredGrams = 0.0f; filterInit = true;
      lastWeightDeci = 0; haveLastWeight = true;
      drawWeight(0, false, unitMode, fine01gMode);
      lastDrawMs = millis();
      touchActivity();
    } else if (ev1 == BTN_LONG) {
      autoHoldReset(); mode = MODE_CAL_SELECT; touchActivity();
    }
  }

  if (mode == MODE_NORMAL) {
    if (ev2 == BTN_SHORT) {
      unitMode = (unitMode + 1) % 3;
      prefs.begin(PREF_NAMESPACE, false); prefs.putInt(PREF_KEY_UNIT, unitMode); prefs.end();
      touchActivity();
    } else if (ev2 == BTN_LONG) {
      fine01gMode = !fine01gMode;
      prefs.begin(PREF_NAMESPACE, false); prefs.putInt(PREF_KEY_RES, fine01gMode?1:0); prefs.end();
      filterInit = false; autoHoldReset();
      drawInfo("Dokładność:", fine01gMode ? "0.1 g" : "1 g");
      delay(500);
      touchActivity();
    }
  }

  switch (mode) {
    case MODE_NORMAL: {
      if (!calBusy) {
        bool newSample = false;
        float gramsF = 0.0f;

        if (scale.is_ready()) {
          gramsF = directionSign * scale.get_units(SAMPLES_PER_READ);
          newSample = true;
          lastReadyMs = millis();
        }

        if (newSample) {
          if (!filterInit) { filteredGrams = gramsF; filterInit = true; }
          else { filteredGrams += IIR_ALPHA * (gramsF - filteredGrams); }

          long deci;
          if (fine01gMode) {
            deci = lroundf(filteredGrams * 10.0f);
            if (fabsf(filteredGrams) < 0.05f) deci = 0;
          } else {
            deci = lroundf(filteredGrams) * 10;
            if (fabsf(filteredGrams) < 0.5f) deci = 0;
          }

          if (labs(deci - lastWeightDeci) >= (fine01gMode ? 1 : 10)) touchActivity();

          lastWeightDeci = deci;
          haveLastWeight = true;

          int enterDelta = fine01gMode ? 1 : 10;
          int exitDelta  = fine01gMode ? 3 : 20;
          autoHoldUpdate(deci, enterDelta, exitDelta);
        }

        long toShowDeci = ahActive ? ahValueDeci : lastWeightDeci;

        unsigned long now = millis();
        if (haveLastWeight && (newSample || (now - lastDrawMs) >= DRAW_MIN_INTERVAL_MS)) {
          drawWeight(toShowDeci, ahActive, unitMode, fine01gMode);
          lastDrawMs = now;
        }

        if (!newSample && (millis() - lastReadyMs) >= NOT_READY_TIMEOUT_MS) {
          drawNoHx711();
        }

        if (marqueeActive && (millis() - marqueeStartMs) >= MARQUEE_MAX_MS) {
          stopMarquee();
        }
      }
    } break;

    case MODE_CAL_SELECT: {
      char l1[28]; snprintf(l1, sizeof(l1), "Kalibracja");
      char l2[28]; snprintf(l2, sizeof(l2), "Wybierz: %d g", CAL_WEIGHTS[selWeightIdx]);
      drawInfo(l1, l2);

      if (ev1 == BTN_SHORT)        { selWeightIdx = (selWeightIdx + 1) % CAL_WEIGHTS_COUNT; touchActivity(); }
      else if (ev1 == BTN_LONG)    { mode = MODE_CAL_TARE; touchActivity(); }
    } break;

    case MODE_CAL_TARE: {
      drawInfo("Zdejmij obciążenie", "Długie = TARA");
      if (ev1 == BTN_LONG) {
        scale.tare();
        beepBeep();
        filterInit = false; autoHoldReset();
        delay(200);
        mode = MODE_CAL_PLACE; touchActivity();
      }
    } break;

    case MODE_CAL_PLACE: {
      if (ev2 == BTN_SHORT) { selWeightIdx = (selWeightIdx + 1) % CAL_WEIGHTS_COUNT; touchActivity(); }
      char l1[28]; snprintf(l1, sizeof(l1), "Połóż %d g", CAL_WEIGHTS[selWeightIdx]);
      drawInfo(l1, "Długie = POMIAR");

      if (ev1 == BTN_LONG) {
        long raw = scale.get_value(10);
        int mass = CAL_WEIGHTS[selWeightIdx];

        float newCal = fabsf((float)raw) / (float)mass;
        calibrationFactor = newCal;
        scale.set_scale(calibrationFactor);
        directionSign = (raw >= 0) ? +1 : -1;

        prefs.begin(PREF_NAMESPACE, false);
        prefs.putFloat(PREF_KEY_CAL, calibrationFactor);
        prefs.putInt(PREF_KEY_DIR, directionSign);
        prefs.end();

        filterInit = false; autoHoldReset();
        mode = MODE_CAL_DONE; touchActivity();
      }
    } break;

    case MODE_CAL_DONE: {
      char l1[24]; snprintf(l1, sizeof(l1), "Zapisano!");
      char l2[24]; snprintf(l2, sizeof(l2), "CF=%.3f", calibrationFactor);
      drawInfo(l1, l2);
      delay(800);
      mode = MODE_NORMAL;
    } break;
  }
}