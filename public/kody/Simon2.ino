/*
 * ====================================================
 * GRA SIMON SAYS DLA DZIECI (4-6 lat)
 * ====================================================
 * Kompatybilne z ESP32 Arduino Core 3.x
 * 
 * AP: "Powtorz-kolory"
 * Hasło: "kubaimilosz"
 * Adres: http://192.168.4.1
 * ====================================================
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "esp_random.h"

// ============ KONFIGURACJA WiFi ============

const char* AP_SSID = "Powtorz-kolory";
const char* AP_PASS = "kubaimilosz";

WebServer server(80);
Preferences preferences;

// ============ KONFIGURACJA PINÓW ============

#define LED_CZERWONA  2
#define LED_ZIELONA   4
#define LED_NIEBIESKA 5
#define LED_ZOLTA     18

#define BTN_CZERWONY  13
#define BTN_ZIELONY   12
#define BTN_NIEBIESKI 14
#define BTN_ZOLTY     27

#define BUZZER        15

// ============ KONFIGURACJA LEDC DLA BUZZERA ============

#define BUZZER_RESOLUTION 8
#define BUZZER_FREQ       2000

// ============ USTAWIENIA GRY ============

#define MAX_POZIOM       20
#define MAX_IMION        10
#define MAX_DLUG_IMIENIA 20
#define MAX_TEKST        250

#define TRYB_GOSC        -1
const String IMIE_GOSC = "Ktos:)";

#define MIN_CZAS_SWIECENIA  300

int czasPokazu = 600;
int czasPrzerwy = 300;
int czasOczekiwania = 5000;
int predkoscPrzewijania = 300;

// ============ DŹWIĘKI ============

#define TON_CZERWONY  262
#define TON_ZIELONY   330
#define TON_NIEBIESKI 392
#define TON_ZOLTY     523
#define TON_SUKCES    880
#define TON_PORAZKA   150

// ============ LCD ============

LiquidCrystal_I2C lcd(0x27, 16, 2);

// ============ ZMIENNE GRY ============

int sekwencja[MAX_POZIOM];
int poziom = 0;
bool graAktywna = false;
unsigned long seedDodatkowy = 0;

String imiona[MAX_IMION];
int wyniki[MAX_IMION];
int liczbaImion = 0;
int aktualnyGracz = TRYB_GOSC;
int ostatniGracz = TRYB_GOSC;
int wynikGosc = 0;

String tekstDoWyswietlenia = "";
bool wyswietlanieTekstu = false;
unsigned long czasStartuTekstu = 0;
int pozycjaPrzewijania = 0;
unsigned long ostatniePrzesuniecie = 0;

int ledy[] = {LED_CZERWONA, LED_ZIELONA, LED_NIEBIESKA, LED_ZOLTA};
int przyciski[] = {BTN_CZERWONY, BTN_ZIELONY, BTN_NIEBIESKI, BTN_ZOLTY};
int tony[] = {TON_CZERWONY, TON_ZIELONY, TON_NIEBIESKI, TON_ZOLTY};

// ============ WŁASNE ZNAKI LCD (poprawiona składnia) ============

byte buzkaUsmiech[] = {0b00000, 0b01010, 0b01010, 0b00000, 0b10001, 0b01110, 0b00000, 0b00000};
byte buzkaSmutek[] = {0b00000, 0b01010, 0b01010, 0b00000, 0b01110, 0b10001, 0b00000, 0b00000};
byte gwiazdka[] = {0b00100, 0b01110, 0b11111, 0b01110, 0b01010, 0b10001, 0b00000, 0b00000};
byte serce[] = {0b00000, 0b01010, 0b11111, 0b11111, 0b01110, 0b00100, 0b00000, 0b00000};
byte strzalkaL[] = {0b00010, 0b00110, 0b01110, 0b11110, 0b01110, 0b00110, 0b00010, 0b00000};
byte strzalkaP[] = {0b01000, 0b01100, 0b01110, 0b01111, 0b01110, 0b01100, 0b01000, 0b00000};

// ============ FUNKCJE BUZZERA - NOWE API ESP32 3.x ============

void buzzerInit() {
  // Nowe API ESP32 Arduino Core 3.x
  ledcAttach(BUZZER, BUZZER_FREQ, BUZZER_RESOLUTION);
}

void buzzerTone(int frequency) {
  if (frequency > 0) {
    ledcWriteTone(BUZZER, frequency);
  } else {
    ledcWriteTone(BUZZER, 0);
  }
}

void buzzerNoTone() {
  ledcWriteTone(BUZZER, 0);
}

void buzzerBeep(int frequency, int durationMs) {
  ledcWriteTone(BUZZER, frequency);
  delay(durationMs);
  ledcWriteTone(BUZZER, 0);
}

// ============ FUNKCJE POMOCNICZE DLA GRACZA ============

String pobierzImieGracza() {
  if (aktualnyGracz == TRYB_GOSC) return IMIE_GOSC;
  return imiona[aktualnyGracz];
}

int pobierzRekordGracza() {
  if (aktualnyGracz == TRYB_GOSC) return wynikGosc;
  return wyniki[aktualnyGracz];
}

void zapiszRekordGracza(int nowyWynik) {
  if (aktualnyGracz == TRYB_GOSC) {
    if (nowyWynik > wynikGosc) wynikGosc = nowyWynik;
  } else {
    if (nowyWynik > wyniki[aktualnyGracz]) wyniki[aktualnyGracz] = nowyWynik;
  }
}

void nastepnyGracz() {
  if (liczbaImion == 0) aktualnyGracz = TRYB_GOSC;
  else if (aktualnyGracz == TRYB_GOSC) aktualnyGracz = 0;
  else if (aktualnyGracz >= liczbaImion - 1) aktualnyGracz = TRYB_GOSC;
  else aktualnyGracz++;
}

void poprzedniGracz() {
  if (liczbaImion == 0) aktualnyGracz = TRYB_GOSC;
  else if (aktualnyGracz == TRYB_GOSC) aktualnyGracz = liczbaImion - 1;
  else if (aktualnyGracz <= 0) aktualnyGracz = TRYB_GOSC;
  else aktualnyGracz--;
}

// ============ STRONA HTML ============

const char MAIN_page[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="pl">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Simon Says - Panel</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { font-family: 'Segoe UI', Arial, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; padding: 20px; }
    .container { max-width: 800px; margin: 0 auto; }
    h1 { text-align: center; color: white; margin-bottom: 20px; text-shadow: 2px 2px 4px rgba(0,0,0,0.3); font-size: 2em; }
    .tabs { display: flex; flex-wrap: wrap; gap: 5px; margin-bottom: 20px; }
    .tab-btn { flex: 1; min-width: 100px; padding: 12px 15px; border: none; background: rgba(255,255,255,0.3); color: white; cursor: pointer; border-radius: 10px 10px 0 0; font-size: 14px; font-weight: bold; transition: all 0.3s; }
    .tab-btn:hover { background: rgba(255,255,255,0.5); }
    .tab-btn.active { background: white; color: #667eea; }
    .tab-content { display: none; background: white; padding: 25px; border-radius: 0 0 15px 15px; box-shadow: 0 10px 30px rgba(0,0,0,0.2); }
    .tab-content.active { display: block; }
    h2 { color: #667eea; margin-bottom: 20px; padding-bottom: 10px; border-bottom: 2px solid #eee; }
    .form-group { margin-bottom: 20px; }
    label { display: block; margin-bottom: 8px; font-weight: bold; color: #333; }
    input[type="text"], input[type="number"], textarea { width: 100%; padding: 12px; border: 2px solid #ddd; border-radius: 8px; font-size: 16px; }
    input:focus, textarea:focus { outline: none; border-color: #667eea; }
    textarea { resize: vertical; min-height: 100px; }
    .btn { padding: 12px 25px; border: none; border-radius: 8px; cursor: pointer; font-size: 16px; font-weight: bold; transition: all 0.3s; margin: 5px; }
    .btn-primary { background: #667eea; color: white; }
    .btn-success { background: #28a745; color: white; }
    .btn-danger { background: #dc3545; color: white; }
    .btn-warning { background: #ffc107; color: #333; }
    .btn-group { display: flex; flex-wrap: wrap; gap: 10px; margin-top: 15px; }
    .player-list { list-style: none; }
    .player-item { display: flex; justify-content: space-between; align-items: center; padding: 12px 15px; background: #f8f9fa; margin-bottom: 8px; border-radius: 8px; border-left: 4px solid #667eea; }
    .player-item.guest { background: #fff3cd; border-left-color: #ffc107; }
    .player-name { font-weight: bold; color: #333; }
    .player-score { background: #667eea; color: white; padding: 5px 15px; border-radius: 20px; font-weight: bold; }
    .delete-btn { background: #dc3545; color: white; border: none; padding: 5px 10px; border-radius: 5px; cursor: pointer; margin-left: 10px; }
    .slider-container { margin: 15px 0; }
    .slider-container input[type="range"] { width: 100%; }
    .slider-value { text-align: center; font-size: 18px; font-weight: bold; color: #667eea; margin-top: 5px; }
    .status { padding: 15px; border-radius: 8px; margin: 15px 0; text-align: center; font-weight: bold; }
    .status.success { background: #d4edda; color: #155724; }
    .status.error { background: #f8d7da; color: #721c24; }
    .status.info { background: #cce5ff; color: #004085; }
    .settings-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 20px; }
    .setting-card { background: #f8f9fa; padding: 20px; border-radius: 10px; text-align: center; }
    .current-player { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; padding: 20px; border-radius: 10px; text-align: center; margin-bottom: 20px; }
    .guest-info { background: #fff3cd; border: 2px solid #ffc107; border-radius: 10px; padding: 15px; margin-bottom: 20px; text-align: center; }
    .guest-info h4 { color: #856404; }
    .guest-info p { color: #856404; font-size: 0.9em; }
  </style>
</head>
<body>
  <div class="container">
    <h1>🎮 Simon Says - Panel 🎮</h1>
    <div class="tabs">
      <button class="tab-btn active" onclick="openTab('gracze')">👤 Gracze</button>
      <button class="tab-btn" onclick="openTab('wyniki')">🏆 Wyniki</button>
      <button class="tab-btn" onclick="openTab('ustawienia')">⚙️ Ustawienia</button>
      <button class="tab-btn" onclick="openTab('wiadomosc')">💬 Wiadomość</button>
    </div>
    <div id="gracze" class="tab-content active">
      <h2>👤 Zarządzanie Graczami</h2>
      <div class="current-player"><h3>Aktualny gracz:</h3><div id="currentPlayerName" style="font-size:1.8em;">-</div></div>
      <div class="guest-info"><h4>🎭 Tryb gościa: "Ktoś:)"</h4><p>Rekord gościa: <strong id="guestScore">0</strong> pkt</p></div>
      <div class="form-group"><label>Dodaj gracza (max 20 znaków):</label><input type="text" id="newPlayerName" maxlength="20" placeholder="Wpisz imię..."></div>
      <div class="btn-group"><button class="btn btn-success" onclick="addPlayer()">➕ Dodaj gracza</button></div>
      <div id="playerStatus" class="status" style="display:none;"></div>
      <h3 style="margin:25px 0 15px 0;">Lista graczy (<span id="playerCount">0</span>/10):</h3>
      <ul class="player-list" id="playerList"></ul>
    </div>
    <div id="wyniki" class="tab-content">
      <h2>🏆 Tabela Wyników</h2>
      <ul class="player-list" id="scoreList"></ul>
      <div class="btn-group"><button class="btn btn-warning" onclick="resetScores()">🔄 Resetuj wyniki</button></div>
    </div>
    <div id="ustawienia" class="tab-content">
      <h2>⚙️ Ustawienia Gry</h2>
      <div class="settings-grid">
        <div class="setting-card"><label>Czas świecenia LED (ms):</label><input type="number" id="czasPokazu" min="200" max="2000" step="100"></div>
        <div class="setting-card"><label>Przerwa między kolorami (ms):</label><input type="number" id="czasPrzerwy" min="100" max="1000" step="50"></div>
        <div class="setting-card"><label>Czas na odpowiedź (ms):</label><input type="number" id="czasOczekiwania" min="2000" max="15000" step="500"></div>
        <div class="setting-card"><label>Prędkość przewijania (ms):</label><input type="number" id="predkoscPrzewijania" min="100" max="1000" step="50"></div>
      </div>
      <div class="btn-group" style="margin-top:25px;"><button class="btn btn-primary" onclick="saveSettings()">💾 Zapisz</button><button class="btn btn-warning" onclick="loadDefaultSettings()">🔄 Domyślne</button></div>
      <div id="settingsStatus" class="status" style="display:none;"></div>
    </div>
    <div id="wiadomosc" class="tab-content">
      <h2>💬 Wyślij Wiadomość</h2>
      <div class="form-group"><label>Tekst (max 250 znaków):</label><textarea id="messageText" maxlength="250" placeholder="Wpisz wiadomość..."></textarea><div style="text-align:right;color:#666;"><span id="charCount">0</span>/250</div></div>
      <div class="slider-container"><label>Prędkość przewijania:</label><input type="range" id="scrollSpeed" min="100" max="800" value="300" oninput="document.getElementById('scrollSpeedValue').textContent=this.value"><div class="slider-value"><span id="scrollSpeedValue">300</span> ms</div></div>
      <div class="btn-group"><button class="btn btn-success" onclick="sendMessage()">📤 WYŚLIJ</button><button class="btn btn-danger" onclick="stopMessage()">⏹️ STOP</button><button class="btn btn-warning" onclick="clearMessage()">🗑️ WYCZYŚĆ</button></div>
      <div id="messageStatus" class="status" style="display:none;"></div>
    </div>
  </div>
  <script>
    function openTab(t){document.querySelectorAll('.tab-content').forEach(e=>e.classList.remove('active'));document.querySelectorAll('.tab-btn').forEach(e=>e.classList.remove('active'));document.getElementById(t).classList.add('active');event.target.classList.add('active');if(t==='gracze')loadPlayers();if(t==='wyniki')loadScores();if(t==='ustawienia')loadCurrentSettings();}
    document.getElementById('messageText').addEventListener('input',function(){document.getElementById('charCount').textContent=this.value.length;});
    function loadPlayers(){fetch('/getPlayers').then(r=>r.json()).then(d=>{const l=document.getElementById('playerList');l.innerHTML='';document.getElementById('playerCount').textContent=d.players.length;document.getElementById('currentPlayerName').textContent=d.currentPlayer||'Ktos:)';document.getElementById('guestScore').textContent=d.guestScore||0;d.players.forEach((p,i)=>{l.innerHTML+=`<li class="player-item"><span class="player-name">${i+1}. ${p.name}</span><div><span class="player-score">Rekord: ${p.score}</span><button class="delete-btn" onclick="deletePlayer(${i})">✕</button></div></li>`;});});}
    function addPlayer(){const n=document.getElementById('newPlayerName').value.trim();if(!n){showStatus('playerStatus','Wpisz imię!','error');return;}fetch('/addPlayer?name='+encodeURIComponent(n)).then(r=>r.json()).then(d=>{if(d.success){showStatus('playerStatus','Dodano: '+n,'success');document.getElementById('newPlayerName').value='';loadPlayers();}else{showStatus('playerStatus',d.message,'error');}});}
    function deletePlayer(i){if(confirm('Usunąć?'))fetch('/deletePlayer?index='+i).then(r=>r.json()).then(d=>{if(d.success){showStatus('playerStatus','Usunięto','success');loadPlayers();}});}
    function loadScores(){fetch('/getPlayers').then(r=>r.json()).then(d=>{let a=[...d.players];a.push({name:'🎭 Ktos:)',score:d.guestScore,isGuest:true});a.sort((x,y)=>y.score-x.score);const l=document.getElementById('scoreList');l.innerHTML='';a.forEach((p,i)=>{let m=i===0?'🥇 ':i===1?'🥈 ':i===2?'🥉 ':'';l.innerHTML+=`<li class="player-item ${p.isGuest?'guest':''}"><span class="player-name">${m}${p.name}</span><span class="player-score">${p.score} pkt</span></li>`;});});}
    function resetScores(){if(confirm('Zresetować?'))fetch('/resetScores').then(()=>{loadScores();loadPlayers();});}
    function loadCurrentSettings(){fetch('/getSettings').then(r=>r.json()).then(d=>{document.getElementById('czasPokazu').value=d.czasPokazu;document.getElementById('czasPrzerwy').value=d.czasPrzerwy;document.getElementById('czasOczekiwania').value=d.czasOczekiwania;document.getElementById('predkoscPrzewijania').value=d.predkoscPrzewijania;});}
    function loadDefaultSettings(){document.getElementById('czasPokazu').value=600;document.getElementById('czasPrzerwy').value=300;document.getElementById('czasOczekiwania').value=5000;document.getElementById('predkoscPrzewijania').value=300;showStatus('settingsStatus','Domyślne','info');}
    function saveSettings(){const p=new URLSearchParams({czasPokazu:document.getElementById('czasPokazu').value,czasPrzerwy:document.getElementById('czasPrzerwy').value,czasOczekiwania:document.getElementById('czasOczekiwania').value,predkoscPrzewijania:document.getElementById('predkoscPrzewijania').value});fetch('/saveSettings?'+p).then(()=>showStatus('settingsStatus','Zapisano!','success'));}
    function sendMessage(){const t=document.getElementById('messageText').value,s=document.getElementById('scrollSpeed').value;if(!t.trim()){showStatus('messageStatus','Wpisz tekst!','error');return;}fetch('/sendMessage?text='+encodeURIComponent(t)+'&speed='+s).then(()=>showStatus('messageStatus','Wysłano!','success'));}
    function stopMessage(){fetch('/stopMessage').then(()=>showStatus('messageStatus','Stop','info'));}
    function clearMessage(){document.getElementById('messageText').value='';document.getElementById('charCount').textContent='0';}
    function showStatus(id,msg,type){const e=document.getElementById(id);e.textContent=msg;e.className='status '+type;e.style.display='block';setTimeout(()=>e.style.display='none',3000);}
    loadPlayers();
  </script>
</body>
</html>
)=====";

// ============ FUNKCJE ZAPISYWANIA DANYCH ============

void zapiszDane() {
  preferences.begin("simon", false);
  preferences.putInt("liczbaImion", liczbaImion);
  preferences.putInt("ostatniGracz", ostatniGracz);
  preferences.putInt("wynikGosc", wynikGosc);
  for (int i = 0; i < MAX_IMION; i++) {
    preferences.putString(("imie" + String(i)).c_str(), imiona[i]);
    preferences.putInt(("wynik" + String(i)).c_str(), wyniki[i]);
  }
  preferences.putInt("czasPokazu", czasPokazu);
  preferences.putInt("czasPrzerwy", czasPrzerwy);
  preferences.putInt("czasOczekiwania", czasOczekiwania);
  preferences.putInt("predkoscPrzew", predkoscPrzewijania);
  preferences.end();
}

void wczytajDane() {
  preferences.begin("simon", true);
  liczbaImion = preferences.getInt("liczbaImion", 0);
  ostatniGracz = preferences.getInt("ostatniGracz", TRYB_GOSC);
  wynikGosc = preferences.getInt("wynikGosc", 0);
  for (int i = 0; i < MAX_IMION; i++) {
    imiona[i] = preferences.getString(("imie" + String(i)).c_str(), "");
    wyniki[i] = preferences.getInt(("wynik" + String(i)).c_str(), 0);
  }
  czasPokazu = preferences.getInt("czasPokazu", 600);
  czasPrzerwy = preferences.getInt("czasPrzerwy", 300);
  czasOczekiwania = preferences.getInt("czasOczekiwania", 5000);
  predkoscPrzewijania = preferences.getInt("predkoscPrzew", 300);
  preferences.end();
  aktualnyGracz = ostatniGracz;
  if (aktualnyGracz != TRYB_GOSC && aktualnyGracz >= liczbaImion) aktualnyGracz = TRYB_GOSC;
}

// ============ HANDLERY WWW ============

void handleRoot() { server.send(200, "text/html", MAIN_page); }

void handleGetPlayers() {
  String json = "{\"players\":[";
  for (int i = 0; i < liczbaImion; i++) {
    if (i > 0) json += ",";
    json += "{\"name\":\"" + imiona[i] + "\",\"score\":" + String(wyniki[i]) + "}";
  }
  json += "],\"currentPlayer\":\"" + pobierzImieGracza() + "\",\"guestScore\":" + String(wynikGosc) + "}";
  server.send(200, "application/json", json);
}

void handleAddPlayer() {
  if (!server.hasArg("name")) { server.send(400, "application/json", "{\"success\":false}"); return; }
  if (liczbaImion >= MAX_IMION) { server.send(200, "application/json", "{\"success\":false,\"message\":\"Max 10!\"}"); return; }
  String name = server.arg("name"); name.trim();
  if (name.length() == 0 || name.length() > MAX_DLUG_IMIENIA) { server.send(200, "application/json", "{\"success\":false,\"message\":\"Zla dlugosc\"}"); return; }
  if (name.equalsIgnoreCase("Ktos:)")) { server.send(200, "application/json", "{\"success\":false,\"message\":\"Zarezerwowane!\"}"); return; }
  for (int i = 0; i < liczbaImion; i++) {
    if (imiona[i].equalsIgnoreCase(name)) { server.send(200, "application/json", "{\"success\":false,\"message\":\"Istnieje!\"}"); return; }
  }
  imiona[liczbaImion] = name;
  wyniki[liczbaImion] = 0;
  liczbaImion++;
  zapiszDane();
  server.send(200, "application/json", "{\"success\":true}");
}

void handleDeletePlayer() {
  if (!server.hasArg("index")) { server.send(400, "application/json", "{\"success\":false}"); return; }
  int index = server.arg("index").toInt();
  if (index < 0 || index >= liczbaImion) { server.send(400, "application/json", "{\"success\":false}"); return; }
  for (int i = index; i < liczbaImion - 1; i++) { imiona[i] = imiona[i + 1]; wyniki[i] = wyniki[i + 1]; }
  liczbaImion--;
  if (aktualnyGracz == index) aktualnyGracz = TRYB_GOSC;
  else if (aktualnyGracz > index) aktualnyGracz--;
  zapiszDane();
  server.send(200, "application/json", "{\"success\":true}");
}

void handleResetScores() {
  for (int i = 0; i < liczbaImion; i++) wyniki[i] = 0;
  wynikGosc = 0;
  zapiszDane();
  server.send(200, "application/json", "{\"success\":true}");
}

void handleGetSettings() {
  String json = "{\"czasPokazu\":" + String(czasPokazu) + ",\"czasPrzerwy\":" + String(czasPrzerwy) +
                ",\"czasOczekiwania\":" + String(czasOczekiwania) + ",\"predkoscPrzewijania\":" + String(predkoscPrzewijania) + "}";
  server.send(200, "application/json", json);
}

void handleSaveSettings() {
  if (server.hasArg("czasPokazu")) czasPokazu = constrain(server.arg("czasPokazu").toInt(), 200, 2000);
  if (server.hasArg("czasPrzerwy")) czasPrzerwy = constrain(server.arg("czasPrzerwy").toInt(), 100, 1000);
  if (server.hasArg("czasOczekiwania")) czasOczekiwania = constrain(server.arg("czasOczekiwania").toInt(), 2000, 15000);
  if (server.hasArg("predkoscPrzewijania")) predkoscPrzewijania = constrain(server.arg("predkoscPrzewijania").toInt(), 100, 1000);
  zapiszDane();
  server.send(200, "application/json", "{\"success\":true}");
}

void handleSendMessage() {
  if (!server.hasArg("text")) { server.send(400, "application/json", "{\"success\":false}"); return; }
  tekstDoWyswietlenia = server.arg("text");
  if (server.hasArg("speed")) predkoscPrzewijania = constrain(server.arg("speed").toInt(), 100, 1000);
  wyswietlanieTekstu = true;
  czasStartuTekstu = millis();
  pozycjaPrzewijania = 0;
  server.send(200, "application/json", "{\"success\":true}");
}

void handleStopMessage() {
  wyswietlanieTekstu = false;
  tekstDoWyswietlenia = "";
  server.send(200, "application/json", "{\"success\":true}");
}

// ============ FUNKCJE LOSOWOŚCI ============

void dodajEntropie() {
  seedDodatkowy ^= esp_random();
  seedDodatkowy ^= micros();
}

int losowyKolor() {
  return esp_random() % 4;
}

void generujNowaSekwencje() {
  for (int i = 0; i < MAX_POZIOM; i++) sekwencja[i] = losowyKolor();
}

// ============ FUNKCJE POMOCNICZE ============

void inicjalizujPiny() {
  for (int i = 0; i < 4; i++) {
    pinMode(ledy[i], OUTPUT);
    digitalWrite(ledy[i], LOW);
    pinMode(przyciski[i], INPUT_PULLUP);
  }
}

void zapalWszystkie() { for (int i = 0; i < 4; i++) digitalWrite(ledy[i], HIGH); }
void zgasWszystkie() { for (int i = 0; i < 4; i++) digitalWrite(ledy[i], LOW); }

// ============ ZAPALANIE LED Z DŹWIĘKIEM ============

void zapalLED(int numer, int czasMs) {
  buzzerTone(tony[numer]);
  digitalWrite(ledy[numer], HIGH);
  delay(czasMs);
  digitalWrite(ledy[numer], LOW);
  buzzerNoTone();
}

// ============ SPRAWDZANIE PRZYCISKU ============

int sprawdzPrzycisk() {
  for (int i = 0; i < 4; i++) {
    if (digitalRead(przyciski[i]) == LOW) {
      delay(30);
      if (digitalRead(przyciski[i]) == LOW) {
        dodajEntropie();
        
        buzzerTone(tony[i]);
        digitalWrite(ledy[i], HIGH);
        
        unsigned long czasStart = millis();
        while (digitalRead(przyciski[i]) == LOW) {
          server.handleClient();
          delay(10);
        }
        
        unsigned long czasTrzymania = millis() - czasStart;
        if (czasTrzymania < MIN_CZAS_SWIECENIA) {
          delay(MIN_CZAS_SWIECENIA - czasTrzymania);
        }
        
        digitalWrite(ledy[i], LOW);
        buzzerNoTone();
        delay(50);
        return i;
      }
    }
  }
  return -1;
}

// ============ WYŚWIETLANIE TEKSTU ============

void wyswietlTekstPrzewijanieLoop() {
  if (!wyswietlanieTekstu) return;
  if (millis() - czasStartuTekstu > 60000) { wyswietlanieTekstu = false; return; }
  if (millis() - ostatniePrzesuniecie < (unsigned long)predkoscPrzewijania) return;
  ostatniePrzesuniecie = millis();
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("== WIADOMOSC ==");
  lcd.setCursor(0, 1);
  
  if (tekstDoWyswietlenia.length() <= 16) {
    lcd.print(tekstDoWyswietlenia);
  } else {
    String tekst = tekstDoWyswietlenia + "     ";
    for (int i = 0; i < 16; i++) {
      lcd.print(tekst[(pozycjaPrzewijania + i) % tekst.length()]);
    }
    pozycjaPrzewijania = (pozycjaPrzewijania + 1) % tekst.length();
  }
}

// ============ EKRAN WYBORU GRACZA ============

void wyswietlWyborGracza() {
  lcd.clear();
  lcd.setCursor(0, 0);
  String imie = pobierzImieGracza();
  if (imie.length() > 12) imie = imie.substring(0, 12);
  lcd.write(4);
  lcd.print(" ");
  int pad = (12 - imie.length()) / 2;
  for (int i = 0; i < pad; i++) lcd.print(" ");
  lcd.print(imie);
  for (int i = 0; i < 12 - pad - imie.length(); i++) lcd.print(" ");
  lcd.print(" ");
  lcd.write(5);
  lcd.setCursor(0, 1);
  lcd.print("Rek:");
  lcd.print(pobierzRekordGracza());
  if (aktualnyGracz == TRYB_GOSC) { lcd.print(" "); lcd.write(0); }
  lcd.print(" START!");
}

bool ekranWyboruGracza() {
  wyswietlWyborGracza();
  int animLed = 0;
  unsigned long ostatniaAnimacja = millis();
  
  while (true) {
    server.handleClient();
    if (wyswietlanieTekstu) { wyswietlTekstPrzewijanieLoop(); continue; }
    
    if (millis() - ostatniaAnimacja > 300) {
      zgasWszystkie();
      digitalWrite(ledy[animLed], HIGH);
      animLed = (animLed + 1) % 4;
      ostatniaAnimacja = millis();
      dodajEntropie();
    }
    
    if (digitalRead(BTN_CZERWONY) == LOW) {
      delay(150);
      poprzedniGracz();
      wyswietlWyborGracza();
      buzzerBeep(300, 80);
      while (digitalRead(BTN_CZERWONY) == LOW) delay(10);
    }
    
    if (digitalRead(BTN_ZIELONY) == LOW) {
      delay(150);
      nastepnyGracz();
      wyswietlWyborGracza();
      buzzerBeep(400, 80);
      while (digitalRead(BTN_ZIELONY) == LOW) delay(10);
    }
    
    if (digitalRead(BTN_NIEBIESKI) == LOW || digitalRead(BTN_ZOLTY) == LOW) {
      delay(150);
      zgasWszystkie();
      ostatniGracz = aktualnyGracz;
      zapiszDane();
      buzzerBeep(500, 150);
      return true;
    }
    
    delay(30);
  }
}

// ============ EFEKT SUKCESU ============

void efektSukces() {
  String imie = pobierzImieGracza();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  BRAWO ");
  lcd.print(imie.substring(0, 6));
  lcd.print("!");
  lcd.setCursor(0, 1);
  lcd.print(" SUPER ROBOTA! ");
  lcd.write(2);
  
  for (int i = 0; i < 3; i++) {
    zapalWszystkie();
    buzzerTone(TON_SUKCES);
    delay(150);
    buzzerNoTone();
    zgasWszystkie();
    delay(100);
  }
  delay(800);
}

// ============ EFEKT PORAŻKI ============

void efektPorazka() {
  String imie = pobierzImieGracza();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(" Ups ");
  lcd.print(imie.substring(0, 8));
  lcd.print("!");
  lcd.setCursor(0, 1);
  lcd.print("Sprobuj jeszcze!");
  
  buzzerTone(TON_PORAZKA);
  for (int i = 0; i < 3; i++) {
    zapalWszystkie();
    delay(150);
    zgasWszystkie();
    delay(150);
  }
  buzzerNoTone();
  delay(1500);
}

// ============ POKAZYWANIE SEKWENCJI ============

void pokazSekwencje() {
  String imie = pobierzImieGracza();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  PATRZ ");
  lcd.print(imie.substring(0, 6));
  lcd.print("!");
  lcd.setCursor(0, 1);
  lcd.print("  Zapamietaj!");
  
  delay(1000);
  
  for (int i = 0; i < poziom; i++) {
    server.handleClient();
    
    lcd.setCursor(0, 1);
    lcd.print("Kolor ");
    lcd.print(i + 1);
    lcd.print(" z ");
    lcd.print(poziom);
    lcd.print("    ");
    
    delay(100);
    zapalLED(sekwencja[i], czasPokazu);
    delay(czasPrzerwy);
  }
}

// ============ CZEKANIE NA ODPOWIEDŹ ============

bool czekajNaOdpowiedz() {
  String imie = pobierzImieGracza();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Twoja kolej ");
  lcd.print(imie.substring(0, 4));
  
  delay(200);
  
  for (int krok = 0; krok < poziom; krok++) {
    lcd.setCursor(0, 1);
    lcd.print("Kolor ");
    lcd.print(krok + 1);
    lcd.print(" z ");
    lcd.print(poziom);
    lcd.print("    ");
    
    unsigned long czasStart = millis();
    int nacisniety = -1;
    
    while (nacisniety == -1) {
      server.handleClient();
      nacisniety = sprawdzPrzycisk();
      
      if (millis() - czasStart > (unsigned long)czasOczekiwania) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Za wolno! ");
        lcd.write(1);
        delay(1500);
        return false;
      }
      
      lcd.setCursor(15, 0);
      lcd.print((millis() / 500) % 2 == 0 ? "*" : " ");
    }
    
    if (nacisniety != sekwencja[krok]) return false;
  }
  return true;
}

// ============ POKAZYWANIE WYNIKU ============

void pokazWynik(int wynik) {
  String imie = pobierzImieGracza();
  int staryRekord = pobierzRekordGracza();
  
  if (wynik > staryRekord) {
    zapiszRekordGracza(wynik);
    zapiszDane();
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("NOWY REKORD!!!");
    lcd.write(2);
    lcd.setCursor(0, 1);
    lcd.print(imie.substring(0, 10));
    lcd.print(": ");
    lcd.print(wynik);
    
    for (int i = 0; i < 5; i++) {
      zapalWszystkie();
      buzzerTone(TON_SUKCES + i * 100);
      delay(100);
      buzzerNoTone();
      zgasWszystkie();
      delay(100);
    }
    delay(2000);
  }
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Wynik: ");
  lcd.print(wynik);
  lcd.setCursor(0, 1);
  lcd.print("Rekord: ");
  lcd.print(pobierzRekordGracza());
  delay(3000);
}

// ============ ROZGRYWKA ============

void rozgrywka() {
  String imie = pobierzImieGracza();
  poziom = 0;
  graAktywna = true;
  
  generujNowaSekwencje();
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Gotowy ");
  lcd.print(imie.substring(0, 7));
  lcd.print("?");
  lcd.setCursor(0, 1);
  lcd.print("  3... 2... 1...");
  delay(2000);
  
  while (graAktywna && poziom < MAX_POZIOM) {
    poziom++;
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Poziom: ");
    lcd.print(poziom);
    lcd.print(" ");
    lcd.write(2);
    delay(1000);
    
    pokazSekwencje();
    delay(300);
    
    if (czekajNaOdpowiedz()) {
      efektSukces();
      delay(300);
    } else {
      efektPorazka();
      graAktywna = false;
    }
  }
  
  pokazWynik((poziom > 0) ? poziom - 1 : 0);
}

// ============ SETUP ============

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== SIMON SAYS ===");
  
  inicjalizujPiny();
  buzzerInit();
  
  // Test dźwięku
  Serial.println("Test buzzera...");
  buzzerBeep(440, 200);
  delay(100);
  buzzerBeep(880, 200);
  delay(100);
  
  Wire.begin(21, 22);
  lcd.begin();
  lcd.backlight();
  
  lcd.createChar(0, buzkaUsmiech);
  lcd.createChar(1, buzkaSmutek);
  lcd.createChar(2, gwiazdka);
  lcd.createChar(3, serce);
  lcd.createChar(4, strzalkaL);
  lcd.createChar(5, strzalkaP);
  
  wczytajDane();
  if (aktualnyGracz != TRYB_GOSC && aktualnyGracz >= liczbaImion) aktualnyGracz = TRYB_GOSC;
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Uruchamiam WiFi");
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress IP = WiFi.softAPIP();
  
  server.on("/", handleRoot);
  server.on("/getPlayers", handleGetPlayers);
  server.on("/addPlayer", handleAddPlayer);
  server.on("/deletePlayer", handleDeletePlayer);
  server.on("/resetScores", handleResetScores);
  server.on("/getSettings", handleGetSettings);
  server.on("/saveSettings", handleSaveSettings);
  server.on("/sendMessage", handleSendMessage);
  server.on("/stopMessage", handleStopMessage);
  server.begin();
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi:");
  lcd.print(AP_SSID);
  lcd.setCursor(0, 1);
  lcd.print("IP:");
  lcd.print(IP);
  delay(3000);
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Haslo:");
  lcd.setCursor(0, 1);
  lcd.print(AP_PASS);
  delay(3000);
}

// ============ LOOP ============

void loop() {
  server.handleClient();
  
  if (wyswietlanieTekstu) {
    wyswietlTekstPrzewijanieLoop();
    for (int i = 0; i < 4; i++) {
      if (digitalRead(przyciski[i]) == LOW) {
        wyswietlanieTekstu = false;
        delay(300);
        break;
      }
    }
    return;
  }
  
  if (ekranWyboruGracza()) {
    rozgrywka();
  }
}