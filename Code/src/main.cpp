// 1.1v
#include <Arduino.h>
#include <Wire.h>
#include <GyverOLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <vector>
#include <algorithm>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <math.h>

#define PIN_SDA   11
#define PIN_SCL   13
#define PIN_UP    5
#define PIN_DOWN  7
#define PIN_OK    6

GyverOLED<SSD1306_128x64, OLED_BUFFER> oled;

const int SCREEN_ROWS = 8;
const int CHARS_PER_LINE = 21;

String AP_SSID = "Esp hpora";
String AP_PASS = "12345678";
IPAddress apIP(192, 168, 4, 1);
DNSServer dnsServer;
WebServer server(80);
bool wifiActive = false;

bool leftHanded = false;
bool sleepEnabled = false;
int sleepMin = 0;
int sleepSec = 0;
unsigned long sleepTimeoutMs = 0;
unsigned long lastActivity = 0;
bool isSleeping = false;
bool justWokeUp = false;
int webTheme = 0;

enum AppState { ST_LIST, ST_VIEW, ST_WIFI, ST_CALC };
AppState state = ST_LIST;

struct FileEntry { 
  String fname; 
  String title; 
  size_t size;
  bool isDir;
};
std::vector<FileEntry> fileEntries;
String currentDir = "/";

int selected  = 0;
int listStart = 0;

String currentFileName = "";
std::vector<uint32_t> pageOffsets;
bool currentFileEmpty = false;
int page = 0;

uint8_t brightness = 200;
const uint8_t BRIGHTNESS_STEP = 15;
String lastFileName = "";

bool settingsDirty = false;
unsigned long settingsChangedAt = 0;
const unsigned long SETTINGS_SAVE_DELAY = 800;
const char* SETTINGS_PATH = "/_cfg.txt";

int scrollPos = 0;
unsigned long scrollTimer = 0;
const unsigned long SCROLL_INTERVAL = 300;
int lastScrollIdx = -1;

const int DEBOUNCE_MAX = 6;
const unsigned long LONGPRESS_MS    = 600;
const unsigned long REPEAT_START_MS = 500;
const unsigned long REPEAT_MS       = 180;

struct Btn {
  uint8_t pin;
  int counter;
  bool stable;
  unsigned long tPress, tRepeat;
  bool longFired;
  bool pressedEvent, releasedEvent, longEvent, repeatEvent;
};

Btn btnUp, btnDown, btnOk;

const char* calcKeys[5][4] = {
  { "C",   "^", "sqrt", "DEL" },
  { "7",   "8", "9",    "/"   },
  { "4",   "5", "6",    "*"   },
  { "1",   "2", "3",    "-"   },
  { "0",   ".", "=",    "+"   }
};

int calcSel = 0;
String calcDisp = "";
String calcResult = "";

void btnInit(Btn &b, uint8_t pin);
void btnUpdate(Btn &b);
void handleButtons();
void moveSelection(int dir);
void openSelectedFile();
void prevPage();
void nextPage();
void changeBrightness(int delta);
void toggleWifi();
void drawScreen();
void drawList();
void drawView();
void drawWifiInfo();
void drawCalc();
void applyOrientation();

void calcMoveSel(int dir);
void calcProcessKey(String key);

int utf8Length(const String &s);
String utf8Substring(const String &s, int startChar, int charCount);
String marqueeText(const String &title, int width, int offset);

bool lsReadWord(File &f, String &word, bool &isBreak);
bool lsNextLine(File &f, String &line, uint32_t &lineStartPos);
void lsReset();
void buildPageIndex(const String &fname);

void markSettingsDirty();
void maybeSaveSettings();
void loadSettings();
void saveSettings();

String pathOf(const String &fn);
void refreshFileList();
struct FileData;
FileData readFile(const String &fname);
void writeFile(const String &fname, String title, const String &content);
String genFilename(String dir);
bool hasSpace(long extraBytes);
bool removeDirRecursive(String path);

String htmlEscape(String s);
void redirectMsg(const String &path, const char *m);
void handleRedirect();
String pageStyle();
String navBar(const String &active);
void handleRoot();
void handleSettingsPage();
void handleUpload();
void handleMkdir();
void handleRmdir();
void handleEditPage();
void handleSave();
void handleDelete();
void handleSaveWifi();
void handleSaveLefty();
void handleSaveSleep();
void handleSaveTheme();
void setupWebServer();

struct FileData { String title; String content; };

void btnInit(Btn &b, uint8_t pin) {
  b.pin = pin;
  pinMode(pin, INPUT_PULLUP);
  bool pressedNow = (digitalRead(pin) == LOW);
  b.counter = pressedNow ? DEBOUNCE_MAX : 0;
  b.stable = pressedNow;
  b.tPress = 0;
  b.tRepeat = 0;
  b.longFired = false;
  b.pressedEvent = b.releasedEvent = b.longEvent = b.repeatEvent = false;
}

void btnUpdate(Btn &b) {
  b.pressedEvent = b.releasedEvent = b.longEvent = b.repeatEvent = false;
  bool pressedNow = (digitalRead(b.pin) == LOW);
  if (pressedNow) { if (b.counter < DEBOUNCE_MAX) b.counter++; } 
  else { if (b.counter > 0) b.counter--; }

  bool newStable = b.stable;
  if (b.counter >= DEBOUNCE_MAX) newStable = true;
  else if (b.counter <= 0) newStable = false;

  if (newStable != b.stable) {
    b.stable = newStable;
    if (b.stable) {
      b.tPress = b.tRepeat = millis();
      b.longFired = false;
      b.pressedEvent = true;
    } else {
      b.releasedEvent = true;
    }
  }
  if (b.stable) {
    unsigned long held = millis() - b.tPress;
    if (!b.longFired && held >= LONGPRESS_MS) {
      b.longFired = true;
      b.longEvent = true;
    }
    if (held >= REPEAT_START_MS && millis() - b.tRepeat >= REPEAT_MS) {
      b.tRepeat = millis();
      b.repeatEvent = true;
    }
  }
}

void applyOrientation() {
  oled.flipH(leftHanded);
  oled.flipV(leftHanded);
}

void setup() {
  Serial.begin(9600);
  randomSeed(micros());

  btnInit(btnUp,   PIN_UP);
  btnInit(btnDown, PIN_DOWN);
  btnInit(btnOk,   PIN_OK);

  Wire.begin(PIN_SDA, PIN_SCL);
  oled.init();
  oled.clear();
  oled.update();

  if (!LittleFS.begin(true)) {
    Serial.println("Ошибка LittleFS");
  }

  WiFi.mode(WIFI_OFF);
  setupWebServer();
  refreshFileList();
  loadSettings();
  oled.setContrast(brightness);
  applyOrientation();

  if (lastFileName.length() > 0) {
    for (size_t i = 0; i < fileEntries.size(); i++) {
      if (fileEntries[i].fname == lastFileName) { selected = i; break; }
    }
  }
  esp_wifi_set_max_tx_power(WIFI_POWER_2dBm);
  lastActivity = millis();
  drawScreen();
}

void enterLightSleep() {
  oled.setPower(false);
  isSleeping = true;
  gpio_wakeup_enable((gpio_num_t)PIN_UP, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)PIN_DOWN, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)PIN_OK, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_light_sleep_start();
  isSleeping = false;
  justWokeUp = true;
  lastActivity = millis();
  oled.setPower(true);
  drawScreen();
}

void loop() {
  btnUpdate(btnUp);
  btnUpdate(btnDown);
  btnUpdate(btnOk);

  if (wifiActive) {
    dnsServer.processNextRequest();
    server.handleClient();
  } else if (sleepEnabled && sleepTimeoutMs > 0) {
    if (millis() - lastActivity >= sleepTimeoutMs) {
      enterLightSleep();
    }
  }

  handleButtons();

  if (!isSleeping && state == ST_LIST && !fileEntries.empty()) {
    String title = fileEntries[selected].title;
    if (utf8Length(title) > CHARS_PER_LINE - 2 && millis() - scrollTimer > SCROLL_INTERVAL) {
      scrollTimer = millis();
      scrollPos++;
      drawScreen();
    }
  }
  maybeSaveSettings();
  delay(5);
}

void handleButtons() {
  bool anyPressedNow = btnUp.stable || btnDown.stable || btnOk.stable;
  bool anyPressedEvent = btnUp.pressedEvent || btnDown.pressedEvent || btnOk.pressedEvent;

  if (justWokeUp) {
    if (!anyPressedNow) justWokeUp = false;
    return;
  }

  if (anyPressedNow || anyPressedEvent || btnUp.repeatEvent || btnDown.repeatEvent) {
    lastActivity = millis();
  }

  static unsigned long bothTimer = 0;
  if (btnUp.stable && btnDown.stable) {
    if (bothTimer == 0) {
      bothTimer = millis();
    } else if (millis() - bothTimer > 400 && state != ST_CALC) {
      state = ST_CALC;
      calcDisp = ""; calcResult = ""; calcSel = 0;
      bothTimer = millis() + 2000; 
      drawScreen();
    }
    return;
  } else {
    bothTimer = 0;
  }

  Btn &navUp = leftHanded ? btnDown : btnUp;
  Btn &navDown = leftHanded ? btnUp : btnDown;

  switch (state) {
    case ST_LIST:
      if (navUp.pressedEvent   || navUp.repeatEvent)   moveSelection(-1);
      if (navDown.pressedEvent || navDown.repeatEvent)  moveSelection(1);
      if (btnOk.releasedEvent && !btnOk.longFired)      openSelectedFile();
      if (btnOk.longEvent)                              toggleWifi();
      break;

    case ST_VIEW:
      if (navUp.pressedEvent   || navUp.repeatEvent)   prevPage();
      if (navDown.pressedEvent || navDown.repeatEvent)  nextPage();
      if (btnOk.releasedEvent && !btnOk.longFired) {
        state = ST_LIST;
        drawScreen();
      }
      break;

    case ST_WIFI:
      if (navUp.pressedEvent   || navUp.repeatEvent)   changeBrightness(BRIGHTNESS_STEP);
      if (navDown.pressedEvent || navDown.repeatEvent)  changeBrightness(-BRIGHTNESS_STEP);
      if (btnOk.longEvent)                              toggleWifi();
      break;
      
    case ST_CALC:
      if (navUp.pressedEvent || navUp.repeatEvent) { calcMoveSel(-1); drawScreen(); }
      if (navDown.pressedEvent || navDown.repeatEvent) { calcMoveSel(1); drawScreen(); }
      if (btnOk.releasedEvent && !btnOk.longFired) {
        int row = calcSel / 4;
        int col = calcSel % 4;
        String key = String(calcKeys[row][col]);
        calcProcessKey(key);
        drawScreen();
      }
      if (btnOk.longEvent) { state = ST_LIST; drawScreen(); }
      break;
  }
}

void moveSelection(int dir) {
  int total = fileEntries.size();
  if (total == 0) return;
  selected += dir;
  if (selected < 0) selected = total - 1;
  if (selected >= total) selected = 0;
  scrollPos = 0;
  lastScrollIdx = selected;
  markSettingsDirty();
  drawScreen();
}

void openSelectedFile() {
  if (fileEntries.empty()) return;
  if (fileEntries[selected].isDir) {
    if (fileEntries[selected].fname == "..") {
      int lastSlash = currentDir.lastIndexOf('/', currentDir.length() - 2);
      if (lastSlash >= 0) currentDir = currentDir.substring(0, lastSlash + 1);
      else currentDir = "/";
    } else {
      currentDir = fileEntries[selected].fname;
    }
    if (!currentDir.endsWith("/")) currentDir += "/";
    selected = 0;
    refreshFileList();
    drawScreen();
    return;
  }
  
  buildPageIndex(fileEntries[selected].fname);
  page = 0;
  state = ST_VIEW;
  markSettingsDirty();
  drawScreen();
}

void prevPage() { if (page > 0) { page--; drawScreen(); } }
void nextPage() { if (page < (int)pageOffsets.size() - 1) { page++; drawScreen(); } }

void changeBrightness(int delta) {
  int v = (int)brightness + delta;
  v = constrain(v, 5, 255);
  brightness = (uint8_t)v;
  oled.setContrast(brightness);
  markSettingsDirty();
  drawScreen();
}

void toggleWifi() {
  wifiActive = !wifiActive;
  if (wifiActive) {
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(AP_SSID.c_str(), AP_PASS.length() > 0 ? AP_PASS.c_str() : NULL);
    dnsServer.start(53, "*", apIP);
    server.begin();
    state = ST_WIFI;
  } else {
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    state = ST_LIST;
    refreshFileList();
    if (selected >= (int)fileEntries.size()) selected = 0;
  }
  lastActivity = millis();
  drawScreen();
}

double applyCalcOp(double a, double b, char op) {
  switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return b != 0 ? a / b : 0;
  }
  return 0;
}

int calcPrecedence(char op) {
  if (op == '+' || op == '-') return 1;
  if (op == '*' || op == '/') return 2;
  return 0;
}

void calcMoveSel(int dir) {
  const int COLS = 4;
  const int ROWS = 5;

  int row = calcSel / COLS;
  int col = calcSel % COLS;

  int vIdx = col * ROWS + row;
  vIdx += dir;
  if (vIdx < 0) vIdx = ROWS * COLS - 1;
  if (vIdx >= ROWS * COLS) vIdx = 0;

  int newCol = vIdx / ROWS;
  int newRow = vIdx % ROWS;
  calcSel = newRow * COLS + newCol;
}

struct CalcParser {
  const char* s;
  bool error;

  CalcParser(const String &str) : s(str.c_str()), error(false) {}

  void skipSpaces() { while (*s == ' ') s++; }

  double parseExpression() {
    double val = parseTerm();
    skipSpaces();
    while (*s == '+' || *s == '-') {
      char op = *s; s++;
      double rhs = parseTerm();
      val = (op == '+') ? val + rhs : val - rhs;
      skipSpaces();
    }
    return val;
  }

  double parseTerm() {
    double val = parsePower();
    skipSpaces();
    while (*s == '*' || *s == '/') {
      char op = *s; s++;
      double rhs = parsePower();
      if (op == '*') {
        val *= rhs;
      } else {
        if (rhs == 0) { error = true; val = 0; }
        else val /= rhs;
      }
      skipSpaces();
    }
    return val;
  }

  double parsePower() {
    double base = parseUnary();
    skipSpaces();
    if (*s == '^') {
      s++;
      double exp = parsePower(); 
      return pow(base, exp);
    }
    return base;
  }

  double parseUnary() {
    skipSpaces();
    if (*s == '-') { s++; return -parseUnary(); }
    if (*s == '+') { s++; return parseUnary(); }
    if (strncmp(s, "sqrt", 4) == 0) {
      s += 4;
      double val = parseUnary(); 
      return sqrt(val);          
    }
    char* endPtr;
    double val = strtod(s, &endPtr);
    if (endPtr == s) { error = true; return 0; }
    s = endPtr;
    return val;
  }
};

String formatCalcNumber(double val) {
  if (isnan(val) || isinf(val)) return "Ошибка";

  const int MAX_LEN = CHARS_PER_LINE - 1; 

  char buf[64];
  snprintf(buf, sizeof(buf), "%.14g", val);
  String out = String(buf);

  if ((int)out.length() > MAX_LEN && out.indexOf('.') != -1 && out.indexOf('e') == -1) {
    int dotPos = out.indexOf('.');
    int keepDecimals = MAX_LEN - dotPos - 1;
    if (keepDecimals < 0) keepDecimals = 0;
    char buf2[64];
    snprintf(buf2, sizeof(buf2), "%.*f", keepDecimals, val);
    out = String(buf2);
    if (out.indexOf('.') != -1) {
      while (out.endsWith("0")) out.remove(out.length() - 1);
      if (out.endsWith(".")) out.remove(out.length() - 1);
    }
  }

  return out;
}

void calcProcessKey(String key) {
  if (key == "DEL") {
    if (calcDisp.endsWith("sqrt")) {
      calcDisp.remove(calcDisp.length() - 4, 4);
    } else if (calcDisp.length() > 0) {
      calcDisp.remove(calcDisp.length() - 1);
    }
    calcResult = "";
  } 
  else if (key == "C") {
    calcDisp = "";
    calcResult = "";
  } 
  else if (key == "=") {
    if (calcDisp.length() == 0) return;
    CalcParser p(calcDisp);
    double result = p.parseExpression();
    p.skipSpaces();
    if (p.error || *p.s != '\0') {
      calcResult = "Ошибка";
    } else {
      calcResult = formatCalcNumber(result);
    }
  } 
  else {
    if (calcDisp.length() < 20) {
      calcDisp += key;
    }
    calcResult = "";
  }
}

void drawScreen() {
  if (isSleeping) return;
  oled.clear();
  switch (state) {
    case ST_LIST: drawList(); break;
    case ST_VIEW: drawView(); break;
    case ST_WIFI: drawWifiInfo(); break;
    case ST_CALC: drawCalc(); break;
  }
  oled.update();
}

void drawCalc() {
  String printStr = (calcResult.length() > 0) ? ("=" + calcResult) : calcDisp;
  if (printStr.length() > 22) {
    printStr = printStr.substring(printStr.length() - 21);
  }
  oled.setCursor(0, 0);
  oled.print(printStr);

  oled.line(0, 20, 127, 20);

  const int COLS      = 4;
  const int ROWS       = 5;
  const int BTN_W       = 32;  
  const int CHAR_W     = 6;   
  const int CHAR_H     = 8;   
  const int BTN_CHARS = 5;   
  const int FIRST_ROW  = 3;   

  for (int row = 0; row < ROWS; row++) {
    for (int col = 0; col < COLS; col++) {
      const char* label = calcKeys[row][col];
      int len = strlen(label);
      if (len > BTN_CHARS) len = BTN_CHARS;

      char buf[BTN_CHARS + 1];
      memset(buf, ' ', BTN_CHARS);
      buf[BTN_CHARS] = '\0';
      int padLeft = (BTN_CHARS - len) / 2;
      memcpy(buf + padLeft, label, len);

      int colX  = col * BTN_W;
      int textX = colX + (BTN_W - BTN_CHARS * CHAR_W) / 2;
      int lineY = FIRST_ROW + row;
      int btnY  = lineY * CHAR_H;

      int idx = row * COLS + col;
      bool selected = (idx == calcSel);

      if (selected) {
        int rx0 = colX + 1;
        int rx1 = colX + BTN_W - 2;
        int ry0 = btnY - 1;             
        int ry1 = btnY + CHAR_H - 1;

        oled.rect(rx0, ry0, rx1, ry1, OLED_FILL);
        oled.invertText(true);
        oled.setCursor(textX, lineY);
        oled.print(buf);
        oled.invertText(false);
      } else {
        oled.setCursor(textX, lineY);
        oled.print(buf);
      }
    }
  }
}

void drawList() {
  const int VIS = SCREEN_ROWS;
  const int MARGIN = 1;
  const int WIDTH = CHARS_PER_LINE - 2;
  int total = fileEntries.size();

  if (total == 0) {
    oled.setCursor(0, 3);
    oled.print("Нет файлов");
    return;
  }
  if (selected < listStart + MARGIN) listStart = max(0, selected - MARGIN);
  if (selected > listStart + VIS - 1 - MARGIN) listStart = selected - VIS + 1 + MARGIN;
  int maxStart = max(0, total - VIS);
  listStart = constrain(listStart, 0, maxStart);

  for (int row = 0; row < VIS && (listStart + row) < total; row++) {
    int idx = listStart + row;
    String title = fileEntries[idx].title;
    int titleChars = utf8Length(title);
    String prefix = (idx == selected) ? "> " : "  ";
    String shown;
    if (titleChars <= WIDTH) shown = title;
    else if (idx == selected) {
      if (lastScrollIdx != selected) { scrollPos = 0; lastScrollIdx = selected; }
      shown = marqueeText(title, WIDTH, scrollPos);
    } else {
      shown = utf8Substring(title, 0, WIDTH);
    }
    oled.setCursor(0, row);
    oled.print(prefix + shown);
  }
}

void drawView() {
  if (currentFileEmpty || currentFileName.length() == 0) {
    oled.setCursor(0, 3);
    oled.print("Пусто");
    return;
  }
  File f = LittleFS.open(pathOf(currentFileName), "r");
  if (!f) {
    oled.setCursor(0, 3);
    oled.print("Ошибка чтения");
    return;
  }
  uint32_t offset = (page >= 0 && page < (int)pageOffsets.size()) ? pageOffsets[page] : 0;
  f.seek(offset);
  lsReset();
  String line;
  uint32_t dummy;
  for (int row = 0; row < SCREEN_ROWS; row++) {
    if (!lsNextLine(f, line, dummy)) break;
    oled.setCursor(0, row);
    oled.print(line);
  }
  f.close();
}

void drawWifiInfo() {
  oled.setCursor(0, 0); oled.print("WiFi: " + AP_SSID);
  oled.setCursor(0, 2); oled.print("IP: " + WiFi.softAPIP().toString());
  oled.setCursor(0, 4); oled.print("Pass: " + (AP_PASS.length() ? AP_PASS : String("None")));
  oled.setCursor(0, 6); oled.print("Яркость: " + String(brightness));
}

int utf8Length(const String &s) {
  int count = 0; size_t i = 0;
  while (i < s.length()) {
    uint8_t c = (uint8_t)s[i];
    if (c < 0x80) i += 1;
    else if ((c & 0xE0) == 0xC0) i += 2;
    else if ((c & 0xF0) == 0xE0) i += 3;
    else if ((c & 0xF8) == 0xF0) i += 4;
    else i += 1;
    count++;
  }
  return count;
}

String utf8Substring(const String &s, int startChar, int charCount) {
  int idxChar = 0; size_t i = 0;
  int byteStart = -1; size_t byteEnd = s.length();
  while (i < s.length()) {
    if (idxChar == startChar) byteStart = (int)i;
    if (idxChar == startChar + charCount) { byteEnd = i; break; }
    uint8_t c = (uint8_t)s[i];
    if (c < 0x80) i += 1;
    else if ((c & 0xE0) == 0xC0) i += 2;
    else if ((c & 0xF0) == 0xE0) i += 3;
    else if ((c & 0xF8) == 0xF0) i += 4;
    else i += 1;
    idxChar++;
  }
  if (idxChar == startChar && byteStart == -1) byteStart = (int)i;
  if (byteStart == -1) return "";
  return s.substring(byteStart, byteEnd);
}

String marqueeText(const String &title, int width, int offset) {
  const String sep = "    ";
  int period = utf8Length(title) + utf8Length(sep);
  if (period <= 0) return title;
  String loop = title + sep + title + sep;
  int pos = ((offset % period) + period) % period;
  return utf8Substring(loop, pos, width);
}

bool lsHavePending = false; String lsPendingWord = ""; bool lsPendingBreak = false;
void lsReset() { lsHavePending = false; lsPendingWord = ""; lsPendingBreak = false; }

bool lsReadWord(File &f, String &word, bool &isBreak) {
  word = ""; isBreak = false; int ch;
  while ((ch = f.read()) != -1) {
    if (ch == ' ' || ch == '\r') { if (word.length() == 0) continue; return true; }
    if (ch == '\n') { isBreak = true; return true; }
    word += (char)ch;
    if (utf8Length(word) >= CHARS_PER_LINE) return true;
  }
  return word.length() > 0;
}

bool lsNextLine(File &f, String &line, uint32_t &lineStartPos) {
  line = ""; bool gotAny = false; bool startCaptured = false;
  while (true) {
    if (!lsHavePending) {
      uint32_t before = f.position();
      if (!lsReadWord(f, lsPendingWord, lsPendingBreak)) return gotAny;
      if (!startCaptured) { lineStartPos = before; startCaptured = true; }
      lsHavePending = true;
    } else if (!startCaptured) {
      lineStartPos = f.position() - lsPendingWord.length() - (lsPendingBreak ? 1 : 0);
      startCaptured = true;
    }
    if (lsPendingWord.length() == 0) { lsHavePending = false; return true; }
    int extra = (utf8Length(line) > 0 ? 1 : 0) + utf8Length(lsPendingWord);
    if (utf8Length(line) + extra <= CHARS_PER_LINE) {
      if (line.length() > 0) line += ' ';
      line += lsPendingWord; gotAny = true; lsHavePending = false;
      if (lsPendingBreak) return true;
    } else return true;
  }
}

void buildPageIndex(const String &fname) {
  pageOffsets.clear(); currentFileName = fname; currentFileEmpty = false;
  File f = LittleFS.open(pathOf(fname), "r");
  if (!f) { currentFileEmpty = true; pageOffsets.push_back(0); return; }
  f.readStringUntil('\n'); lsReset();
  pageOffsets.push_back(f.position());
  String line; uint32_t lineStart; int rowInPage = 0; bool any = false;
  while (lsNextLine(f, line, lineStart)) {
    any = true;
    if (rowInPage == SCREEN_ROWS) { pageOffsets.push_back(lineStart); rowInPage = 0; }
    rowInPage++;
  }
  f.close();
  currentFileEmpty = !any;
}

void markSettingsDirty() { settingsDirty = true; settingsChangedAt = millis(); }
void maybeSaveSettings() {
  if (settingsDirty && millis() - settingsChangedAt > SETTINGS_SAVE_DELAY) {
    saveSettings(); settingsDirty = false;
  }
}

void loadSettings() {
  if (!LittleFS.exists(SETTINGS_PATH)) return;
  File f = LittleFS.open(SETTINGS_PATH, "r");
  if (!f) return;
  String l1 = f.readStringUntil('\n'); l1.trim();
  String l2 = f.readStringUntil('\n'); l2.trim();
  String l3 = f.readStringUntil('\n'); l3.trim();
  String l4 = f.readStringUntil('\n'); l4.trim();
  String l5 = f.readStringUntil('\n'); l5.trim();
  String l6 = f.readStringUntil('\n'); l6.trim();
  String l7 = f.readStringUntil('\n'); l7.trim();
  String l8 = f.readStringUntil('\n'); l8.trim();
  String l9 = f.readStringUntil('\n'); l9.trim();
  f.close();
  if (l1.length() > 0) { int v = l1.toInt(); if (v >= 5 && v <= 255) brightness = (uint8_t)v; }
  lastFileName = l2;
  if (l3.length() > 0) leftHanded = (l3.toInt() == 1);
  if (l4.length() > 0) AP_SSID = l4;
  if (l5.length() >= 0) AP_PASS = l5;
  if (l6.length() > 0) sleepMin = l6.toInt();
  if (l7.length() > 0) sleepSec = l7.toInt();
  if (l8.length() > 0) sleepEnabled = (l8.toInt() == 1);
  if (l9.length() > 0) webTheme = constrain(l9.toInt(), 0, 8);
  sleepTimeoutMs = ((unsigned long)sleepMin * 60 + sleepSec) * 1000UL;
}

void saveSettings() {
  String fnameToSave = "";
  if (!fileEntries.empty() && selected >= 0 && selected < (int)fileEntries.size() && !fileEntries[selected].isDir) {
    fnameToSave = fileEntries[selected].fname;
  }
  File f = LittleFS.open(SETTINGS_PATH, "w");
  if (!f) return;
  f.println(String(brightness)); f.println(fnameToSave); f.println(leftHanded ? "1" : "0");
  f.println(AP_SSID); f.println(AP_PASS); f.println(String(sleepMin));
  f.println(String(sleepSec)); f.println(sleepEnabled ? "1" : "0"); f.println(String(webTheme));
  f.close();
}

String pathOf(const String &fn) { return fn.startsWith("/") ? fn : "/" + fn; }

void refreshFileList() {
  fileEntries.clear();
  if (currentDir != "/") fileEntries.push_back({"..", ".. (Назад)", 0, true});
  
  File root = LittleFS.open(currentDir);
  if (!root) return;
  File f = root.openNextFile();
  while (f) {
    String path = f.path();
    if (path.length() == 0) {
      path = currentDir + String(f.name());
      path.replace("//", "/");
    }
    if (path.indexOf("_cfg.txt") == -1) {
      if (f.isDirectory()) {
        String shortName = path.substring(path.lastIndexOf('/') + 1);
        fileEntries.push_back({path, "[" + shortName + "]", 0, true});
      } else {
        String title = f.readStringUntil('\n'); title.trim();
        if (title.length() == 0) title = "Без названия";
        fileEntries.push_back({path, title, f.size(), false});
      }
    }
    f = root.openNextFile();
  }

  std::sort(fileEntries.begin(), fileEntries.end(), [](const FileEntry &a, const FileEntry &b) {
    if (a.fname == "..") return true; if (b.fname == "..") return false;
    if (a.isDir && !b.isDir) return true; if (!a.isDir && b.isDir) return false;
    return a.title < b.title;
  });
}

FileData readFile(const String &fname) {
  FileData d; File f = LittleFS.open(pathOf(fname), "r");
  if (!f) return d;
  d.title = f.readStringUntil('\n'); d.title.trim();
  d.content = f.readString(); f.close(); return d;
}

void writeFile(const String &fname, String title, const String &content) {
  title.replace("\n", " "); title.replace("\r", "");
  File f = LittleFS.open(pathOf(fname), "w");
  if (!f) return;
  f.println(title); f.print(content); f.close();
}

String genFilename(String dir) {
  if (!dir.endsWith("/")) dir += "/";
  unsigned long maxId = 0;
  File root = LittleFS.open(dir);
  if (root) {
    File f = root.openNextFile();
    while (f) {
      String n = f.name();
      if (!f.isDirectory() && n.indexOf("d_") != -1) {
        int idx = n.lastIndexOf("d_") + 2;
        unsigned long id = n.substring(idx).toInt();
        if (id > maxId) maxId = id;
      }
      f = root.openNextFile();
    }
  }
  return dir + "d_" + String(maxId + 1) + ".txt";
}

bool hasSpace(long extraBytes) {
  return ((long)LittleFS.totalBytes() - (long)LittleFS.usedBytes() - extraBytes) > 512;
}

bool removeDirRecursive(String path) {
  if (!path.endsWith("/")) path += "/";

  File dir = LittleFS.open(path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }

  bool allOk = true;
  File f = dir.openNextFile();
  while (f) {
    String childPath = f.path();
    if (childPath.length() == 0) {
      childPath = path + String(f.name());
      childPath.replace("//", "/");
    }
    bool childIsDir = f.isDirectory();
    f.close();

    if (childIsDir) {
      if (!removeDirRecursive(childPath)) allOk = false;
    } else {
      if (!LittleFS.remove(childPath)) allOk = false;
    }
    f = dir.openNextFile();
  }
  dir.close();

  if (!LittleFS.rmdir(path)) allOk = false;
  return allOk;
}

String htmlEscape(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;"); s.replace("\"", "&quot;");
  return s;
}

void redirectMsg(const String &path, const char *m) {
  server.sendHeader("Location", path + "&msg=" + m, true);
  server.send(302, "text/plain", "");
}

void handleRedirect() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

String pageStyle() {
  String css = F("*{box-sizing:border-box;margin:0;padding:0}:root{");
  switch(webTheme) {
      case 1: css += F("--bg:#121212;--card:#1e1e1e;--txt:#e6e6e6;--acc:#c9a876;--brd:#2c2c2c;"); break;
      case 2: css += F("--bg:#0d1117;--card:#161b22;--txt:#c9d1d9;--acc:#3fb950;--brd:#30363d;"); break;
      case 3: css += F("--bg:#282a36;--card:#353746;--txt:#f0f0f5;--acc:#bd93f9;--brd:#565873;"); break;
      case 4: css += F("--bg:#2e3440;--card:#3b4252;--txt:#e5e9f0;--acc:#81a1c1;--brd:#4c566a;"); break;
      case 5: css += F("--bg:#002b36;--card:#073642;--txt:#93a1a1;--acc:#2aa198;--brd:#586e75;"); break;
      case 6: css += F("--bg:#1f1216;--card:#2b1a20;--txt:#f0e0e6;--acc:#c2607a;--brd:#4a2a35;"); break;
      case 7: css += F("--bg:#f5f7fa;--card:#ffffff;--txt:#1e293b;--acc:#2563eb;--brd:#e2e8f0;"); break;
      case 8: css += F("--bg:#f6ecd9;--card:#fffaf0;--txt:#3f342a;--acc:#b5651d;--brd:#e3d5bd;"); break;
      default: css += F("--bg:#0f172a;--card:#1e293b;--txt:#e2e8f0;--acc:#38bdf8;--brd:#334155;"); break;
  }
  css += F("}body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
    "background:var(--bg);color:var(--txt);padding:16px;max-width:600px;margin:auto;line-height:1.5}"
    "nav{display:flex;gap:10px;margin-bottom:16px;background:var(--card);padding:6px;border-radius:10px;border:1px solid var(--brd)}"
    "nav a{flex:1;text-align:center;padding:10px;color:var(--txt);text-decoration:none;font-weight:600;border-radius:6px;transition:background .2s}"
    "nav a.active{background:var(--acc);color:var(--bg)}"
    "h2{color:var(--acc);font-size:20px;margin-bottom:12px;font-weight:700}"
    "h3{color:var(--txt);opacity:.8;margin:16px 0 8px;font-size:13px;text-transform:uppercase;letter-spacing:.05em}"
    ".card{background:var(--card);border-radius:12px;padding:16px;margin-bottom:16px;border:1px solid var(--brd)}"
    "input,textarea,select{width:100%;margin:6px 0 12px;padding:12px;border-radius:8px;"
    "border:1px solid var(--brd);background:var(--bg);color:var(--txt);font-size:15px;outline:none}"
    "input:focus,textarea:focus,select:focus{border-color:var(--acc)}"
    "textarea{min-height:140px;resize:vertical;font-family:inherit}"
    "button{width:100%;padding:12px;border:none;border-radius:8px;background:var(--acc);color:var(--bg);"
    "font-size:15px;font-weight:700;cursor:pointer}"
    "button:active{opacity:.8}"
    "button.del{background:#f43f5e;color:#fff}"
    "button.edit{background:var(--brd);color:var(--txt)}"
    ".row{display:flex;gap:10px;align-items:center}"
    ".row form{flex:1}"
    ".item{background:var(--card);border-radius:10px;padding:14px;margin:10px 0;border:1px solid var(--brd)}"
    ".item-hdr{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}"
    ".item .title{font-weight:600;font-size:16px;word-break:break-word}"
    ".badge{background:var(--brd);color:var(--acc);padding:3px 8px;border-radius:6px;font-size:12px;font-weight:600}"
    ".bar{background:var(--brd);border-radius:8px;overflow:hidden;height:10px;margin:8px 0 16px}"
    ".fill{background:var(--acc);height:100%}"
    ".hint{opacity:.6;font-size:13px}"
    ".msg-ok{color:#4ade80;background:#14532d;padding:10px;border-radius:8px;margin-bottom:12px}"
    ".msg-err{color:#f87171;background:#7f1d1d;padding:10px;border-radius:8px;margin-bottom:12px}"
    "a{color:var(--acc);text-decoration:none}"
    ".toggle-label{display:flex;align-items:center;justify-content:space-between;cursor:pointer;font-size:15px}"
    ".toggle-label input{width:auto;margin:0}");
  return css;
}

String navBar(const String &active) {
  return "<nav><a href='/' class='" + String(active == "main" ? "active" : "") + "'>Шпаргалки</a>"
         "<a href='/settings' class='" + String(active == "settings" ? "active" : "") + "'>Настройки</a></nav>";
}

void handleRoot() {
  String reqDir = server.hasArg("dir") ? server.arg("dir") : "/";
  if (!reqDir.endsWith("/")) reqDir += "/";

  size_t total = LittleFS.totalBytes(); size_t used = LittleFS.usedBytes();
  int pct = total ? (int)(used * 100 / total) : 0;

  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>ESP SuperminiReader</title><style>");
  html += pageStyle() + F("</style></head><body>") + navBar("main");
  html += "<div><span class='hint'>Память:</span> " + String(used) + " / " + String(total) + " Б (" + String(pct) + "%)</div>";
  html += "<div class='bar'><div class='fill' style='width:" + String(pct) + "%'></div></div>";

  if (server.hasArg("msg")) {
    String msg = server.arg("msg");
    if (msg == "nospace") html += F("<div class='msg-err'>Ошибка: Не хватает места!</div>");
    if (msg == "empty")   html += F("<div class='msg-err'>Ошибка: Введите заголовок!</div>");
    if (msg == "err_rmdir") html += F("<div class='msg-err'>Ошибка при удалении папки!</div>");
    if (msg == "ok")      html += F("<div class='msg-ok'>Успешно!</div>");
  }

  html += "<div class='card'><h3>Создать папку</h3>"
          "<form method='POST' action='/mkdir'>"
          "<input type='hidden' name='dir' value='" + reqDir + "'>"
          "<input type='text' name='dirname' placeholder='Имя папки' required>"
          "<button type='submit'>Создать</button></form></div>";

  html += "<div class='card'><h3>Добавить шпаргалку</h3>"
          "<form method='POST' action='/upload'>"
          "<input type='hidden' name='dir' value='" + reqDir + "'>"
          "<input type='text' name='title' placeholder='Заголовок шпаргалки' required>"
          "<textarea name='content' placeholder='Содержимое файла'></textarea>"
          "<button type='submit'>Загрузить файл</button></form></div>";

  html += "<h3>Содержимое папки: " + htmlEscape(reqDir) + "</h3>";
  
  if (reqDir != "/") {
    String parentDir = reqDir.substring(0, reqDir.lastIndexOf('/', reqDir.length() - 2) + 1);
    if (parentDir == "") parentDir = "/";
    html += "<div class='item'><a href='/?dir=" + parentDir + "'>&#128193; .. (Назад)</a></div>";
  }

  File root = LittleFS.open(reqDir);
  if (root) {
    File f = root.openNextFile();
    while (f) {
      String path = f.path();
      if (path.length() == 0) {
        path = reqDir + String(f.name());
        path.replace("//", "/");
      }
      if (path.indexOf("_cfg.txt") == -1) {
        if (f.isDirectory()) {
          String shortName = path.substring(path.lastIndexOf('/') + 1);
          html += "<div class='item'><div class='item-hdr'><a href='/?dir=" + path + "'>&#128193; " + htmlEscape(shortName) + "</a>"
                  "<form method='POST' action='/rmdir' onsubmit=\"return confirm('Удалить папку и всё её содержимое?')\" style='margin:0;'>"
                  "<input type='hidden' name='dir' value='" + path + "'>"
                  "<button class='del' type='submit' style='padding:4px 8px;font-size:12px;width:auto'>Удалить</button></form></div></div>";
        } else {
          String title = f.readStringUntil('\n'); title.trim();
          if (title.length() == 0) title = "Без названия";
          String szStr = f.size() < 1024 ? String(f.size()) + " Б" : String(f.size() / 1024.0, 1) + " КБ";
          html += "<div class='item'><div class='item-hdr'><div class='title'>" + htmlEscape(title) + "</div>"
                  "<span class='badge'>" + szStr + "</span></div>"
                  "<div class='row'>"
                  "<form method='GET' action='/edit'>"
                  "<input type='hidden' name='f' value='" + path + "'>"
                  "<input type='hidden' name='dir' value='" + reqDir + "'>"
                  "<button class='edit' type='submit'>Изменить</button></form>"
                  "<form method='POST' action='/delete' onsubmit=\"return confirm('Удалить шпаргалку?')\">"
                  "<input type='hidden' name='f' value='" + path + "'>"
                  "<input type='hidden' name='dir' value='" + reqDir + "'>"
                  "<button class='del' type='submit'>Удалить</button></form>"
                  "</div></div>";
        }
      }
      f = root.openNextFile();
    }
  }
  html += F("</body></html>");
  server.send(200, "text/html; charset=utf-8", html);
  refreshFileList();
}

void handleSettingsPage() {
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>Настройки</title><style>");
  html += pageStyle() + F("</style></head><body>") + navBar("settings") + F("<h2>Настройки устройства</h2>");

  if (server.hasArg("msg")) {
    String msg = server.arg("msg");
    if (msg == "wifierr") html += F("<div class='msg-err'>Ошибка: Имя сети от 1 символа, а пароль 0 или от 8 символов!</div>");
    if (msg == "ok")      html += F("<div class='msg-ok'>Настройки сохранены!</div>");
  }

  html += F("<div class='card'><h3>Внешний вид</h3><form method='POST' action='/save_theme'><select name='theme'>"
    "<option value='0'"); if (webTheme == 0) html += " selected"; html += F(">1. Синяя</option>"
    "<option value='1'"); if (webTheme == 1) html += " selected"; html += F(">2. Черно/Бежевая</option>"
    "<option value='2'"); if (webTheme == 2) html += " selected"; html += F(">3. Зеленая</option>"
    "<option value='3'"); if (webTheme == 3) html += " selected"; html += F(">4. Фиолетовая</option>"
    "<option value='4'"); if (webTheme == 4) html += " selected"; html += F(">5. Голубая</option>"
    "<option value='5'"); if (webTheme == 5) html += " selected"; html += F(">6. Бирюзовая</option>"
    "<option value='6'"); if (webTheme == 6) html += " selected"; html += F(">7. Бордовая</option>"
    "<option value='7'"); if (webTheme == 7) html += " selected"; html += F(">8. Светлая</option>"
    "<option value='8'"); if (webTheme == 8) html += " selected"; html += F(">9. Сепия/Бумага</option>"
    "</select><button type='submit'>Применить тему</button></form></div>");

  html += F("<div class='card'><h3>Параметры Wi-Fi</h3><form method='POST' action='/save_wifi'>"
    "<label class='hint'>Имя сети (SSID):</label><input type='text' name='ssid' value='");
  html += htmlEscape(AP_SSID);
  html += F("' required><label class='hint'>Пароль (минимум 8 символов или пустой):</label><input type='text' name='pass' value='");
  html += htmlEscape(AP_PASS);
  html += F("'><button type='submit'>Сохранить Wi-Fi</button></form></div>");

  html += F("<div class='card'><h3>Режим левши</h3><form method='POST' action='/save_lefty'>"
    "<label class='toggle-label'><span>Поворот экрана на 180°</span><input type='checkbox' name='lefty' value='1'");
  if (leftHanded) html += F(" checked");
  html += F("></label><br><button type='submit'>Применить</button></form></div>");

  html += F("<div class='card'><h3>Спящий режим</h3><form method='POST' action='/save_sleep'>"
    "<label class='toggle-label'><span>Включить автозасыпание</span><input type='checkbox' name='sleep_en' value='1'");
  if (sleepEnabled) html += F(" checked");
  html += F("></label><br><div class='row'>"
    "<div><label class='hint'>Минуты</label><input type='number' name='min' min='0' max='60' value='");
  html += String(sleepMin);
  html += F("'></div><div><label class='hint'>Секунды</label><input type='number' name='sec' min='0' max='59' value='");
  html += String(sleepSec);
  html += F("'></div></div><button type='submit'>Сохранить</button></form></div></body></html>");
  server.send(200, "text/html; charset=utf-8", html);
}

void handleMkdir() {
  String d = server.arg("dir");
  String n = server.arg("dirname"); n.trim();
  if (n.length() > 0) {
    String target = d + (d.endsWith("/") ? "" : "/") + n;
    LittleFS.mkdir(target);
  }
  redirectMsg("/?dir=" + d, "ok");
}

void handleRmdir() {
  String d = server.arg("dir");
  String parentDir = d.substring(0, d.lastIndexOf('/', d.length() - 2) + 1);
  if (parentDir == "") parentDir = "/";

  if (removeDirRecursive(d)) {
    redirectMsg("/?dir=" + parentDir, "ok");
  } else {
    redirectMsg("/?dir=" + parentDir, "err_rmdir");
  }
}

void handleUpload() {
  String dir = server.arg("dir");
  String title = server.arg("title"); String content = server.arg("content");
  title.trim();
  if (title.length() == 0) { redirectMsg("/?dir=" + dir, "empty"); return; }
  if (!hasSpace(title.length() + content.length() + 8)) { redirectMsg("/?dir=" + dir, "nospace"); return; }
  String fn = genFilename(dir);
  writeFile(fn, title, content);
  redirectMsg("/?dir=" + dir, "ok");
}

void handleEditPage() {
  if (!server.hasArg("f")) { handleRedirect(); return; }
  String fn = server.arg("f");
  String reqDir = server.hasArg("dir") ? server.arg("dir") : "/";
  if (!LittleFS.exists(pathOf(fn))) { handleRedirect(); return; }

  FileData d = readFile(fn);
  String html = F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>Редактирование</title><style>");
  html += pageStyle() + F("</style></head><body>") + navBar("main");
  html += F("<h2>Изменить шпаргалку</h2>");
  html += "<form method='POST' action='/save'>"
          "<input type='hidden' name='f' value='" + fn + "'>"
          "<input type='hidden' name='dir' value='" + reqDir + "'>"
          "<input type='text' name='title' value='" + htmlEscape(d.title) + "' required>"
          "<textarea name='content'>" + htmlEscape(d.content) + "</textarea>"
          "<button type='submit'>Сохранить</button></form>"
          "<form method='POST' action='/delete' style='margin-top:10px;' onsubmit=\"return confirm('Удалить шпаргалку?')\">"
          "<input type='hidden' name='f' value='" + fn + "'>"
          "<input type='hidden' name='dir' value='" + reqDir + "'>"
          "<button class='del' type='submit'>Удалить</button></form>"
          "<p style='margin-top:16px;'><a href='/?dir=" + reqDir + "'>&larr; Назад</a></p></body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

void handleSave() {
  String fn = server.arg("f"); String title = server.arg("title"); String content = server.arg("content");
  String dir = server.arg("dir"); title.trim();
  if (title.length() == 0 || !LittleFS.exists(pathOf(fn))) { redirectMsg("/?dir=" + dir, "empty"); return; }
  
  size_t oldSize = 0; File f = LittleFS.open(pathOf(fn), "r");
  if (f) { oldSize = f.size(); f.close(); }
  long delta = (title.length() + content.length() + 2) - (long)oldSize;
  if (delta > 0 && !hasSpace(delta)) { redirectMsg("/?dir=" + dir, "nospace"); return; }

  writeFile(fn, title, content);
  redirectMsg("/?dir=" + dir, "ok");
}

void handleDelete() {
  if (server.hasArg("f")) LittleFS.remove(pathOf(server.arg("f")));
  redirectMsg("/?dir=" + server.arg("dir"), "ok");
}

void handleSaveWifi() {
  String newSsid = server.arg("ssid"); String newPass = server.arg("pass"); newSsid.trim();
  if (newSsid.length() < 1 || (newPass.length() > 0 && newPass.length() < 8)) { redirectMsg("/settings?", "wifierr"); return; }
  AP_SSID = newSsid; AP_PASS = newPass; markSettingsDirty(); redirectMsg("/settings?", "ok");
}

void handleSaveLefty() {
  leftHanded = server.hasArg("lefty"); applyOrientation(); markSettingsDirty(); drawScreen();
  redirectMsg("/settings?", "ok");
}

void handleSaveSleep() {
  sleepEnabled = server.hasArg("sleep_en");
  int minVal = server.arg("min").toInt();
  sleepMin = max(0, minVal);
  int secVal = server.arg("sec").toInt();
  sleepSec = constrain(secVal, 0, 59);  
  sleepTimeoutMs = ((unsigned long)sleepMin * 60 + sleepSec) * 1000UL;
  lastActivity = millis();
  markSettingsDirty();
  redirectMsg("/settings?", "ok");
}

void handleSaveTheme() {
  if (server.hasArg("theme")) { webTheme = constrain(server.arg("theme").toInt(), 0, 8); markSettingsDirty(); }
  redirectMsg("/settings?", "ok");
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/settings", HTTP_GET, handleSettingsPage);
  server.on("/upload", HTTP_POST, handleUpload);
  server.on("/mkdir", HTTP_POST, handleMkdir);
  server.on("/rmdir", HTTP_POST, handleRmdir);
  server.on("/edit", HTTP_GET, handleEditPage);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/save_wifi", HTTP_POST, handleSaveWifi);
  server.on("/save_lefty", HTTP_POST, handleSaveLefty);
  server.on("/save_sleep", HTTP_POST, handleSaveSleep);
  server.on("/save_theme", HTTP_POST, handleSaveTheme);

  server.on("/generate_204", HTTP_GET, handleRedirect);
  server.on("/gen_204", HTTP_GET, handleRedirect);
  server.on("/hotspot-detect.html", HTTP_GET, handleRedirect);
  server.onNotFound(handleRedirect);
}
