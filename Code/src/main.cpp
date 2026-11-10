#include <Arduino.h>
#include <Wire.h>
#include <GyverOLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <DNSServer.h>
#include <esp_system.h>
#if __has_include(<esp_random.h>)
#include <esp_random.h>
#endif
#include <LittleFS.h>
#include <algorithm>
#include <esp_sleep.h>
#include <math.h>

namespace cfg {
constexpr uint8_t PIN_SDA = 11, PIN_SCL = 13, PIN_UP = 5, PIN_DOWN = 7, PIN_OK = 6, PIN_RGB = 48;  // PIN_RGB - встроенный WS2812 на ESP32-S3 SuperMini
constexpr int SCREEN_ROWS = 8;         // строк текста на экране
constexpr int CHARS_PER_LINE = 21;     // символов в строке
constexpr int PATH_LEN = 128;          // макс. длина пути в байтах (кириллица = 2 байта на букву)
constexpr int TITLE_LEN = 96;          // макс. длина заголовка в списке
constexpr int MAX_FILES = 256;         // файлов в одной папке (лишние не показываются)
constexpr int MAX_PAGES = 4096;        // страниц в одном файле (по 8 строк)
constexpr int MAX_MARKS = 64;          // макс. выделенных для удаления
constexpr size_t IMG_BYTES = 1024;     // 128x64 бит
constexpr uint32_t SCROLL_MS = 300;    // скорость бегущей строки
constexpr uint32_t SAVE_DELAY_MS = 800;
constexpr const char *SETTINGS_PATH = "/_cfg.txt";
constexpr const char *AI_PATH = "/_ai.txt";  // временный файл с ответом ИИ
constexpr int AI_Q_BYTES = 240;              // макс. длина вопроса к ИИ в байтах
}  

using Oled = GyverOLED<SSD1306_128x64, OLED_BUFFER>;

namespace str {
inline void trim(char *s) {
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '\t')) s[--n] = 0;
  size_t i = 0;
  while (s[i] == ' ' || s[i] == '\t') i++;
  if (i) memmove(s, s + i, n - i + 1);
}
inline bool safeChars(const char *s) {
  for (; *s; s++) {
    uint8_t c = (uint8_t)*s;
    if (c < 0x20 || strchr("'\"<>&\\%?#", c)) return false;
  }
  return true;
}
inline bool safePath(const char *p) {
  return p && p[0] == '/' && strlen(p) < (size_t)cfg::PATH_LEN && !strstr(p, "..") && safeChars(p);
}
inline bool endsWith(const char *s, const char *suffix) {
  size_t a = strlen(s), b = strlen(suffix);
  return a >= b && !strcmp(s + a - b, suffix);
}
inline const char *baseName(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}
inline void urlEncode(char *out, size_t cap, const char *in) {
  static const char DIGITS[] = "0123456789ABCDEF";
  size_t n = 0;
  for (; *in && n + 4 < cap; in++) {
    uint8_t c = (uint8_t)*in;
    if (isalnum(c) || strchr("/-_.", c)) out[n++] = (char)c;
    else { out[n++] = '%'; out[n++] = DIGITS[c >> 4]; out[n++] = DIGITS[c & 15]; }
  }
  out[n] = 0;
}
} 

namespace u8 {
inline bool isCont(uint8_t c) { return (c & 0xC0) == 0x80; }
inline int length(const char *s) {
  int n = 0;
  for (; *s; s++) if (!isCont((uint8_t)*s)) n++;
  return n;
}
inline const char *next(const char *p) {
  if (*p) { p++; while (isCont((uint8_t)*p)) p++; }
  return p;
}

inline void copy(char *dst, size_t cap, const char *src, int start, int count) {
  const char *p = src;
  for (int i = 0; i < start && *p; i++) p = next(p);
  size_t n = 0;
  for (int i = 0; i < count && *p; i++) {
    const char *e = next(p);
    size_t l = (size_t)(e - p);
    if (n + l + 1 > cap) break;
    memcpy(dst + n, p, l);
    n += l;
    p = e;
  }
  dst[n] = 0;
}

inline void trimPartial(char *s) {
  size_t n = strlen(s), i = n;
  while (i > 0 && isCont((uint8_t)s[i - 1])) i--;
  if (i == 0) { s[0] = 0; return; }
  uint8_t lead = (uint8_t)s[i - 1];
  size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  if (n - (i - 1) < need) s[i - 1] = 0;
}
}  

namespace fsx {
inline bool hasSpace(long extra) {
  return (long)LittleFS.totalBytes() - (long)LittleFS.usedBytes() - extra > 512;
}
// вызывает fn(путь, файл) для каждого элемента папки (служебный файл настроек пропускается)
template <class Fn> void scanDir(const char *dir, Fn fn) {
  File root = LittleFS.open(dir);
  if (!root) return;
  char tmp[2 * cfg::PATH_LEN];
  File f = root.openNextFile();
  while (f) {
    const char *p = f.path();
    if (!p || !*p) { snprintf(tmp, sizeof tmp, "%s%s", dir, f.name()); p = tmp; }
    if (!strstr(p, cfg::SETTINGS_PATH + 1) && !strstr(p, "_up.tmp") && !strstr(p, "_ai.txt")) fn(p, f);
    f = root.openNextFile();
  }
}

inline size_t readLine(File &f, char *buf, size_t cap) {
  size_t n = f.readBytesUntil('\n', buf, cap - 1);
  buf[n] = 0;
  if (n == cap - 1) {
    int c, guard = 256;
    while (guard-- > 0 && f.available() && (c = f.read()) != '\n' && c != -1) {}
  }
  return n;
}
inline bool removeDirRecursive(const char *path, int depth = 0) {
  if (depth > 8) return false;  
  char child[cfg::PATH_LEN * 2];
  bool ok = true;
  for (;;) {
    bool isDir = false, found = false;
    {
      File d = LittleFS.open(path);
      if (!d || !d.isDirectory()) return false;
      File c = d.openNextFile();
      if (c) {
        const char *p = c.path();
        if (!p || !*p) snprintf(child, sizeof child, "%s/%s", path, c.name());
        else strlcpy(child, p, sizeof child);
        isDir = c.isDirectory();
        found = true;
      }
    }
    if (!found) break;
    bool r = isDir ? removeDirRecursive(child, depth + 1) : LittleFS.remove(child);
    if (!r) { ok = false; break; }  // не удалось - выходим, чтобы не зациклиться
  }
  return LittleFS.rmdir(path) && ok;
}
}  

class Button {
 public:
  void begin(uint8_t p) {
    pin = p;
    pinMode(pin, INPUT_PULLUP);
    stable = lastRaw = digitalRead(pin) == LOW;
  }
  void update(uint32_t now) {  // антидребезг по времени, а не по числу циклов
    pressed = released = longPress = repeat = false;
    bool raw = digitalRead(pin) == LOW;
    if (raw != lastRaw) { lastRaw = raw; rawAt = now; }
    if (raw != stable && now - rawAt >= DEBOUNCE_MS) {
      stable = raw;
      if (stable) { tPress = tRepeat = now; longFired = false; pressed = true; }
      else released = true;
    }
    if (stable) {
      uint32_t held = now - tPress;
      if (!longFired && held >= LONGPRESS_MS) { longFired = true; longPress = true; }
      if (held >= REPEAT_START_MS && now - tRepeat >= REPEAT_MS) { tRepeat = now; repeat = true; }
    }
  }
  bool stable = false, pressed = false, released = false, longPress = false, repeat = false, longFired = false;

 private:
  static const uint32_t DEBOUNCE_MS = 12, LONGPRESS_MS = 600, REPEAT_START_MS = 500, REPEAT_MS = 180;
  uint8_t pin = 0;
  bool lastRaw = false;
  uint32_t rawAt = 0, tPress = 0, tRepeat = 0;
};

class Settings {
 public:
  uint8_t brightness = 200;
  bool leftHanded = false;
  bool sleepEnabled = false;
  int sleepMin = 0, sleepSec = 0;
  int webTheme = 0;
  int best = 0;  // рекорд Flappy Bird
  int tetBest = 0;  // рекорд Tetris
  int marBest = 0;  // рекорд Mario
  bool rgbBoot = false;  // подсветка включается при старте (код 1010)
  int promptCs = 100;  // суфлёр: секунд на строку, в сотых (100 = 1.00 с)
  char ssid[33];
  char pass[65];
  char lastFile[cfg::PATH_LEN];
  char staSsid[33] = "", staPass[65] = "", apiKey[128] = "", model[40] = "openai/gpt-4o-mini";  // ИИ: сеть для запросов и ключ ProxyAPI
  bool aiReady() const { return staSsid[0] && apiKey[0]; }

  Settings() {
    strcpy(ssid, "Esp hpora");
    strcpy(pass, "12345678");
    lastFile[0] = 0;
  }
  uint32_t sleepTimeoutMs() const { return ((uint32_t)sleepMin * 60 + (uint32_t)sleepSec) * 1000UL; }

  void touch(uint32_t now) { dirty = true; changedAt = now; }       // отложенное сохранение
  void tick(uint32_t now) {
    if (dirty && now - changedAt > cfg::SAVE_DELAY_MS) { save(); dirty = false; }
  }

  void load() {
    if (!LittleFS.exists(cfg::SETTINGS_PATH)) return;
    File f = LittleFS.open(cfg::SETTINGS_PATH, "r");
    if (!f) return;
    char l[18][cfg::PATH_LEN];
    int got = 0;
    while (got < 18 && f.available()) {
      fsx::readLine(f, l[got], cfg::PATH_LEN);
      str::trim(l[got]);
      got++;
    }
    f.close();
    if (got > 0 && l[0][0]) { int v = atoi(l[0]); if (v >= 5 && v <= 255) brightness = (uint8_t)v; }
    if (got > 1) strlcpy(lastFile, l[1], sizeof lastFile);
    if (got > 2 && l[2][0]) leftHanded = atoi(l[2]) == 1;
    if (got > 3 && l[3][0]) strlcpy(ssid, l[3], sizeof ssid);
    if (got > 4) strlcpy(pass, l[4], sizeof pass);
    if (got > 5 && l[5][0]) sleepMin = constrain(atoi(l[5]), 0, 60);
    if (got > 6 && l[6][0]) sleepSec = constrain(atoi(l[6]), 0, 59);
    if (got > 7 && l[7][0]) sleepEnabled = atoi(l[7]) == 1;
    if (got > 8 && l[8][0]) webTheme = constrain(atoi(l[8]), 0, 8);
    if (got > 9 && l[9][0]) best = constrain(atoi(l[9]), 0, 99999);
    if (got > 10 && l[10][0]) promptCs = constrain(atoi(l[10]), 5, 6000);
    if (got > 11 && l[11][0]) tetBest = constrain(atoi(l[11]), 0, 999999);
    if (got > 12 && l[12][0]) marBest = constrain(atoi(l[12]), 0, 999999);
    if (got > 13 && l[13][0]) rgbBoot = atoi(l[13]) == 1;
    if (got > 14) strlcpy(staSsid, l[14], sizeof staSsid);
    if (got > 15) strlcpy(staPass, l[15], sizeof staPass);
    if (got > 16) strlcpy(apiKey, l[16], sizeof apiKey);
    if (got > 17 && l[17][0]) strlcpy(model, l[17], sizeof model);
  }
  void save() const {
    File f = LittleFS.open(cfg::SETTINGS_PATH, "w");
    if (!f) return;
    f.println((int)brightness);
    f.println(lastFile);
    f.println(leftHanded ? 1 : 0);
    f.println(ssid);
    f.println(pass);
    f.println(sleepMin);
    f.println(sleepSec);
    f.println(sleepEnabled ? 1 : 0);
    f.println(webTheme);
    f.println(best);
    f.println(promptCs);
    f.println(tetBest);
    f.println(marBest);
    f.println(rgbBoot ? 1 : 0);
    f.println(staSsid);
    f.println(staPass);
    f.println(apiKey);
    f.println(model);
    f.close();
  }

 private:
  bool dirty = false;
  uint32_t changedAt = 0;
};

class Calculator {
 public:
  Calculator() { reset(); }
  void reset() { disp[0] = 0; result[0] = 0; sel = 0; }
  void moveSel(int dir) {
    int row = sel / COLS, col = sel % COLS;
    int v = col * ROWS + row + dir;
    if (v < 0) v = ROWS * COLS - 1;
    if (v >= ROWS * COLS) v = 0;
    sel = (v % ROWS) * COLS + v / ROWS;
  }
  const char *text() const { return disp; }
  const char *selectedKey() const { return KEYS[sel / COLS][sel % COLS]; }
  void pressSelected() { press(KEYS[sel / COLS][sel % COLS]); }

  void press(const char *key) {
    if (!strcmp(key, "DEL")) {
      size_t n = strlen(disp);
      if (n >= 4 && !strcmp(disp + n - 4, "sqrt")) disp[n - 4] = 0;
      else if (n) disp[n - 1] = 0;
      result[0] = 0;
    } else if (!strcmp(key, "C")) {
      disp[0] = 0; result[0] = 0;
    } else if (!strcmp(key, "=")) {
      if (!disp[0]) return;
      Parser p(disp);
      double r = p.expr();
      p.skip();
      if (p.error || *p.s) strlcpy(result, "Ошибка", sizeof result);
      else format(r, result, sizeof result);
    } else {
      if (strlen(disp) < 20) strlcat(disp, key, sizeof disp);
      result[0] = 0;
    }
  }

  // вычисление строки без UI (удобно для проверки)
  static bool eval(const char *text, double &out) {
    Parser p(text);
    out = p.expr();
    p.skip();
    return !p.error && !*p.s;
  }

  void draw(Oled &oled) const {
    // Только оформление. Масштаб 1, весь текст на границах страниц (8 px) - так GyverOLED рисует чисто.
    // Дисплей: рамка, ввод или результат слева направо
    char line[32];
    if (result[0]) snprintf(line, sizeof line, "=%s", result);
    else strlcpy(line, disp, sizeof line);
    int len = u8::length(line);
    if (len > 19) { char t[32]; u8::copy(t, sizeof t, line, len - 19, 19); strlcpy(line, t, sizeof line); len = 19; }
    // Скруглённая рамка из сплошных линий (углы - короткие диагонали, без отдельных точек)
    oled.line(2, 1, 125, 1);   oled.line(2, 21, 125, 21);
    oled.line(0, 3, 0, 19);    oled.line(127, 3, 127, 19);
    oled.line(0, 3, 2, 1);     oled.line(125, 1, 127, 3);
    oled.line(0, 19, 2, 21);   oled.line(125, 21, 127, 19);
    oled.setCursor(5, 1);
    oled.print(line);
    if (!result[0]) oled.line(5 + len * 6, 8, 5 + len * 6, 14);  // курсор ввода

    const int BTN_W = 32, CHAR_W = 6, CHAR_H = 8, BTN_CHARS = 5, FIRST_ROW = 3;

    for (int row = 0; row < ROWS; row++) {
      for (int col = 0; col < COLS; col++) {
        const char *label = KEYS[row][col];
        int len2 = (int)strlen(label);
        if (len2 > BTN_CHARS) len2 = BTN_CHARS;
        // Печатаем только саму надпись, без пробелов-отступов: инвертированные пробелы
        // закрашивали углы плашки и скругление не было видно.
        int colX = col * BTN_W, textX = colX + (BTN_W - len2 * CHAR_W) / 2;
        int lineY = FIRST_ROW + row, btnY = lineY * CHAR_H;
        bool isSel = (row * COLS + col) == sel;
        if (isSel) {
          // плашка: по одному срезанному пикселю в каждом углу (сплошные прямоугольники, без отдельных точек)
          oled.rect(colX + 2, btnY - 1, colX + BTN_W - 3, btnY + 7, OLED_FILL);
          oled.rect(colX + 1, btnY,     colX + BTN_W - 2, btnY + 6, OLED_FILL);
          oled.invertText(true);
        }
        oled.setCursor(textX, lineY);
        oled.print(label);
        if (isSel) oled.invertText(false);
      }
    }
  }

 private:
  enum { COLS = 4, ROWS = 5 };
  static const char *const KEYS[ROWS][COLS];
  char disp[24];
  char result[24];
  int sel;
  struct Parser {
    const char *s;
    bool error;
    explicit Parser(const char *str) : s(str), error(false) {}
    void skip() { while (*s == ' ') s++; }
    double expr() {
      double v = term();
      skip();
      while (*s == '+' || *s == '-') {
        char op = *s++;
        double r = term();
        v = (op == '+') ? v + r : v - r;
        skip();
      }
      return v;
    }
    double term() {
      double v = unary();
      skip();
      while (*s == '*' || *s == '/') {
        char op = *s++;
        double r = unary();
        if (op == '*') v *= r;
        else if (r == 0) { error = true; v = 0; }
        else v /= r;
        skip();
      }
      return v;
    }
    double unary() {
      skip();
      if (*s == '-') { s++; return -unary(); }
      if (*s == '+') { s++; return unary(); }
      return power();
    }
    double power() {
      double b = primary();
      skip();
      if (*s == '^') { s++; return pow(b, unary()); }
      return b;
    }
    double primary() {
      skip();
      if (!strncmp(s, "sqrt", 4)) { s += 4; return sqrt(unary()); }
      char *end;
      double v = strtod(s, &end);
      if (end == s) { error = true; return 0; }
      s = end;
      return v;
    }
  };

  static void format(double v, char *out, size_t cap) {
    if (isnan(v) || isinf(v)) { strlcpy(out, "Ошибка", cap); return; }
    const int MAX_LEN = cfg::CHARS_PER_LINE - 1;
    char buf[48];
    snprintf(buf, sizeof buf, "%.14g", v);
    char *dot = strchr(buf, '.');
    if ((int)strlen(buf) > MAX_LEN && dot && !strchr(buf, 'e')) {
      int keep = MAX_LEN - (int)(dot - buf) - 1;
      if (keep < 0) keep = 0;
      snprintf(buf, sizeof buf, "%.*f", keep, v);
      if (strchr(buf, '.')) {
        size_t n = strlen(buf);
        while (n && buf[n - 1] == '0') buf[--n] = 0;
        if (n && buf[n - 1] == '.') buf[--n] = 0;
      }
    }
    strlcpy(out, buf, cap);
  }
};
const char *const Calculator::KEYS[Calculator::ROWS][Calculator::COLS] = {
  {"C", "^", "sqrt", "DEL"},
  {"7", "8", "9", "/"},
  {"4", "5", "6", "*"},
  {"1", "2", "3", "-"},
  {"0", ".", "=", "+"}
};

class LineReader {
 public:
  void begin(File &file, uint32_t offset) {
    f = &file;
    f->seek(offset);
    pos = offset;
    bl = bi = 0;
    have = false;
  }
  uint32_t position() const { return pos; }
  void skipLine() { int c; while ((c = readByte()) != -1 && c != '\n') {} }
  bool nextLine(char *line, size_t cap, uint32_t *startOff) {
    line[0] = 0;
    int lc = 0;
    size_t lb = 0;
    bool got = false, started = false;
    for (;;) {
      if (!have) {
        if (!readWord()) return got;
        have = true;
      }
      if (!started) { if (startOff) *startOff = wstart; started = true; }
      if (wchars == 0) { have = false; if (wbreak) return true; continue; }  // пустая строка в тексте / слово целиком выброшено
      int extra = (lc > 0 ? 1 : 0) + wchars;
      if (lc + extra > cfg::CHARS_PER_LINE) return true;  // слово переносится на следующую строку
      if (lc > 0 && lb + 1 < cap) line[lb++] = ' ';
      if (lb + (size_t)wbytes < cap) { memcpy(line + lb, word, (size_t)wbytes); lb += (size_t)wbytes; }
      line[lb] = 0;
      lc += extra;
      got = true;
      have = false;
      if (wbreak) return true;
    }
  }

 private:
  File *f = nullptr;
  uint32_t pos = 0, wstart = 0;
  uint8_t buf[64];
  int bl = 0, bi = 0;
  char word[cfg::CHARS_PER_LINE * 4 + 1];
  int wbytes = 0, wchars = 0;
  bool wbreak = false, have = false;

  int readByte() {
    if (bi >= bl) {
      int n = (int)f->read(buf, sizeof buf);
      if (n <= 0) { bl = bi = 0; return -1; }
      bl = n;
      bi = 0;
    }
    pos++;
    return buf[bi++];
  }
  void unread() { bi--; pos--; }

  bool readWord() {
    wbytes = wchars = 0;
    wbreak = false;
    int c;
    while ((c = readByte()) == ' ' || c == '\r') {}
    if (c == -1) return false;
    wstart = pos - 1;
    if (c == '\n') { wbreak = true; word[0] = 0; return true; }
    for (;;) {
      if (c == ' ' || c == '\r') break;
      if (c == '\n') { wbreak = true; break; }
      bool lead = !u8::isCont((uint8_t)c);
      if (lead && wchars >= cfg::CHARS_PER_LINE) { unread(); break; }  
      if (wbytes < (int)sizeof word - 1) word[wbytes++] = (char)c;
      if (lead) wchars++;
      c = readByte();
      if (c == -1) break;
    }
    word[wbytes] = 0;
    normalize();
    return true;
  }

  // Убирает из слова всё, чего нет в шрифте OLED (даёт '?'): битый UTF-8, управляющие,
  // невидимые и типографские символы заменяются на ASCII-аналоги.
  void normalize() {
    char out[sizeof word];
    int o = 0;
    for (int i = 0; i < wbytes;) {
      uint8_t c = (uint8_t)word[i];
      if (c < 0x20 || c == 0x7F) { i++; continue; }
      if (c < 0x80) { out[o++] = (char)c; i++; continue; }
      int need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
      bool ok = need && i + need <= wbytes;
      for (int k = 1; ok && k < need; k++) ok = u8::isCont((uint8_t)word[i + k]);
      if (!ok) { i++; continue; }  // оборванный/битый символ — выкидываем
      uint8_t b1 = (uint8_t)word[i + 1], b2 = need > 2 ? (uint8_t)word[i + 2] : 0;
      const char *rep = nullptr;
      bool known = false;
      if (c == 0xC2 && b1 == 0xA0) { rep = " "; known = true; }
      else if (c == 0xC2 && (b1 == 0xAB || b1 == 0xBB)) { rep = "\""; known = true; }
      else if (c == 0xC2 && b1 == 0xAD) { rep = ""; known = true; }
      else if (c == 0xE2 && b1 == 0x80) {
        known = true;
        if (b2 >= 0x80 && b2 <= 0x8F) rep = "";            // zero-width и пр.
        else if (b2 >= 0x90 && b2 <= 0x95) rep = "-";
        else if (b2 == 0x98 || b2 == 0x99 || b2 == 0x9A) rep = "'";
        else if (b2 == 0x9C || b2 == 0x9D || b2 == 0x9E) rep = "\"";
        else if (b2 == 0xA6) rep = "...";
        else if (b2 == 0xA2) rep = "*";
        else known = false;
      }
      else if (c == 0xEF && b1 == 0xBB && b2 == 0xBF) { rep = ""; known = true; }  // BOM
      if (known) { for (const char *r = rep; *r && o < (int)sizeof out - 1; r++) out[o++] = *r; }
      else if (need == 2 && (c == 0xD0 || c == 0xD1) && o + 2 < (int)sizeof out) { out[o++] = (char)c; out[o++] = (char)b1; }
      else if (need >= 3) { /* прочие 3-4-байтные (эмодзи и т.п.) — не отображаются */ }
      else if (o + 2 < (int)sizeof out) { out[o++] = (char)c; out[o++] = (char)b1; }
      i += need;
    }
    memcpy(word, out, o);
    word[o] = 0;
    wbytes = o;
    wchars = u8::length(word);
  }
};

class Reader {
 public:
  bool bar = false;  // полоса прокрутки справа (для ответов ИИ)
  void openText(const char *path) {
    strlcpy(file, path, sizeof file);
    bar = false;
    isImage = false;
    page = 0;
    hasNext = false;
    pageOff[0] = 0;
    File f = LittleFS.open(file, "r");
    if (!f) return;
    LineReader lr;
    lr.begin(f, 0);
    lr.skipLine();  
    pageOff[0] = lr.position();
  }
  void openImage(const char *path) {
    strlcpy(file, path, sizeof file);
    bar = false;
    isImage = true;
    imgOk = false;
    File f = LittleFS.open(file, "r");
    if (!f) return;
    size_t n = f.read(img, cfg::IMG_BYTES);
    f.close();
    if (n < cfg::IMG_BYTES) memset(img + n, 0, cfg::IMG_BYTES - n);
    imgOk = true;
  }
  bool next() {
    if (!hasNext || page + 1 >= cfg::MAX_PAGES) return false;
    pageOff[++page] = nextOff;
    return true;
  }
  bool prev() {
    if (page <= 0) return false;
    page--;
    return true;
  }

  // ---- суфлёр: построчная прокрутка с первой страницы; страница и её смещения не меняются,
  // поэтому после выхода экран выглядит ровно как до запуска
  bool canPrompt() const { return !isImage && page == 0; }
  void promptStart() { prompting = true; promptMore = false; promptOff = pageOff[0]; }
  void promptStop() { prompting = false; }
  bool promptAdvance() {  // false = текст закончился
    if (!promptMore) return false;
    promptOff = promptNext;
    return true;
  }

  void draw(Oled &oled) {
    if (prompting && !isImage) { drawPrompt(oled); return; }
    if (isImage) {
      if (!imgOk) { msg(oled, "Ошибка чтения"); return; }
      oled.drawBitmap(0, 0, img, 128, 64);
      return;
    }
    File f = LittleFS.open(file, "r");
    if (!f) { msg(oled, "Ошибка чтения"); return; }
    LineReader lr;
    lr.begin(f, pageOff[page]);
    char line[cfg::CHARS_PER_LINE * 4 + 1];
    int rows = 0;
    for (; rows < cfg::SCREEN_ROWS; rows++) {
      if (!lr.nextLine(line, sizeof line, nullptr)) break;
      oled.setCursor(0, rows);
      oled.print(line);
    }
    uint32_t off = 0;
    hasNext = rows == cfg::SCREEN_ROWS && lr.nextLine(line, sizeof line, &off);
    if (hasNext) nextOff = off;
    if (page > 0 || hasNext) {  // полоса прокрутки справа во всех текстовых файлах
      uint32_t sz = f.size();
      int ty = !hasNext ? 54 : sz ? (int)((54UL * pageOff[page]) / sz) : 0;
      for (int y = 0; y < 64; y += 4) oled.dot(127, y, 1);
      oled.rect(126, ty, 127, ty + 9, OLED_FILL);
    }
    f.close();
    if (rows == 0 && page == 0) msg(oled, "Пусто");
  }

 private:
  char file[cfg::PATH_LEN] = "";
  bool isImage = false;
  int page = 0;
  bool hasNext = false;
  uint32_t nextOff = 0;
  bool prompting = false, promptMore = false;
  uint32_t promptOff = 0, promptNext = 0;
  void drawPrompt(Oled &oled) {
    promptMore = false;
    File f = LittleFS.open(file, "r");
    if (!f) { msg(oled, "Ошибка чтения"); return; }
    LineReader lr;
    lr.begin(f, promptOff);
    char line[cfg::CHARS_PER_LINE * 4 + 1];
    uint32_t off = 0;
    int rows = 0;
    for (; rows < cfg::SCREEN_ROWS; rows++) {
      if (!lr.nextLine(line, sizeof line, rows == 1 ? &off : nullptr)) break;
      oled.setCursor(0, rows);
      oled.print(line);
    }
    // Боковая полоса прокрутки суфлёра: положение бегунка зависит
    // от текущего смещения в файле, как и в обычном режиме чтения.
    uint32_t sz = f.size();
    int ty = sz ? (int)((54ULL * promptOff) / sz) : 0;
    if (ty > 54) ty = 54;
    for (int y = 0; y < 64; y += 4) oled.dot(127, y, 1);
    oled.rect(126, ty, 127, ty + 9, OLED_FILL);

    f.close();
    if (rows >= 2) { promptMore = true; promptNext = off; }  // есть вторая строка — можно сдвинуться
  }
  uint32_t pageOff[cfg::MAX_PAGES];  // начало каждой посещённой страницы
  bool imgOk = false;
  uint8_t img[cfg::IMG_BYTES];
  static void msg(Oled &oled, const char *t) { oled.setCursor(0, 3); oled.print(t); }
};

struct Entry {
  char path[cfg::PATH_LEN];
  char title[cfg::TITLE_LEN];
  bool isDir;
  bool isImage;
};

class FileBrowser {
 public:
  FileBrowser() { strcpy(dir, "/"); }

  int count() const { return n; }
  const Entry *current() const { return (n > 0 && sel < n) ? &items[sel] : nullptr; }

  void refresh() {
    n = 0;
    if (strcmp(dir, "/") != 0) add("..", ".. (Назад)", true, false);
    fsx::scanDir(dir, [this](const char *p, File &f) {
      const char *name = str::baseName(p);
      char t[cfg::TITLE_LEN];
      if (f.isDirectory()) {
        snprintf(t, sizeof t, "[%s]", name);
        add(p, t, true, false);
      }
      else if (str::endsWith(p, ".h")) {
        add(p, name, false, true);
      }
      else {
        fsx::readLine(f, t, sizeof t);
        str::trim(t);
        if (!t[0]) strlcpy(t, "Без названия", sizeof t);
        add(p, t, false, false);
      }
    });
    std::sort(items, items + n, [](const Entry &a, const Entry &b) {
      bool aUp = strcmp(a.path, "..") == 0, bUp = strcmp(b.path, "..") == 0;
      if (aUp || bUp) return aUp && !bUp;
      if (a.isDir != b.isDir) return a.isDir;
      return strcmp(a.title, b.title) < 0;
    });
    if (sel >= n) sel = n ? n - 1 : 0;
  }

  void move(int d) {
    if (n == 0) return;
    sel += d;
    if (sel < 0) sel = n - 1;
    if (sel >= n) sel = 0;
    scrollPos = 0;
  }
  // открыть папку последнего файла и встать на него
  void openAt(const char *p) {
    if (p[0] && LittleFS.exists(p)) {
      strlcpy(dir, p, sizeof dir);
      char *sl = strrchr(dir, '/');
      if (sl) sl[1] = 0; else strcpy(dir, "/");
    }
    refresh();
    selectByPath(p);
  }
  // если текущая папка удалена - подняться до существующей
  void fixDir() {
    char t[sizeof dir];
    while (strcmp(dir, "/")) {
      strlcpy(t, dir, sizeof t);
      t[strlen(t) - 1] = 0;
      if (LittleFS.exists(t)) break;
      char *sl = strrchr(t, '/');
      if (sl) sl[1] = 0; else strcpy(t, "/");
      strcpy(dir, t);
      sel = 0; top = 0;
    }
    refresh();
  }
  // выделение для удаления
  void beginMark() { markMode = true; nMarks = 0; }
  void endMark() { markMode = false; nMarks = 0; }
  int markCount() const { return nMarks; }
  const char *markAt(int i) const { return marks[i]; }
  bool isMarked(const char *p) const {
    for (int i = 0; i < nMarks; i++) if (!strcmp(marks[i], p)) return true;
    return false;
  }
  void toggleMark() {
    const Entry *e = current();
    if (!e || !strcmp(e->path, "..")) return;
    for (int i = 0; i < nMarks; i++)
      if (!strcmp(marks[i], e->path)) { strcpy(marks[i], marks[--nMarks]); return; }
    if (nMarks < cfg::MAX_MARKS) strlcpy(marks[nMarks++], e->path, sizeof marks[0]);
  }
  void selectByPath(const char *p) {
    if (!p[0]) return;
    for (int i = 0; i < n; i++) if (!strcmp(items[i].path, p)) { sel = i; break; }
  }

  bool enterSelected() {
    const Entry *e = current();
    if (!e || !e->isDir) return false;
    if (!strcmp(e->path, "..")) {
      size_t len = strlen(dir);
      if (len > 1) dir[len - 1] = 0;  
      char *slash = strrchr(dir, '/');
      if (slash) slash[1] = 0;
      else strcpy(dir, "/");
    } else {
      strlcpy(dir, e->path, sizeof dir);
      if (dir[strlen(dir) - 1] != '/') strlcat(dir, "/", sizeof dir);
    }
    sel = 0;
    top = 0;
    scrollPos = 0;
    refresh();
    return true;
  }

  bool tickScroll(uint32_t now) {
    if (n == 0 || sel >= n || now - scrollAt <= cfg::SCROLL_MS) return false;
    if (u8::length(items[sel].title) <= WIDTH) return false;
    scrollAt = now;
    scrollPos++;
    return true;
  }

  void draw(Oled &oled) {
    if (n == 0) { oled.setCursor(0, 3); oled.print("Нет файлов"); return; }
    const int VIS = cfg::SCREEN_ROWS, MARGIN = 1;
    if (sel < top + MARGIN) top = sel - MARGIN > 0 ? sel - MARGIN : 0;
    if (sel > top + VIS - 1 - MARGIN) top = sel - VIS + 1 + MARGIN;
    top = constrain(top, 0, n - VIS > 0 ? n - VIS : 0);
    char shown[cfg::TITLE_LEN * 2 + 16], line[cfg::TITLE_LEN * 2 + 24];
    for (int row = 0; row < VIS && top + row < n; row++) {
      int idx = top + row;
      const char *title = items[idx].title;
      if (u8::length(title) <= WIDTH) strlcpy(shown, title, sizeof shown);
      else if (idx == sel) marquee(shown, sizeof shown, title, scrollPos);
      else u8::copy(shown, sizeof shown, title, 0, WIDTH);
      snprintf(line, sizeof line, "%s%s", idx == sel ? "> " : "  ", shown);
      oled.setCursor(0, row);
      if (markMode && isMarked(items[idx].path)) {  // инверсия всей строки
        size_t l = strlen(line);
        for (int k = u8::length(line); k < cfg::CHARS_PER_LINE && l + 1 < sizeof line; k++) line[l++] = ' ';
        line[l] = 0;
        oled.invertText(true);
        oled.print(line);
        oled.invertText(false);
      } else oled.print(line);
    }
  }

 private:
  bool markMode = false;
  int nMarks = 0;
  char marks[cfg::MAX_MARKS][cfg::PATH_LEN];
  enum { WIDTH = cfg::CHARS_PER_LINE - 2 };
  Entry items[cfg::MAX_FILES];
  int n = 0, sel = 0, top = 0, scrollPos = 0;
  uint32_t scrollAt = 0;
  char dir[cfg::PATH_LEN + 2];

  void add(const char *path, const char *title, bool isDir, bool isImage) {
    if (n >= cfg::MAX_FILES || strlen(path) >= sizeof items[0].path) return;  
    Entry &e = items[n++];
    strlcpy(e.path, path, sizeof e.path);
    strlcpy(e.title, title, sizeof e.title);
    u8::trimPartial(e.title);
    e.isDir = isDir;
    e.isImage = isImage;
  }
  static void marquee(char *out, size_t cap, const char *title, int pos) {
    char loop[cfg::TITLE_LEN * 2 + 8];
    snprintf(loop, sizeof loop, "%s    %s", title, title);
    int period = u8::length(title) + 4;
    u8::copy(out, cap, loop, pos % period, WIDTH);
  }
};

class HtmlOut {
 public:
  explicit HtmlOut(WebServer &s) : srv(s), n(0) {}
  void begin() {
    srv.setContentLength(CONTENT_LENGTH_UNKNOWN);
    srv.send(200, "text/html; charset=utf-8", "");
  }
  void raw(const char *s) {
    size_t l = strlen(s);
    while (l) {
      size_t room = sizeof buf - n, c = l < room ? l : room;
      memcpy(buf + n, s, c);
      n += c; s += c; l -= c;
      if (n == sizeof buf) flush();
    }
  }
  void num(long v) { char t[16]; snprintf(t, sizeof t, "%ld", v); raw(t); }
  void esc(const char *s) { escN(s, strlen(s)); }
  void escN(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
      switch (s[i]) {
        case '&': raw("&amp;"); break;
        case '<': raw("&lt;"); break;
        case '>': raw("&gt;"); break;
        case '"': raw("&quot;"); break;
        case '\'': raw("&#39;"); break;
        default: put(s[i]);
      }
    }
  }
  void url(const char *s) {
    char enc[cfg::PATH_LEN * 3 + 4];
    str::urlEncode(enc, sizeof enc, s);
    raw(enc);
  }
  void end() {
    flush();
    srv.sendContent("", 0);  
  }

 private:
  WebServer &srv;
  char buf[768];
  size_t n;
  void put(char c) { buf[n++] = c; if (n == sizeof buf) flush(); }
  void flush() { if (n) { srv.sendContent(buf, n); n = 0; } }  
};

namespace gfx {
constexpr int W = 128, H = 64, ROW_BYTES = W / 8;
inline void toOled(const uint8_t *src, uint8_t *dst) {
  memset(dst, 0, cfg::IMG_BYTES);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      if ((src[y * ROW_BYTES + x / 8] >> (7 - x % 8)) & 1) dst[(y / 8) * W + x] |= (uint8_t)(1 << (y % 8));
}
inline void fromOled(const uint8_t *src, uint8_t *dst) {
  memset(dst, 0, cfg::IMG_BYTES);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      if ((src[(y / 8) * W + x] >> (y % 8)) & 1) dst[y * ROW_BYTES + x / 8] |= (uint8_t)(0x80 >> (x % 8));
}
inline int hexVal(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }
// вытаскивает байты вида 0x3C из текста; возвращает их количество (могут быть лишние - их считаем, но не храним)
inline size_t parseHex(const char *code, uint8_t *out, size_t cap) {
  size_t count = 0;
  const char *p = code;
  while (*p) {
    if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }        // комментарий //
    if (p[0] == '/' && p[1] == '*') { p += 2; while (*p && !(p[0] == '*' && p[1] == '/')) p++; if (*p) p += 2; continue; }  // /* */
    bool prevWord = p > code && isalnum((uint8_t)p[-1]);  // "1280x640" не считаем байтом
    if (!prevWord && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
      p += 2;
      int v = 0, d = 0;
      while (d < 2 && isxdigit((uint8_t)*p)) { v = v * 16 + hexVal(*p); p++; d++; }
      if (d) { if (count < cap) out[count] = (uint8_t)v; count++; }
    } else p++;
  }
  return count;
}
}  

struct WebHooks {
  virtual void filesChanged() = 0;    // файлы изменены из браузера
  virtual void displayChanged() = 0;  // сменилась ориентация экрана
  virtual void sleepChanged() = 0;    // сменился таймаут сна
  virtual ~WebHooks() {}
};

class WebUI {
 public:
  WebUI(Settings &s, FileBrowser &f, WebHooks &h)
      : settings(s), files(f), hooks(h), server(80), apIP(192, 168, 4, 1), running(false) {}

  void setup() {  // маршруты регистрируются один раз
    on("/", HTTP_GET, &WebUI::pageRoot);
    on("/settings", HTTP_GET, &WebUI::pageSettings);
    on("/edit", HTTP_GET, &WebUI::pageEdit);
    server.on("/upload", HTTP_POST, [this]() { doUpload(); }, [this]() { uploadChunk(); });
    on("/mkdir", HTTP_POST, &WebUI::doMkdir);
    on("/rmdir", HTTP_POST, &WebUI::doRmdir);
    server.on("/save", HTTP_POST, [this]() { doSave(); }, [this]() { uploadChunk(); });
    on("/delete", HTTP_POST, &WebUI::doDelete);
    on("/save_wifi", HTTP_POST, &WebUI::doSaveWifi);
    on("/save_ai", HTTP_POST, &WebUI::doSaveAi);
    on("/save_lefty", HTTP_POST, &WebUI::doSaveLefty);
    on("/save_sleep", HTTP_POST, &WebUI::doSaveSleep);
    on("/save_prompt", HTTP_POST, &WebUI::doSavePrompt);
    on("/save_theme", HTTP_POST, &WebUI::doSaveTheme);
    on("/edit_img", HTTP_GET, &WebUI::pageEditImg);
    on("/upload_img", HTTP_POST, &WebUI::doUploadImg);
    on("/save_img", HTTP_POST, &WebUI::doSaveImg);
    on("/generate_204", HTTP_GET, &WebUI::redirectHome);  
    on("/gen_204", HTTP_GET, &WebUI::redirectHome);
    on("/hotspot-detect.html", HTTP_GET, &WebUI::redirectHome);
    server.onNotFound([this]() { redirectHome(); });
  }
  void start() {
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(settings.ssid, settings.pass[0] ? settings.pass : nullptr);
    dns.start(53, "*", apIP);
    LittleFS.remove(TMP);
    server.begin();
    running = true;
  }
  void stop() {
    dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    running = false;
  }
  void service() { dns.processNextRequest(); server.handleClient(); }
  bool active() const { return running; }
  const IPAddress &ip() const { return apIP; }

 private:
  enum { DIR_BUF = cfg::PATH_LEN + 2 };
  struct Theme { const char *bg, *card, *txt, *acc, *brd; };
  struct Msg { const char *code; bool err; const char *text; };

  Settings &settings;
  FileBrowser &files;
  WebHooks &hooks;
  WebServer server;
  DNSServer dns;
  IPAddress apIP;
  bool running;
  uint8_t imgA[cfg::IMG_BYTES], imgB[cfg::IMG_BYTES];  // рабочие буферы для картинок

  void on(const char *uri, HTTPMethod m, void (WebUI::*h)()) {
    server.on(uri, m, [this, h]() { (this->*h)(); });
  }

  static const Theme &theme(int i) {
    static const Theme T[9] = {
      {"#0f172a", "#1e293b", "#e2e8f0", "#38bdf8", "#334155"},
      {"#121212", "#1e1e1e", "#e6e6e6", "#c9a876", "#2c2c2c"},
      {"#0d1117", "#161b22", "#c9d1d9", "#3fb950", "#30363d"},
      {"#282a36", "#353746", "#f0f0f5", "#bd93f9", "#565873"},
      {"#2e3440", "#3b4252", "#e5e9f0", "#81a1c1", "#4c566a"},
      {"#002b36", "#073642", "#93a1a1", "#2aa198", "#586e75"},
      {"#1f1216", "#2b1a20", "#f0e0e6", "#c2607a", "#4a2a35"},
      {"#f5f7fa", "#ffffff", "#1e293b", "#2563eb", "#e2e8f0"},
      {"#f6ecd9", "#fffaf0", "#3f342a", "#b5651d", "#e3d5bd"}};
    return T[i < 0 ? 0 : i > 8 ? 8 : i];
  }

  void head(HtmlOut &o, const char *title, bool settingsTab) {
    o.begin();
    o.raw("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>");
    o.esc(title);
    o.raw("</title><style>*{box-sizing:border-box;margin:0;padding:0}:root{--bg:");
    const Theme &t = theme(settings.webTheme);
    o.raw(t.bg); o.raw(";--card:"); o.raw(t.card); o.raw(";--txt:"); o.raw(t.txt);
    o.raw(";--acc:"); o.raw(t.acc); o.raw(";--brd:"); o.raw(t.brd);
    o.raw(";}"
      "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--txt);padding:16px;max-width:600px;margin:auto;line-height:1.5}"
      "nav{display:flex;gap:10px;margin-bottom:16px;background:var(--card);padding:6px;border-radius:10px;border:1px solid var(--brd)}"
      "nav a{flex:1;text-align:center;padding:10px;color:var(--txt);text-decoration:none;font-weight:600;border-radius:6px;transition:background .2s}"
      "nav a.active{background:var(--acc);color:var(--bg)}"
      "h2{color:var(--acc);font-size:20px;margin-bottom:12px;font-weight:700}"
      "h3{color:var(--txt);opacity:.8;margin:16px 0 8px;font-size:13px;text-transform:uppercase;letter-spacing:.05em}"
      ".card{background:var(--card);border-radius:12px;padding:16px;margin-bottom:16px;border:1px solid var(--brd)}"
      "input,textarea,select{width:100%;margin:6px 0 12px;padding:12px;border-radius:8px;border:1px solid var(--brd);background:var(--bg);color:var(--txt);font-size:15px;outline:none}"
      "input:focus,textarea:focus,select:focus{border-color:var(--acc)}"
      "textarea{min-height:140px;resize:vertical;font-family:inherit}"
      "button{width:100%;padding:12px;border:none;border-radius:8px;background:var(--acc);color:var(--bg);font-size:15px;font-weight:700;cursor:pointer}"
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
      ".toggle-label input{width:auto;margin:0}"
      "</style></head><body><nav><a href='/' class='");
    o.raw(settingsTab ? "" : "active");
    o.raw("'>Шпаргалки</a><a href='/settings' class='");
    o.raw(settingsTab ? "active" : "");
    o.raw("'>Настройки</a></nav>");
  }

  void message(HtmlOut &o, const char *code) {
    static const Msg MSGS[] = {
      {"ok", false, "Успешно!"},
      {"saved", false, "Настройки сохранены!"},
      {"nospace", true, "Ошибка: не хватает места!"},
      {"empty", true, "Ошибка: введите заголовок!"},
      {"badname", true, "Ошибка: недопустимое имя"},
      {"toolong", true, "Ошибка: слишком длинный путь"},
      {"err_rmdir", true, "Ошибка при удалении папки!"},
      {"imgname", true, "Ошибка: введите имя файла картинки!"},
      {"imgsize", true, "Ошибка: нужно ровно 1024 байта (128x64), проверьте вставленный код"},
      {"wifierr", true, "Ошибка: имя сети 1-32 символа, пароль пустой или 8-63 символа!"},
      {"aierr", true, "Ошибка: проверьте имя сети (до 32), пароль (до 63), API-ключ и модель"}};
    if (!code || !*code) return;
    for (size_t i = 0; i < sizeof MSGS / sizeof MSGS[0]; i++) {
      if (strcmp(MSGS[i].code, code)) continue;
      o.raw(MSGS[i].err ? "<div class='msg-err'>" : "<div class='msg-ok'>");
      o.raw(MSGS[i].text);
      o.raw("</div>");
      return;
    }
  }

  void hidden(HtmlOut &o, const char *name, const char *value) {
    o.raw("<input type='hidden' name='"); o.raw(name); o.raw("' value='"); o.esc(value); o.raw("'>");
  }

  void argDir(char *out) {
    String d = server.hasArg("dir") ? server.arg("dir") : String("/");
    if (!str::safePath(d.c_str())) { strcpy(out, "/"); return; }
    strlcpy(out, d.c_str(), DIR_BUF);
    if (out[strlen(out) - 1] != '/') strlcat(out, "/", DIR_BUF);
  }
  void redirect(const char *dir, const char *msg) {
    char enc[cfg::PATH_LEN * 3 + 4], loc[cfg::PATH_LEN * 3 + 40];
    str::urlEncode(enc, sizeof enc, dir);
    snprintf(loc, sizeof loc, "/?dir=%s&msg=%s", enc, msg);
    server.sendHeader("Location", String(loc), true);
    server.send(302, "text/plain", "");
  }
  void redirectSettings(const char *msg) {
    char loc[48];
    snprintf(loc, sizeof loc, "/settings?msg=%s", msg);
    server.sendHeader("Location", String(loc), true);
    server.send(302, "text/plain", "");
  }
  void redirectHome() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  }

  static void writeText(const char *path, const char *title, const String &content) {
    File f = LittleFS.open(path, "w");
    if (!f) return;
    for (const char *p = title; *p; p++) f.print(*p == '\n' || *p == '\r' ? ' ' : *p);
    f.print('\n');
    f.print(content);
    f.close();
  }
  static void newFileName(const char *dir, char *out, size_t cap) {
    unsigned long maxId = 0;
    fsx::scanDir(dir, [&maxId](const char *p, File &f) {
      const char *name = str::baseName(p);
      if (!f.isDirectory() && !strncmp(name, "d_", 2)) { unsigned long id = strtoul(name + 2, nullptr, 10); if (id > maxId) maxId = id; }
    });
    snprintf(out, cap, "%sd_%lu.txt", dir, maxId + 1);
  }

  void itemRow(HtmlOut &o, const char *icon, const char *title, size_t size, const char *editAction,
               const char *confirmText, const char *path, const char *dir) {
    char b[24];
    if (size < 1024) snprintf(b, sizeof b, "%u Б", (unsigned)size);
    else snprintf(b, sizeof b, "%.1f КБ", size / 1024.0);
    o.raw("<div class='item'><div class='item-hdr'><div class='title'>");
    o.raw(icon); o.esc(title);
    o.raw("</div><span class='badge'>"); o.raw(b);
    o.raw("</span></div><div class='row'><form method='GET' action='"); o.raw(editAction); o.raw("'>");
    hidden(o, "f", path); hidden(o, "dir", dir);
    o.raw("<button class='edit' type='submit'>Изменить</button></form>"
          "<form method='POST' action='/delete' onsubmit=\"return confirm('");
    o.raw(confirmText);
    o.raw("')\">");
    hidden(o, "f", path); hidden(o, "dir", dir);
    o.raw("<button class='del' type='submit'>Удалить</button></form></div></div>");
  }

  void pageRoot() {
    char dir[DIR_BUF];
    argDir(dir);
    HtmlOut o(server);
    head(o, "ESP SuperminiReader", false);

    size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
    int pct = total ? (int)(used * 100 / total) : 0;
    char b[128];
    snprintf(b, sizeof b, "<div><span class='hint'>Память:</span> %u / %u Б (%d%%)</div>", (unsigned)used, (unsigned)total, pct);
    o.raw(b);
    snprintf(b, sizeof b, "<div class='bar'><div class='fill' style='width:%d%%'></div></div>", pct);
    o.raw(b);
    message(o, server.arg("msg").c_str());

    o.raw("<div class='card'><h3>Создать папку</h3><form method='POST' action='/mkdir'>");
    hidden(o, "dir", dir);
    o.raw("<input type='text' name='dirname' placeholder='Имя папки' required><button type='submit'>Создать</button></form></div>");

    o.raw("<div class='card'><h3>Добавить шпаргалку</h3><script>function sendTxt(f){var d=new FormData();['dir','f','title'].forEach(function(n){var e=f.elements[n];if(e)d.append(n,e.value)});var bl=new Blob([f.elements['content'].value],{type:'text/plain'});d.append('len',bl.size);d.append('content',bl,'c.txt');var b=f.querySelector('button[type=submit]');b.disabled=true;fetch(f.action,{method:'POST',body:d}).then(function(r){location.href=r.url}).catch(function(){b.disabled=false;alert('Ошибка загрузки')});return false}</script><form method='POST' action='/upload' enctype='multipart/form-data' onsubmit=\"return sendTxt(this)\">");
    hidden(o, "dir", dir);
    o.raw("<input type='text' name='title' placeholder='Заголовок шпаргалки' required>"
          "<textarea name='content' placeholder='Содержимое файла'></textarea>"
          "<button type='submit'>Загрузить файл</button></form></div>");
    o.raw("<div class='card'><h3>Загрузить картинку (128x64)</h3><form method='POST' action='/upload_img'>");
    hidden(o, "dir", dir);
    o.raw(R"JS(<label style='display:block;padding:14px;border:2px dashed var(--acc);border-radius:10px;text-align:center;color:var(--acc);font-weight:600;cursor:pointer;margin:6px 0 12px;position:relative;overflow:hidden'><span id='pn'>&#128444; Выбрать картинку</span><input type='file' id='pf' accept='image/*' style='position:absolute;inset:0;width:100%;height:100%;margin:0;padding:0;border:0;opacity:0;cursor:pointer;font-size:16px'></label>
<label style='display:flex;align-items:center;gap:10px;font-size:13px;margin-bottom:10px'>Порог<input type='range' id='pt' min='1' max='254' value='128' style='flex:1;width:auto;margin:0;padding:0;border:0;background:none;accent-color:var(--acc)'></label>
<label style='display:flex;align-items:center;gap:8px;font-size:13px;margin-bottom:10px'><input type='checkbox' id='pi' style='width:auto;margin:0;accent-color:var(--acc)'>Инвертировать</label>
<canvas id='pc' width='128' height='64' style='width:100%;image-rendering:pixelated;background:#000;border-radius:8px;display:none;margin-bottom:8px'></canvas>
<script>(function(){var f=document.currentScript.parentNode,fi=f.querySelector('#pf'),th=f.querySelector('#pt'),iv=f.querySelector('#pi'),c=f.querySelector('#pc'),x=c.getContext('2d',{willReadFrequently:true}),im=null;
function go(){if(!im)return;x.fillStyle='#000';x.fillRect(0,0,128,64);var s=Math.min(128/im.width,64/im.height),w=im.width*s,h=im.height*s;var ox=(128-w)/2,oy=(64-h)/2;x.imageSmoothingQuality='high';x.drawImage(im,ox,oy,w,h);var q=x.getImageData(Math.ceil(ox),Math.ceil(oy),1,1).data;x.fillStyle='rgb('+q[0]+','+q[1]+','+q[2]+')';x.fillRect(0,0,128,64);x.drawImage(im,ox,oy,w,h);
var g=x.getImageData(0,0,128,64),d=g.data,o=[],t=+th.value,v=iv.checked;
for(var y=0;y<64;y++)for(var b=0;b<16;b++){var n=0;for(var k=0;k<8;k++){var i=(y*128+b*8+k)*4,on=(d[i]*.299+d[i+1]*.587+d[i+2]*.114)>t;if(on!=v)n|=128>>k;d[i]=d[i+1]=d[i+2]=(on!=v)?255:0;d[i+3]=255}o.push('0x'+(n+256).toString(16).slice(1).toUpperCase())}
x.putImageData(g,0,0);c.style.display='block';f.imgcode.value=o.join(', ')}
fi.onchange=function(){var r=fi.files[0];if(!r)return;f.querySelector('#pn').textContent=r.name;if(!f.imgname.value)f.imgname.value=r.name.replace(/\.[^.]*$/,'').replace(/[^\w-]/g,'_').slice(0,30);
var u=URL.createObjectURL(r),i=new Image();i.onload=function(){im=i;go();URL.revokeObjectURL(u)};i.onerror=function(){im=null;f.imgcode.value='';alert('Не удалось открыть картинку');URL.revokeObjectURL(u)};i.src=u};
th.oninput=iv.onchange=go})();</script>)JS");
    o.raw("<input type='text' name='imgname' placeholder='Имя файла (без .h)' required>"
          "<textarea name='imgcode' placeholder='Выберите картинку выше или вставьте байты: 0x3C, 0x00, ... (ровно 1024 байта)' required></textarea>"
          "<button type='submit'>Загрузить картинку</button></form></div>");
    o.raw("<h3>Содержимое папки: "); o.esc(dir); o.raw("</h3>");

    if (strcmp(dir, "/") != 0) { 
      char parent[DIR_BUF];
      strlcpy(parent, dir, sizeof parent);
      parent[strlen(parent) - 1] = 0;
      char *slash = strrchr(parent, '/');
      if (slash) slash[1] = 0;
      o.raw("<div class='item'><a href='/?dir="); o.url(parent); o.raw("'>&#128193; .. (Назад)</a></div>");
    }

    fsx::scanDir(dir, [&](const char *p, File &f) {
      if (!str::safePath(p)) return;
      const char *name = str::baseName(p);
      if (f.isDirectory()) {
        o.raw("<div class='item'><div class='item-hdr'><a href='/?dir="); o.url(p); o.raw("'>&#128193; "); o.esc(name);
        o.raw("</a><form method='POST' action='/rmdir' onsubmit=\"return confirm('Удалить папку и всё её содержимое?')\" style='margin:0;'>");
        hidden(o, "dir", p);
        o.raw("<button class='del' type='submit' style='padding:4px 8px;font-size:12px;width:auto'>Удалить</button></form></div></div>");
      }
      else if (str::endsWith(p, ".h")) {
        itemRow(o, "&#128444; ", name, f.size(), "/edit_img", "Удалить картинку?", p, dir);
      }
      else {
        char title[cfg::TITLE_LEN];
        fsx::readLine(f, title, sizeof title);
        str::trim(title);
        u8::trimPartial(title);
        itemRow(o, "", title[0] ? title : "Без названия", f.size(), "/edit", "Удалить шпаргалку?", p, dir);
      }
    });
    o.raw("</body></html>");
    o.end();
  }

  void pageEdit() {
    if (!server.hasArg("f")) { redirectHome(); return; }
    String fn = server.arg("f");
    char dir[DIR_BUF];
    argDir(dir);
    if (!str::safePath(fn.c_str()) || !LittleFS.exists(fn.c_str())) { redirectHome(); return; }
    File f = LittleFS.open(fn.c_str(), "r");
    if (!f) { redirectHome(); return; }
    char title[256];
    fsx::readLine(f, title, sizeof title);
    str::trim(title);

    HtmlOut o(server);
    head(o, "Редактирование", false);
    o.raw("<h2>Изменить шпаргалку</h2><script>function sendTxt(f){var d=new FormData();['dir','f','title'].forEach(function(n){var e=f.elements[n];if(e)d.append(n,e.value)});var bl=new Blob([f.elements['content'].value],{type:'text/plain'});d.append('len',bl.size);d.append('content',bl,'c.txt');var b=f.querySelector('button[type=submit]');b.disabled=true;fetch(f.action,{method:'POST',body:d}).then(function(r){location.href=r.url}).catch(function(){b.disabled=false;alert('Ошибка загрузки')});return false}</script><form method='POST' action='/save' enctype='multipart/form-data' onsubmit=\"return sendTxt(this)\">");
    hidden(o, "f", fn.c_str()); hidden(o, "dir", dir);
    o.raw("<input type='text' name='title' value='"); o.esc(title);
    o.raw("' required><textarea name='content'>");
    uint8_t chunk[128];
    int got;
    while ((got = (int)f.read(chunk, sizeof chunk)) > 0) o.escN((const char *)chunk, (size_t)got);
    f.close();
    o.raw("</textarea><button type='submit'>Сохранить</button></form>"
          "<form method='POST' action='/delete' style='margin-top:10px;' onsubmit=\"return confirm('Удалить шпаргалку?')\">");
    hidden(o, "f", fn.c_str()); hidden(o, "dir", dir);
    o.raw("<button class='del' type='submit'>Удалить</button></form><p style='margin-top:16px;'><a href='/?dir=");
    o.url(dir);
    o.raw("'>&larr; Назад</a></p></body></html>");
    o.end();
  }

  // Потоковая приём текста: содержимое пишется прямо в файл кусками, без больших String в памяти
  static constexpr const char *TMP = "/_up.tmp";
  File upFile;
  bool upErr = false;
  size_t upBytes = 0;

  void uploadChunk() {
    HTTPUpload &u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
      if (upFile) upFile.close();
      upErr = false;
      upBytes = 0;
      upFile = LittleFS.open(TMP, "w");
      if (!upFile) { upErr = true; return; }
      String t = server.arg("title");
      t.trim();
      for (size_t i = 0; i < t.length(); i++) {
        char c = t[i];
        if (upFile.write((uint8_t)(c == '\n' || c == '\r' ? ' ' : c)) != 1) upErr = true;
      }
      if (upFile.write((uint8_t)'\n') != 1) upErr = true;
    } else if (u.status == UPLOAD_FILE_WRITE) {
      upBytes += u.currentSize;
      if (upFile && !upErr && upFile.write(u.buf, u.currentSize) != u.currentSize) upErr = true;
    } else if (u.status == UPLOAD_FILE_END) {
      if (upFile) upFile.close();
    } else if (u.status == UPLOAD_FILE_ABORTED) {
      upErr = true;
      if (upFile) upFile.close();
    }
  }

  // Приём не стартовал (пустое содержимое) — создаём временный файл только с заголовком
  bool ensureTmp(const String &title) {
    if (LittleFS.exists(TMP)) return true;
    File f = LittleFS.open(TMP, "w");
    if (!f) return false;
    for (size_t i = 0; i < title.length(); i++) f.write((uint8_t)(title[i] == '\n' || title[i] == '\r' ? ' ' : title[i]));
    f.write((uint8_t)'\n');
    f.close();
    return true;
  }

  void doUpload() {
    char dir[DIR_BUF];
    argDir(dir);
    String title = server.arg("title");
    title.trim();
    if (!title.length()) { LittleFS.remove(TMP); redirect(dir, "empty"); return; }
    if (upFile) upFile.close();
    if (upErr || (size_t)server.arg("len").toInt() != upBytes || !ensureTmp(title)) { LittleFS.remove(TMP); upErr = false; upBytes = 0; redirect(dir, "nospace"); return; }
    upBytes = 0;
    char path[cfg::PATH_LEN * 2];
    newFileName(dir, path, sizeof path);
    if (strlen(path) >= (size_t)cfg::PATH_LEN) { LittleFS.remove(TMP); redirect(dir, "toolong"); return; }
    bool ok = LittleFS.rename(TMP, path);
    if (!ok) LittleFS.remove(TMP);
    hooks.filesChanged();
    redirect(dir, ok ? "ok" : "nospace");
  }

  void doSave() {
    String fn = server.arg("f"), title = server.arg("title");
    char dir[DIR_BUF];
    argDir(dir);
    title.trim();
    if (upFile) upFile.close();
    if (!title.length() || !str::safePath(fn.c_str()) || !LittleFS.exists(fn.c_str())) { LittleFS.remove(TMP); redirect(dir, "empty"); return; }
    if (upErr || (size_t)server.arg("len").toInt() != upBytes || !ensureTmp(title)) { LittleFS.remove(TMP); upErr = false; upBytes = 0; redirect(dir, "nospace"); return; }
    upBytes = 0;
    LittleFS.remove(fn.c_str());
    bool ok = LittleFS.rename(TMP, fn.c_str());
    if (!ok) LittleFS.remove(TMP);
    hooks.filesChanged();
    redirect(dir, ok ? "ok" : "nospace");
  }

  void doDelete() {
    String fn = server.arg("f");
    char dir[DIR_BUF];
    argDir(dir);
    if (str::safePath(fn.c_str())) LittleFS.remove(fn.c_str());
    hooks.filesChanged();
    redirect(dir, "ok");
  }

  void doMkdir() {
    char dir[DIR_BUF];
    argDir(dir);
    String name = server.arg("dirname");
    name.trim();
    if (!name.length() || name.length() > 32 || name.indexOf('/') >= 0 || name.indexOf("..") >= 0 || !str::safeChars(name.c_str())) {
      redirect(dir, "badname");
      return;
    }
    char path[cfg::PATH_LEN * 2];
    snprintf(path, sizeof path, "%s%s", dir, name.c_str());
    if (strlen(path) >= (size_t)cfg::PATH_LEN) { redirect(dir, "toolong"); return; }
    LittleFS.mkdir(path);
    hooks.filesChanged();
    redirect(dir, "ok");
  }

  void doRmdir() {
    String d = server.arg("dir");
    if (!str::safePath(d.c_str()) || d == "/") { redirect("/", "err_rmdir"); return; }
    char parent[DIR_BUF];
    strlcpy(parent, d.c_str(), sizeof parent);
    char *slash = strrchr(parent, '/');
    if (slash) slash[1] = 0;
    bool ok = fsx::removeDirRecursive(d.c_str());
    hooks.filesChanged();
    redirect(parent, ok ? "ok" : "err_rmdir");
  }

  void pageEditImg() {
    if (!server.hasArg("f")) { redirectHome(); return; }
    String fn = server.arg("f");
    char dir[DIR_BUF];
    argDir(dir);
    if (!str::safePath(fn.c_str()) || !LittleFS.exists(fn.c_str())) { redirectHome(); return; }
    File f = LittleFS.open(fn.c_str(), "r");
    if (!f) { redirectHome(); return; }
    size_t n = f.read(imgA, cfg::IMG_BYTES);
    f.close();
    if (n < cfg::IMG_BYTES) memset(imgA + n, 0, cfg::IMG_BYTES - n);
    gfx::fromOled(imgA, imgB);  

    HtmlOut o(server);
    head(o, "Редактирование картинки", false);
    o.raw("<h2>Изменить: "); o.esc(str::baseName(fn.c_str()));
    o.raw("</h2><form method='POST' action='/save_img'>");
    hidden(o, "f", fn.c_str()); hidden(o, "dir", dir);
    o.raw("<textarea name='imgcode' style='min-height:220px'>");
    char h[8];
    for (size_t i = 0; i < cfg::IMG_BYTES; i++) {
      snprintf(h, sizeof h, "0x%02X, ", imgB[i]);
      o.raw(h);
      if ((i + 1) % 16 == 0) o.raw("\n");
    }
    o.raw("</textarea><button type='submit'>Сохранить</button></form>"
          "<form method='POST' action='/delete' style='margin-top:10px;' onsubmit=\"return confirm('Удалить картинку?')\">");
    hidden(o, "f", fn.c_str()); hidden(o, "dir", dir);
    o.raw("<button class='del' type='submit'>Удалить</button></form><p style='margin-top:16px;'><a href='/?dir=");
    o.url(dir);
    o.raw("'>&larr; Назад</a></p></body></html>");
    o.end();
  }

  bool parseImage(const String &code) {
    size_t n = gfx::parseHex(code.c_str(), imgB, cfg::IMG_BYTES);
    if (n != cfg::IMG_BYTES) return false;
    gfx::toOled(imgB, imgA);
    return true;
  }
  // Пишет через временный файл: при ошибке записи старая картинка остаётся целой
  static bool writeBin(const char *path, const uint8_t *data, size_t len) {
    const char *tmp = "/_up.tmp";
    File f = LittleFS.open(tmp, "w");
    if (!f) return false;
    bool ok = f.write(data, len) == len;
    f.close();
    if (ok) { LittleFS.remove(path); ok = LittleFS.rename(tmp, path); }
    if (!ok) LittleFS.remove(tmp);
    return ok;
  }

  void doUploadImg() {
    char dir[DIR_BUF];
    argDir(dir);
    String raw = server.arg("imgname"), code = server.arg("imgcode");
    char name[40];
    size_t k = 0;
    for (size_t i = 0; i < raw.length() && k + 1 < sizeof name; i++) { 
      char c = raw[i];
      if (c != '/' && c != '\\' && c != '.') name[k++] = c;
    }
    name[k] = 0;
    str::trim(name);
    if (!name[0] || strlen(name) > 30 || !str::safeChars(name)) { redirect(dir, "imgname"); return; }
    if (!parseImage(code)) { redirect(dir, "imgsize"); return; }
    if (!fsx::hasSpace((long)cfg::IMG_BYTES + 8)) { redirect(dir, "nospace"); return; }
    char path[cfg::PATH_LEN * 2];
    snprintf(path, sizeof path, "%s%s.h", dir, name);
    for (int n = 2; LittleFS.exists(path) && n < 1000; n++) snprintf(path, sizeof path, "%s%s_%d.h", dir, name, n);  // не затираем чужую картинку
    if (LittleFS.exists(path)) { redirect(dir, "imgname"); return; }
    if (strlen(path) >= (size_t)cfg::PATH_LEN) { redirect(dir, "toolong"); return; }
    bool ok = writeBin(path, imgA, cfg::IMG_BYTES);
    hooks.filesChanged();
    redirect(dir, ok ? "ok" : "nospace");
  }

  void doSaveImg() {
    String fn = server.arg("f"), code = server.arg("imgcode");
    char dir[DIR_BUF];
    argDir(dir);
    if (!str::safePath(fn.c_str()) || !LittleFS.exists(fn.c_str())) { redirect(dir, "empty"); return; }
    if (!parseImage(code)) { redirect(dir, "imgsize"); return; }
    bool ok = writeBin(fn.c_str(), imgA, cfg::IMG_BYTES);
    hooks.filesChanged();
    redirect(dir, ok ? "ok" : "nospace");
  }

  // ---------- настройки
  void pageSettings() {
    static const char *const NAMES[9] = {"Синяя", "Черно/Бежевая", "Зеленая", "Фиолетовая", "Голубая",
                                         "Бирюзовая", "Бордовая", "Светлая", "Сепия/Бумага"};
    HtmlOut o(server);
    head(o, "Настройки", true);
    o.raw("<h2>Настройки устройства</h2>");
    message(o, server.arg("msg").c_str());
    char b[112];

    o.raw("<div class='card'><h3>Внешний вид</h3><form method='POST' action='/save_theme'><select name='theme'>");
    for (int i = 0; i < 9; i++) {
      snprintf(b, sizeof b, "<option value='%d'%s>%d. %s</option>", i, i == settings.webTheme ? " selected" : "", i + 1, NAMES[i]);
      o.raw(b);
    }
    o.raw("</select><button type='submit'>Применить тему</button></form></div>");

    o.raw("<div class='card'><h3>Параметры Wi-Fi</h3><form method='POST' action='/save_wifi'>"
          "<label class='hint'>Имя сети (SSID):</label><input type='text' name='ssid' value='");
    o.esc(settings.ssid);
    o.raw("' required><label class='hint'>Пароль (минимум 8 символов или пустой):</label><input type='text' name='pass' value='");
    o.esc(settings.pass);
    o.raw("'><button type='submit'>Сохранить Wi-Fi</button></form></div>");

    o.raw("<div class='card'><h3>Нейросеть (ProxyAPI)</h3><form method='POST' action='/save_ai'>"
          "<label class='hint'>Wi-Fi сеть с интернетом для запросов - имя:</label><input type='text' name='sta_ssid' maxlength='32' value='");
    o.esc(settings.staSsid);
    o.raw("'><label class='hint'>Пароль этой сети:</label><input type='text' name='sta_pass' maxlength='63' value='");
    o.esc(settings.staPass);
    o.raw("'><label class='hint'>API-ключ ProxyAPI:</label><div style='position:relative'>"
          "<input type='password' id='ak' name='api_key' autocomplete='off' style='padding-right:46px' value='");
    o.esc(settings.apiKey);
    o.raw("'><button type='button' style='position:absolute;right:6px;top:8px;width:36px;height:36px;padding:0;background:none;color:var(--txt);font-size:18px;opacity:.5' "
          "onclick='var i=document.getElementById(\"ak\"),h=i.type==\"password\";i.type=h?\"text\":\"password\";this.style.opacity=h?1:.5'>&#128065;</button></div>"
          "<label class='hint'>Модель:</label><input type='text' name='model' maxlength='39' value='");
    o.esc(settings.model);
    o.raw("'><button type='submit'>Сохранить</button><p class='hint'>");
    o.raw(settings.aiReady() ? "Готово: в калькуляторе введите 3333." : "Не готово: нужны имя сети и API-ключ.");
    o.raw("</p></form></div>");

    o.raw("<div class='card'><h3>Режим левши</h3><form method='POST' action='/save_lefty'>"
          "<label class='toggle-label'><span>Поворот экрана на 180&deg;</span><input type='checkbox' name='lefty' value='1'");
    if (settings.leftHanded) o.raw(" checked");
    o.raw("></label><br><button type='submit'>Применить</button></form></div>");

    o.raw("<div class='card'><h3>Спящий режим</h3><form method='POST' action='/save_sleep'>"
          "<label class='toggle-label'><span>Включить автозасыпание</span><input type='checkbox' name='sleep_en' value='1'");
    if (settings.sleepEnabled) o.raw(" checked");
    o.raw("></label><br><div class='row'><div><label class='hint'>Минуты</label>"
          "<input type='number' name='min' min='0' max='60' value='");
    o.num(settings.sleepMin);
    o.raw("'></div><div><label class='hint'>Секунды</label>"
          "<input type='number' name='sec' min='0' max='59' value='");
    o.num(settings.sleepSec);
    o.raw("'></div></div><button type='submit'>Сохранить</button></form></div>");

    o.raw("<div class='card'><h3>Суфлёр</h3><form method='POST' action='/save_prompt'>"
          "<label class='hint'>Секунд на строку (от 0.05 до 60, шаг 0.01)</label>"
          "<input type='number' name='speed' min='0.05' max='60' step='0.01' value='");
    snprintf(b, sizeof b, "%d.%02d", settings.promptCs / 100, settings.promptCs % 100);
    o.raw(b);
    o.raw("'><button type='submit'>Сохранить</button></form></div></body></html>");
    o.end();
  }

  void doSaveWifi() {
    String s = server.arg("ssid"), p = server.arg("pass");
    s.trim();
    if (s.length() < 1 || s.length() > 32 || (p.length() > 0 && (p.length() < 8 || p.length() > 63))) {
      redirectSettings("wifierr");
      return;
    }
    strlcpy(settings.ssid, s.c_str(), sizeof settings.ssid);  
    strlcpy(settings.pass, p.c_str(), sizeof settings.pass);
    settings.touch(millis());
    redirectSettings("saved");
  }
  void doSaveAi() {
    String s = server.arg("sta_ssid"), p = server.arg("sta_pass"), k = server.arg("api_key"), m = server.arg("model");
    s.trim(); k.trim(); m.trim();
    bool ok = s.length() <= 32 && p.length() <= 63 && k.length() <= 127 && m.length() <= 39;
    for (size_t i = 0; ok && i < k.length(); i++) ok = (uint8_t)k[i] > 0x20 && (uint8_t)k[i] < 0x7F;
    for (size_t i = 0; ok && i < m.length(); i++) ok = isalnum((uint8_t)m[i]) || strchr("._:/-", m[i]);
    for (size_t i = 0; ok && i < s.length(); i++) ok = (uint8_t)s[i] >= 0x20;
    if (!ok) { redirectSettings("aierr"); return; }
    strlcpy(settings.staSsid, s.c_str(), sizeof settings.staSsid);
    strlcpy(settings.staPass, p.c_str(), sizeof settings.staPass);
    if (k.length()) strlcpy(settings.apiKey, k.c_str(), sizeof settings.apiKey);  // пусто - оставить прежний
    strlcpy(settings.model, m.length() ? m.c_str() : "openai/gpt-4o-mini", sizeof settings.model);
    settings.touch(millis());
    redirectSettings("saved");
  }
  void doSaveLefty() {
    settings.leftHanded = server.hasArg("lefty");
    settings.touch(millis());
    hooks.displayChanged();
    redirectSettings("saved");
  }
  void doSaveSleep() {
    settings.sleepEnabled = server.hasArg("sleep_en");
    settings.sleepMin = constrain((int)server.arg("min").toInt(), 0, 60);
    settings.sleepSec = constrain((int)server.arg("sec").toInt(), 0, 59);
    settings.touch(millis());
    hooks.sleepChanged();
    redirectSettings("saved");
  }
  void doSavePrompt() {
    String v = server.arg("speed");
    v.replace(',', '.');
    float sec = v.toFloat();
    if (!(sec >= 0.05f)) sec = 0.05f;  // заодно ловит NaN и пустое значение
    if (sec > 60.0f) sec = 60.0f;
    settings.promptCs = constrain((int)lroundf(sec * 100.0f), 5, 6000);
    settings.touch(millis());
    redirectSettings("saved");
  }
  void doSaveTheme() {
    if (server.hasArg("theme")) {
      settings.webTheme = constrain((int)server.arg("theme").toInt(), 0, 8);
      settings.touch(millis());
    }
    redirectSettings("saved");
  }
};

class FlappyBird {
 public:
  int best = 0;
  void reset() {
    st = READY; y = 24; vy = 0; score = 0; fr = 0; gnd = 0; last = 0;
    for (int i = 0; i < 3; i++) { p[i].x = 150 + i * PITCH; p[i].gap = random(GMIN, GMAX + 1); p[i].passed = false; }
  }
  void flap() {
    if (st == READY) { st = PLAY; vy = FLAP; }
    else if (st == PLAY) vy = FLAP;
    else if (st == PAUSE) st = PLAY;
    else if (st == DEAD && millis() - deadAt > 600) reset();
  }
  void pause() { if (st == PLAY) st = PAUSE; else if (st == PAUSE) st = PLAY; }

  bool tick(uint32_t now) {
    if (st == PAUSE || st == DEAD || now - last < STEP_MS) return false;
    last = now;
    fr++;
    if (st == READY) { y = 24 + 2.5f * sinf(fr * 0.2f); gnd += SPD; return true; }
    vy += G; if (vy > VMAX) vy = VMAX;
    y += vy;
    if (y < 0) { y = 0; if (vy < 0) vy = 0; }
    if (st == PLAY) {
      gnd += SPD;
      int bx0 = BX + 1, bx1 = BX + BW - 2, by0 = (int)y + 1, by1 = (int)y + BH - 2;
      for (int i = 0; i < 3; i++) {
        Pipe &q = p[i];
        q.x -= SPD;
        if (q.x + PW < 0) { q.x += 3 * PITCH; q.gap = random(GMIN, GMAX + 1); q.passed = false; }
        int px0 = (int)q.x, px1 = px0 + PW - 1;
        if (bx1 >= px0 && bx0 <= px1 && (by0 < q.gap || by1 >= q.gap + GAP)) st = DYING;
        if (!q.passed && q.x + PW < BX) { q.passed = true; score++; }
      }
    }
    if ((int)y + BH >= GY) { y = GY - BH; st = DEAD; deadAt = now; if (score > best) best = score; }
    return true;
  }

  void draw(Oled &o) {
    for (int i = 0; i < 3; i++) {
      int x = (int)p[i].x, gt = p[i].gap, gb = p[i].gap + GAP;
      box(o, x + 2, 0, x + PW - 3, gt - CAPH - 1, OLED_FILL);
      box(o, x + 5, 0, x + 5, gt - CAPH - 1, OLED_CLEAR);
      box(o, x, gt - CAPH, x + PW - 1, gt - 1, OLED_FILL);
      box(o, x + 3, gt - CAPH + 1, x + 3, gt - 2, OLED_CLEAR);
      box(o, x, gb, x + PW - 1, gb + CAPH - 1, OLED_FILL);
      box(o, x + 3, gb + 1, x + 3, gb + CAPH - 2, OLED_CLEAR);
      box(o, x + 2, gb + CAPH, x + PW - 3, GY - 1, OLED_FILL);
      box(o, x + 5, gb + CAPH, x + 5, GY - 1, OLED_CLEAR);
    }
    box(o, 0, GY, 127, GY, OLED_FILL);
    int off = (int)gnd;
    for (int yy = GY + 2; yy < 64; yy++)
      for (int x = 0; x < 128; x++)
        if ((((x + off) >> 2) + ((yy - GY) >> 1)) & 1) o.dot(x, yy, 1);
    // птичка
    static const char *const BIRD[BH] = {
      "...#####....", "..#.....##..", ".#......#.#.", ".#......##..",
      ".#.....#####", ".#.....#...#", "..#.....####", "...#####...."};
    int by = (int)y;
    for (int r = 0; r < BH; r++)
      for (int c = 0; c < BW; c++) o.dot(BX + c, by + r, BIRD[r][c] == '#');
    static const uint8_t WY[4] = {1, 3, 5, 3};
    int wy = (st == PLAY || st == READY) ? WY[(fr >> 1) & 3] : 5;
    box(o, BX + 3, by + wy, BX + 6, by + wy + 1, OLED_FILL);
    // счёт
    if (st != DEAD) {
      char b[8]; snprintf(b, sizeof b, "%d", score);
      int w = strlen(b) * 12;
      box(o, 0, 0, w + 3, 17, OLED_CLEAR);
      o.setScale(2); o.setCursorXY(2, 2); o.print(b); o.setScale(1);
    }
    char b[24];
    if (st == READY) {
      box(o, 33, 36, 94, 47, OLED_CLEAR);
      o.setCursorXY(37, 38); o.print("GET READY!");
    } else if (st == PAUSE) {
      box(o, 41, 28, 86, 40, OLED_CLEAR); box(o, 41, 28, 86, 40, OLED_STROKE);
      o.setCursorXY(49, 31); o.print("PAUSE");
    } else if (st == DEAD) {
      box(o, 20, 8 ,105, 53, OLED_CLEAR); box(o, 20, 8, 105, 53, OLED_STROKE);
      auto ctr = [&](int yy, const char *t){
        o.setCursorXY((128-(int)strlen(t) * 6) / 2, yy);
        o.print(t);
      };
      ctr(12, "GAME OVER");
      box(o, 28, 22, 99, 22, OLED_FILL);
      snprintf(b, sizeof b, "Score: %d", score); ctr(30, b);
      snprintf(b, sizeof b, "Best: %d", best); ctr(40, b);
    }
  }

 private:
  enum St { READY, PLAY, PAUSE, DYING, DEAD };
  enum { GY = 56, BX = 28, BW = 12, BH = 8, PW = 18, CAPH = 6, GAP = 26, PITCH = 64, GMIN = 12, GMAX = 22 };
  static constexpr float G = 0.28f, FLAP = -3.2f, VMAX = 4.5f, SPD = 1.5f;
  static constexpr uint32_t STEP_MS = 33;
  struct Pipe { float x; int gap; bool passed; };
  Pipe p[3];
  St st = READY;
  float y = 24, vy = 0, gnd = 0;
  int score = 0, fr = 0;
  uint32_t last = 0, deadAt = 0;
  static void box(Oled &o, int x0, int y0, int x1, int y1, uint8_t f) {
    if (x1 < 0 || x0 > 127 || y1 < 0 || y0 > 63 || x1 < x0 || y1 < y0) return;
    o.rect(x0 < 0 ? 0 : x0, y0 < 0 ? 0 : y0, x1 > 127 ? 127 : x1, y1 > 63 ? 63 : y1, f);
  }
};

class Tetris {
 public:
  int best = 0;
  void reset() {
    // каждая игра - новая последовательность: аппаратный генератор + время + счётчик запусков
    static uint32_t runs = 0;
    rng = esp_random() ^ (micros() * 2654435761u) ^ (millis() << 7) ^ (++runs * 0x9E3779B9u);
    if (!rng) rng = 0x1234567;
    for (int i = 0; i < 8; i++) nextRand();
    memset(rows, 0, sizeof rows);
    score = 0; lines = 0; level = 1; over = false;
    bagPos = 7;
    nextType = takeFromBag();
    spawn();
    last = millis();
  }
  bool tick(uint32_t now) {
    if (over || now - last < (uint32_t)interval()) return false;
    last = now;
    if (!collides(px, py + 1, rot)) py++;
    else lockPiece();
    return true;
  }
  void move(int dir, uint32_t now) {
    lastMoved = false;
    lastDir = dir;
    lastMoveAt = now;
    if (over) return;
    if (!collides(px + dir, py, rot)) { px += dir; lastMoved = true; }
  }
  // ОК: поворот (в конце игры - новая игра)
  void rotate() {
    if (over) { if (millis() - overAt > 500) reset(); return; }
    int nr = (rot + 1) & 3;
    static const int KICK[5] = {0, -1, 1, -2, 2};
    for (int k : KICK) if (!collides(px + k, py, nr)) { px += k; rot = nr; return; }
  }

  // обе кнопки вместе: если одна из них только что сдвинула фигуру - отменяем сдвиг (ускорение включает флаг fast)
  void chordStart(uint32_t now) {
    if (over) return;
    if (lastMoved && now - lastMoveAt < 160) { px -= lastDir; lastMoved = false; }
  }
  bool fast = false;  // пока обе кнопки зажаты, фигура падает в 3 раза быстрее

  // Экран повёрнут влево на 90°: фигуры падают слева направо (к кнопкам), игровое "вправо" - вверх по экрану
  void draw(Oled &oled) const {
    oled.rect(X0 - 2, Y0 - 2, X0 + H * CELL, Y0 + W * CELL, OLED_STROKE);
    for (int v = 0; v < H; v++)
      for (int u = 0; u < W; u++)
        if (rows[v] & (1u << u)) cell(oled, u, v);
    if (!over) {
      uint16_t m = SHAPES[type][rot];
      for (int i = 0; i < 16; i++) {
        if (!(m & (0x8000 >> i))) continue;
        int cu = px + (i & 3), cv = py + (i >> 2);
        if (cv >= 0) cell(oled, cu, cv);
      }
    } else {
      drawScore(oled);
    }
  }

 private:
  enum { W = 10, H = 20, CELL = 6, X0 = 5, Y0 = 2 };
  static const uint16_t SHAPES[7][4];
  uint16_t rows[H] = {};
  int px = 3, py = 0, rot = 0, type = 0, nextType = 0;
  int score = 0, lines = 0, level = 1, lastDir = 0, bagPos = 7;
  bool over = false, lastMoved = false;
  uint32_t overAt = 0, last = 0, lastMoveAt = 0, rng = 1;
  uint8_t bag[7] = {};

  int interval() const { int v = 700 - (level - 1) * 60; if (v < 90) v = 90; return fast ? (v / 4 < 25 ? 25 : v / 4) : v; }

  uint32_t nextRand() {  // xorshift32, засеивается в reset()
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
  }
  int takeFromBag() {  // "мешок" из 7 фигур: без длинных серий одинаковых
    if (bagPos >= 7) {
      for (int i = 0; i < 7; i++) bag[i] = (uint8_t)i;
      for (int i = 6; i > 0; i--) { int j = (int)(nextRand() % (uint32_t)(i + 1)); uint8_t t = bag[i]; bag[i] = bag[j]; bag[j] = t; }
      bagPos = 0;
    }
    return bag[bagPos++];
  }
  void spawn() {
    type = nextType;
    nextType = takeFromBag();
    rot = 0; px = 3; py = 0;
    if (collides(px, py, rot)) { over = true; overAt = millis(); }
  }
  bool collides(int x, int y, int r) const {
    uint16_t m = SHAPES[type][r];
    for (int i = 0; i < 16; i++) {
      if (!(m & (0x8000 >> i))) continue;
      int cx = x + (i & 3), cy = y + (i >> 2);
      if (cx < 0 || cx >= W || cy >= H) return true;
      if (cy >= 0 && (rows[cy] & (1u << cx))) return true;
    }
    return false;
  }
  void lockPiece() {
    uint16_t m = SHAPES[type][rot];
    for (int i = 0; i < 16; i++) {
      if (!(m & (0x8000 >> i))) continue;
      int cx = px + (i & 3), cy = py + (i >> 2);
      if (cy >= 0) rows[cy] |= (uint16_t)(1u << cx);
    }
    const uint16_t FULL = (1u << W) - 1;
    int cleared = 0;
    for (int y = H - 1; y >= 0;) {
      if (rows[y] == FULL) {
        for (int k = y; k > 0; k--) rows[k] = rows[k - 1];
        rows[0] = 0;
        cleared++;
      } else y--;
    }
    if (cleared) {
      static const int PTS[5] = {0, 100, 300, 500, 800};
      score += PTS[cleared] * level;
      lines += cleared;
      level = lines / 10 + 1;
    }
    if (score > best) best = score;
    spawn();
  }
  // кубик 5x5 с зазором в 1 px и квадратным вырезом по центру; u - столбец поля, v - ряд (сверху вниз)
  static void cell(Oled &o, int u, int v) {
    int sx = X0 + v * CELL, sy = Y0 + (W - 1 - u) * CELL;
    o.rect(sx, sy, sx + 4, sy + 4, OLED_FILL);
    o.rect(sx + 1, sy + 1, sx + 3, sy + 3, OLED_CLEAR);  // квадратный вырез 3x3
  }

  // Конец игры: рамка со счётом, цифры повёрнуты так же, как поле (3x5 пикселей, масштаб 2)
  void drawScore(Oled &oled) const {
    static const uint8_t DIG[10][5] = {{7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
                                       {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}};
    char b[12];
    snprintf(b, sizeof b, "%d", score);
    int n = (int)strlen(b), S = 2;
    int len = (4 * n - 1) * S;                 // длина строки цифр (вдоль экрана снизу вверх)
    int x0 = 64 - 5 * S / 2, yBottom = 32 + len / 2;
    oled.rect(x0 - 4, 32 - len / 2 - 4, x0 + 5 * S + 3, yBottom + 3, OLED_CLEAR);
    oled.rect(x0 - 4, 32 - len / 2 - 4, x0 + 5 * S + 3, yBottom + 3, OLED_STROKE);
    for (int d = 0; d < n; d++)
      for (int gv = 0; gv < 5; gv++)
        for (int gu = 0; gu < 3; gu++)
          if (DIG[b[d] - '0'][gv] & (4 >> gu)) {
            int sx = x0 + gv * S, sy = yBottom - (d * 4 + gu + 1) * S;
            oled.rect(sx, sy, sx + S - 1, sy + S - 1, OLED_FILL);
          }
  }
};

// I, O, T, S, Z, J, L: по 4 поворота, матрица 4x4 в 16 битах (старший бит - левый верхний угол)
const uint16_t Tetris::SHAPES[7][4] = {
  {0x0F00, 0x2222, 0x00F0, 0x4444},
  {0x6600, 0x6600, 0x6600, 0x6600},
  {0x4E00, 0x4640, 0x0E40, 0x4C40},
  {0x6C00, 0x4620, 0x06C0, 0x8C40},
  {0xC600, 0x2640, 0x0C60, 0x4C80},
  {0x8E00, 0x6440, 0x0E20, 0x44C0},
  {0x2E00, 0x4460, 0x0E80, 0xC440},
};

namespace mspr {
const uint8_t TL[9][5] = {{0,0,0,0,0},{0x1F,0x15,0x0A,0x15,0x0A},{0x1E,0x1E,0x00,0x0F,0x0F},{0x1F,0x11,0x15,0x11,0x1F},
  {0x1F,0x11,0x11,0x11,0x1F},{0x1F,0x1F,0x1B,0x1F,0x1F},{0x1D,0x1D,0x1D,0x1D,0x1D},{0x1F,0x1F,0x15,0x1F,0x1F},{0x1F,0x11,0x15,0x11,0x1F}};
const uint8_t MS[5] = {0x0E,0x0F,0x0E,0x1F,0x1B};
const uint8_t MB[10] = {0x0E,0x0F,0x0E,0x0E,0x1F,0x1F,0x0E,0x0E,0x1B,0x1B};
const uint8_t GO[5] = {0x0E,0x1F,0x15,0x0E,0x1B};
const uint8_t SQ[2] = {0x0E,0x1F};
const uint8_t KO[6] = {0x0C,0x0E,0x0C,0x1F,0x1F,0x0A};
const uint8_t SH[3] = {0x0E,0x1F,0x1F};
const uint8_t MU[5] = {0x0E,0x1F,0x15,0x0E,0x0E};
const uint8_t DG[10][5] = {{7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7}};
}

// Super Mario Bros, уровень 1-1. Тайл 5 px (16 px оригинала), физика в тайлах/кадр при 60 Гц.
// ▲ = влево, ▼ = вправо, ОК = прыжок (держать - выше), ОК 1 с = выход
class Mario {
 public:
  int best = 0;
  bool inL = false, inR = false, inA = false;
  void start() { score = 0; coins = 0; lives = 3; load(); }
  void jump() {
    if (st == OVER || st == WIN) { if (millis() - endAt > 700) start(); }
    else jumpReq = 6;
  }
  bool tick(uint32_t now) {
    if (now - last < 33) return false;
    acc += now - last; last = now;
    for (int n = 0; acc >= 16 && n < 4; n++, acc -= 16) step();
    if (acc > 16) acc = 0;
    if (score > best) best = score;
    return true;
  }
  void draw(Oled &o) const {
    using namespace mspr;
    int cam = (int)(camX * T);
    for (int c = cam / T; c <= cam / T + 26 && c < COLS; c++)
      for (int r = 0; r < ROWS; r++) if (map[c][r]) spr(o, c * T - cam, OY + r * T, TL[map[c][r]], 5, false);
    int px = FLAGX * T - cam;
    if (px > -8 && px < 128) {
      o.rect(px, OY + T, px, OY + (G - 1) * T - 1, OLED_FILL);
      int fy = OY + (int)(flagY * T);
      for (int k = 0; k < 5; k++) o.rect(px - 5 + k, fy + k, px - 1, fy + k, OLED_FILL);
    }
    int cx = 202 * T - cam;
    if (cx > -30 && cx < 128) {
      o.rect(cx, OY + 6 * T, cx + 5 * T - 1, OY + G * T - 1, OLED_STROKE);
      o.rect(cx + 2 * T, OY + 8 * T, cx + 3 * T - 1, OY + G * T - 1, OLED_STROKE);
      o.rect(cx, OY + 5 * T + 2, cx + 1, OY + 6 * T, OLED_FILL);
      o.rect(cx + 5 * T - 2, OY + 5 * T + 2, cx + 5 * T - 1, OY + 6 * T, OLED_FILL);
    }
    for (const auto &e : en) if (e.al && e.on) {
      int sx = (int)((e.x - 0.5f) * T) - cam, sy = OY + (int)(e.y * T + 0.5f);
      if (e.type == 0) { if (e.t) spr(o, sx, sy - 2, SQ, 2, false); else spr(o, sx, sy - 5, GO, 5, (frame >> 3) & 1); }
      else if (e.type == 1) spr(o, sx, sy - 6, KO, 6, e.vx > 0);
      else if (e.type == 2) spr(o, sx, sy - 3, SH, 3, false);
      else spr(o, sx, sy - 5, MU, 5, false);
    }
    for (const auto &f : fx) if (f.t) { int a = (int)(f.x * T) - cam, b = OY + (int)(f.y * T); o.rect(a - 1, b - 1, a + 1, b + 1, OLED_STROKE); }
    if (st != WIN && !(inv && (frame & 2))) {
      uint8_t m[10]; int n = big ? 10 : 5;
      memcpy(m, big ? MB : MS, n);
      m[n - 1] = !ground ? 0x11 : (fabsf(vx) > 0.01f && ((int)(x * 6) & 1)) ? 0x0A : 0x1B;
      spr(o, (int)((x - 0.5f) * T) - cam, OY + (int)(y * T + 0.5f) - n, m, n, !face);
    }
    num(o, 0, score, 6);
    o.rect(30, 1, 32, 3, OLED_STROKE); num(o, 36, coins, 2);
    o.dot(60, 1, 1); o.dot(62, 1, 1); o.dot(61, 2, 1); o.dot(60, 3, 1); o.dot(62, 3, 1); num(o, 65, lives, 1);
    num(o, 108, timeLeft, 3);
    if (st == OVER || st == WIN) {
      o.rect(22, 20, 105, 43, OLED_CLEAR); o.rect(22, 20, 105, 43, OLED_STROKE);
      o.setCursor(st == OVER ? 40 : 28, 3); o.print(st == OVER ? "GAME OVER" : "COURSE CLEAR");
      o.setCursor(46, 4); o.print(score);
    }
  }

 private:
  enum { COLS = 212, ROWS = 12, T = 5, OY = 4, G = 10, FLAGX = 198, MAXE = 24 };
  enum St { PLAY, DIE, FLAG, WALK, OVER, WIN };
  static constexpr float HW = 0.375f, VMAX = 0.0977f;
  struct En { float x, y, vx, vy; uint8_t type, t, k; bool al, on; };  // type: 0 гумба, 1 купа, 2 панцирь, 3 гриб
  struct Fx { float x, y; uint8_t t; };
  uint8_t map[COLS][ROWS];
  En en[MAXE];
  Fx fx[4];
  St st = PLAY;
  float x = 3.5f, y = G, vx = 0, vy = 0, camX = 0, flagY = 1.5f;
  int score = 0, coins = 0, lives = 3, timeLeft = 400, jumpReq = 0, dieT = 0;
  uint8_t inv = 0;
  uint32_t frame = 0, last = 0, acc = 0, endAt = 0;
  bool big = false, ground = true, face = true;

  bool sol(int c, int r) const {
    if (c < 0 || c >= COLS) return true;
    if (r < 0 || r >= ROWS) return false;
    return map[c][r] != 0;
  }
  bool hit(float nx, float ny, float h) const {
    int c0 = floorf(nx - HW), c1 = floorf(nx + HW), r0 = floorf(ny - h + 0.02f), r1 = floorf(ny - 0.02f);
    for (int c = c0; c <= c1; c++) for (int r = r0; r <= r1; r++) if (sol(c, r)) return true;
    return false;
  }
  static void spr(Oled &o, int sx, int sy, const uint8_t *rw, int n, bool flip) {
    for (int j = 0; j < n; j++) for (int i = 0; i < 5; i++)
      if (rw[j] & (1 << (flip ? i : 4 - i))) {
        int a = sx + i, b = sy + j;
        if (a >= 0 && a < 128 && b >= 0 && b < 64) o.dot(a, b, 1);
      }
  }
  static void num(Oled &o, int x0, int v, int n) {
    if (v < 0) v = 0;
    for (int i = n - 1; i >= 0; i--, v /= 10)
      for (int gv = 0; gv < 5; gv++) for (int gu = 0; gu < 3; gu++)
        if (mspr::DG[v % 10][gv] & (4 >> gu)) o.dot(x0 + i * 4 + gu, gv, 1);
  }

  void load() {
    memset(map, 0, sizeof map); memset(en, 0, sizeof en); memset(fx, 0, sizeof fx);
    static const uint8_t PIT[] = {69, 70, 86, 87, 88, 153, 154};
    for (int c = 0; c < COLS; c++) {
      bool p = false;
      for (uint8_t q : PIT) p |= (q == c);
      if (!p) map[c][G] = map[c][G + 1] = 1;
    }
    auto B = [&](int c, int r, uint8_t t) { map[c][r] = t; };
    auto stair = [&](int c0, int n, int dir) { for (int i = 0; i < n; i++) for (int k = 1; k <= (dir > 0 ? i + 1 : n - i); k++) map[c0 + i][G - k] = 5; };
    auto pipe = [&](int c, int h) { for (int k = 1; k <= h; k++) map[c][G - k] = map[c + 1][G - k] = (k == h ? 7 : 6); };
    B(16, 6, 3); B(20, 6, 2); B(21, 6, 8); B(22, 6, 2); B(23, 6, 3); B(24, 6, 2); B(22, 2, 3);
    pipe(28, 2); pipe(38, 3); pipe(46, 4); pipe(57, 4);
    B(77, 6, 2); B(78, 6, 8); B(79, 6, 2);
    for (int c = 80; c < 88; c++) B(c, 2, 2);
    for (int c = 91; c < 94; c++) B(c, 2, 2);
    B(94, 2, 3); B(94, 6, 2); B(100, 6, 2); B(101, 6, 2);
    B(106, 6, 3); B(109, 6, 3); B(112, 6, 3); B(109, 2, 8);
    B(118, 6, 2); for (int c = 121; c < 124; c++) B(c, 2, 2);
    B(129, 2, 3); B(130, 2, 3); B(129, 6, 2); B(130, 6, 2);
    stair(134, 4, 1); stair(140, 4, -1); stair(148, 4, 1); stair(155, 4, -1);
    pipe(163, 2); B(168, 6, 2); B(169, 6, 2); B(170, 6, 3); B(171, 6, 2); pipe(179, 2);
    stair(181, 8, 1);
    for (int k = 1; k <= 8; k++) map[189][G - k] = 5;
    map[FLAGX][G - 1] = 5;
    static const uint8_t GM[] = {22, 40, 51, 53, 80, 82, 97, 99, 114, 116, 124, 126, 128, 130, 174, 176};
    int n = 0;
    for (uint8_t c : GM) en[n++] = {c + 0.5f, (float)G, -0.031f, 0, 0, 0, 0, true, false};
    en[n++] = {107.5f, (float)G, -0.031f, 0, 1, 0, 0, true, false};
    x = 3.5f; y = G; vx = vy = 0; camX = 0; big = false; inv = 0; ground = true; face = true;
    st = PLAY; timeLeft = 400; frame = 0; jumpReq = 0; flagY = 1.5f; last = millis(); acc = 0;
  }

  void hurt(bool kill) {
    if (!kill && inv) return;
    if (!kill && big) { big = false; inv = 120; return; }
    st = DIE; dieT = 0; vx = 0; vy = y > ROWS ? 0 : -0.25f;
  }

  void bump(int c, int r) {
    if (c < 0 || c >= COLS || r < 0 || r >= ROWS) return;
    uint8_t t = map[c][r];
    for (auto &e : en) if (e.al && e.on && e.type < 3 && !e.t && (int)floorf(e.x) == c && fabsf(e.y - r) < 0.3f) { e.al = false; score += 100; }
    if (t == 2 && big) { map[c][r] = 0; score += 50; }
    else if (t == 3) {
      map[c][r] = 4; score += 200;
      if (++coins >= 100) { coins -= 100; lives++; }
      for (auto &f : fx) if (!f.t) { f = {c + 0.5f, r - 0.5f, 18}; break; }
    } else if (t == 8) {
      map[c][r] = 4;
      for (auto &e : en) if (!e.al) { e = {c + 0.5f, (float)r, 0.0625f, 0, 3, 0, 0, true, true}; break; }
    }
  }

  void step() {
    frame++;
    for (auto &f : fx) if (f.t) { f.t--; f.y -= 0.1f; }
    if (st == OVER || st == WIN) return;
    if (st == DIE) {
      if (++dieT > 20) { vy += 0.0254f; if (vy > 0.25f) vy = 0.25f; y += vy; }
      if (y > ROWS + 4) { if (lives > 1) { lives--; load(); } else { st = OVER; endAt = millis(); } }
      return;
    }
    if (inv) inv--;
    if (st == PLAY && frame % 24 == 0 && --timeLeft <= 0) { hurt(true); return; }
    moveMario();
    if (st == PLAY) touch();
    enemies();
    float tg = x - 11.0f;
    if (tg > camX) camX = tg;
    if (camX > COLS - 25.6f) camX = COLS - 25.6f;
  }

  void moveMario() {
    float h = big ? 1.9f : 0.9f;
    if (st == FLAG) {
      y += 0.125f; if (flagY < 8.0f) flagY += 0.125f;
      if (y >= G) { y = G; st = WALK; x = FLAGX + 1 + HW; vx = 0; vy = 0; ground = true; }
      return;
    }
    bool R = (inR && st == PLAY) || st == WALK, L = inL && st == PLAY;
    float d = (R && !L) ? 1 : (L && !R) ? -1 : 0;
    vx = d * VMAX;  // без разгона: всегда постоянная скорость
    if (d) face = d > 0;
    if (jumpReq) jumpReq--;
    if (jumpReq && ground && st == PLAY) { vy = -0.25f - fabsf(vx) * 0.2f; jumpReq = 0; ground = false; }
    vy += (vy < 0 && inA) ? 0.00727f : 0.0254f;
    if (vy > 0.25f) vy = 0.25f;
    float nx = x + vx;
    if (hit(nx, y, h)) { nx = vx > 0 ? floorf(nx + HW) - HW - 0.001f : floorf(nx - HW) + 1 + HW + 0.001f; vx = 0; }
    x = nx;
    if (x - HW < camX) { x = camX + HW; if (vx < 0) vx = 0; }
    ground = false;
    float ny = y + vy;
    int cl = floorf(x - HW), cr = floorf(x + HW);
    if (vy >= 0) {
      int ty = floorf(ny);
      if (ty >= 0 && ty < ROWS && (sol(cl, ty) || sol(cr, ty)) && y <= ty + 0.001f) { y = ty; vy = 0; ground = true; }
      else y = ny;
    } else {
      int ty = floorf(ny - h);
      if (sol(cl, ty) || sol(cr, ty)) {
        int tc = sol((int)floorf(x), ty) ? (int)floorf(x) : (sol(cl, ty) ? cl : cr);
        y = ty + 1 + h + 0.001f; vy = 0; bump(tc, ty);
      } else y = ny;
    }
    if (y > ROWS + 0.5f) { hurt(true); return; }
    if (st == PLAY && x + HW >= FLAGX - 0.01f) {  // флагшток
      st = FLAG; vx = 0; vy = 0; x = FLAGX - HW - 0.01f; flagY = 1.5f;
      score += 100 + (int)(G - y) * 500;
    }
    if (st == WALK && x >= 203.5f) { st = WIN; score += timeLeft * 50; timeLeft = 0; endAt = millis(); }
  }

  void touch() {
    float h = big ? 1.9f : 0.9f;
    for (auto &e : en) {
      if (!e.al || !e.on || e.t) continue;
      float eh = e.type == 1 ? 1.2f : e.type == 2 ? 0.6f : 0.9f;
      if (fabsf(e.x - x) > 0.75f || y < e.y - eh + 0.05f || y - h > e.y - 0.05f) continue;
      if (e.type == 3) { e.al = false; score += 1000; big = true; continue; }
      if (vy > 0 && y - (e.y - eh) < 0.6f) {  // прыжок сверху
        if (e.type == 0) { e.t = 25; e.vx = 0; }
        else if (e.type == 1) { e.type = 2; e.vx = 0; }
        else e.vx = e.vx != 0 ? 0 : (x < e.x ? 0.19f : -0.19f);
        e.k = 15; score += 100; vy = inA ? -0.2f : -0.13f;
        if (e.type == 2 && e.vx != 0) inv = 12;
      } else if (e.type == 2 && e.vx == 0) {
        if (!e.k) { e.vx = x < e.x ? 0.19f : -0.19f; inv = 12; score += 400; }
      } else hurt(false);
    }
  }

  void enemies() {
    for (auto &e : en) {
      if (!e.al) continue;
      if (!e.on) { if (e.x - camX < 26.5f) e.on = true; else continue; }
      if (e.t) { if (--e.t == 0) e.al = false; continue; }
      if (e.k) e.k--;
      e.vy += 0.0156f; if (e.vy > 0.25f) e.vy = 0.25f;
      if (e.vx != 0) {
        float nx = e.x + e.vx, dx = e.vx > 0 ? 0.4f : -0.4f;
        if (sol((int)floorf(nx + dx), (int)floorf(e.y - 0.3f))) e.vx = -e.vx; else e.x = nx;
      }
      float ny = e.y + e.vy; int ty = floorf(ny);
      if (e.vy > 0 && ty < ROWS && (sol((int)floorf(e.x - 0.35f), ty) || sol((int)floorf(e.x + 0.35f), ty)) && e.y <= ty + 0.001f) { e.y = ty; e.vy = 0; }
      else e.y = ny;
      if (e.y > ROWS + 1 || e.x < camX - 3) { e.al = false; continue; }
      if (e.type == 2 && e.vx != 0)  // летящий панцирь сбивает врагов
        for (auto &o : en) if (&o != &e && o.al && o.on && !o.t && o.type < 2 && fabsf(o.x - e.x) < 0.8f && fabsf(o.y - e.y) < 0.8f) { o.al = false; score += 200; }
    }
  }
};

// ---- ИИ: разбор JSON-ответа (без внешних библиотек)
namespace aijson {
template <class S> void putUtf8(S &o, uint32_t cp) {
  if (cp < 0x80) o += (char)cp;
  else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
  else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
  else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
}
inline int hex4(const char *p) {
  int v = 0;
  for (int i = 0; i < 4; i++) {
    if (!isxdigit((uint8_t)p[i])) return -1;
    v = v * 16 + (p[i] <= '9' ? p[i] - '0' : (p[i] | 0x20) - 'a' + 10);
  }
  return v;
}
// строковое поле "key":"..." (поиск с позиции from); false, если поля нет или оно не строка
template <class S> bool str(const char *js, const char *key, S &out, const char *from = nullptr) {
  char pat[24];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(from ? from : js, pat);
  if (!p) return false;
  p += strlen(pat);
  while (*p == ' ' || *p == ':') p++;
  if (*p != '"') return false;
  for (p++; *p && *p != '"'; p++) {
    if (*p != '\\') { out += *p; continue; }
    p++;
    switch (*p) {
      case 0: return true;
      case 'n': out += '\n'; break;
      case 't': out += ' '; break;
      case 'r': case 'b': case 'f': break;
      case 'u': {
        int cp = hex4(p + 1);
        if (cp < 0) break;
        p += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u') {  // суррогатная пара
          int lo = hex4(p + 3);
          if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
        }
        putUtf8(out, (uint32_t)cp);
        break;
      }
      default: out += *p;  // \" \\ \/
    }
  }
  return true;
}
}  // namespace aijson

// ---- ИИ: экранная клавиатура (РУС / ENG / 123)
// ▲▼ выбирают строку, ОК заходит в строку; в строке ▲ влево, ▼ вправо, ОК вводит символ и возвращает к выбору строки
namespace akb {
const char *const KR[3][3][12] = {
  {{"й","ц","у","к","е","н","г","ш","щ","з","х",nullptr}, {"ф","ы","в","а","п","р","о","л","д","ж","э",nullptr}, {"я","ч","с","м","и","т","ь","б","ю","ъ","ё",nullptr}},
  {{"q","w","e","r","t","y","u","i","o","p","-",nullptr}, {"a","s","d","f","g","h","j","k","l","'",":",nullptr}, {"z","x","c","v","b","n","m","!","(",")",nullptr}},
  {{"1","2","3","4","5","6","7","8","9","0","%",nullptr}, {".",",","?","!",":",";","-","(",")","\"","'",nullptr}, {"+","=","/","*","@","#","$","&","_","<",">",nullptr}}};
const char *const ML[3] = {"РУС", "ENG", "123"};
const uint8_t I_SHIFT[5] = {0x08, 0x1C, 0x2A, 0x08, 0x08};
const uint8_t I_BKSP[5] = {0x10, 0x30, 0x7F, 0x30, 0x10};
const uint8_t I_ENTER[5] = {0x02, 0x02, 0x22, 0x7E, 0x20};
}  // namespace akb

class AiKeyboard {
 public:
  enum Res { NONE, REDRAW, SEND };
  void open() { mode = 0; row = 0; inKey = false; memset(mem, 0, sizeof mem); build(); }  // режим по умолчанию - русский
  void clear() { text[0] = 0; shift = true; cap = false; }
  const char *query() const { return text; }
  bool tick(uint32_t now) {  // мигание курсора
    bool b = (now / 500) & 1;
    if (b == blink) return false;
    blink = b;
    return true;
  }
  Res handle(bool u, bool d, bool click) {
    if (!inKey) {
      if (u) { row = (row + 3) % 4; return REDRAW; }
      if (d) { row = (row + 1) % 4; return REDRAW; }
      if (click) { inKey = true; col = mem[row] < n[row] ? mem[row] : 0; return REDRAW; }
      return NONE;
    }
    if (u) { col = (col + n[row] - 1) % n[row]; return REDRAW; }
    if (d) { col = (col + 1) % n[row]; return REDRAW; }
    if (!click) return NONE;
    mem[row] = col;
    inKey = false;
    return act(keys[row][col]);
  }
  bool back() {  // отмена выбора клавиши: вернуться к выбору строки (true, если это было нужно)
    if (!inKey) return false;
    mem[row] = col;
    inKey = false;
    return true;
  }
  // Весь текст выводится строго по страницам (кратно 8 px): так буквы не режутся на границах страниц буфера
  void draw(Oled &o) const {
    int len = u8::length(text), tr = len / 21 + 1, r0 = tr > 3 ? tr - 3 : 0;
    char b[96];
    for (int i = 0; i < 3 && r0 + i < tr; i++) {
      u8::copy(b, sizeof b, text, (r0 + i) * 21, 21);
      o.setCursor(0, i);
      o.print(b);
    }
    if (blink) { int cx = (len % 21) * 6, cy = (len / 21 - r0) * 8 + 7; o.rect(cx, cy, cx + 4, cy, OLED_FILL); }
    o.rect(0, 28, 127, 28, OLED_FILL);
    for (int r = 0; r < 4; r++) {
      int ry = 32 + r * 8;  // страницы 4..7, нижний ряд ровно у края экрана
      for (int i = 0; i < n[r]; i++) drawKey(o, keys[r][i], ry, inKey && r == row && i == col);
      if (!inKey && r == row) {  // выбранная строка: маркеры по краям
        o.rect(0, ry, 1, ry + 7, OLED_FILL);
        o.rect(126, ry, 127, ry + 7, OLED_FILL);
      }
    }
  }

 private:
  enum { K_CH, K_MODE, K_SHIFT, K_SPACE, K_BKSP, K_ENTER };
  struct K { uint8_t act; const char *s; uint8_t x, w; };
  K keys[4][12];
  uint8_t n[4] = {0, 0, 0, 0}, mem[4] = {0, 0, 0, 0};
  char text[cfg::AI_Q_BYTES + 1] = {0};
  int mode = 0, row = 0, col = 0;
  bool inKey = false, shift = true, cap = false, blink = false;

  static bool isLetter(const char *s) {
    uint8_t a = (uint8_t)s[0];
    return ((a | 0x20) >= 'a' && (a | 0x20) <= 'z') || a == 0xD0 || a == 0xD1;
  }
  static void upper(char *d, const char *s) {  // строчная буква (латиница/кириллица) -> заглавная
    uint8_t a = (uint8_t)s[0], b = (uint8_t)s[1];
    if (a >= 'a' && a <= 'z') { d[0] = (char)(a - 32); d[1] = 0; }
    else if (a == 0xD0 && b >= 0xB0 && b <= 0xBF) { d[0] = (char)0xD0; d[1] = (char)(b - 0x20); d[2] = 0; }
    else if (a == 0xD1 && b >= 0x80 && b <= 0x8F) { d[0] = (char)0xD0; d[1] = (char)(b + 0x20); d[2] = 0; }
    else if (a == 0xD1 && b == 0x91) { d[0] = (char)0xD0; d[1] = (char)0x81; d[2] = 0; }
    else strlcpy(d, s, 4);
  }
  void build() {
    for (int r = 0; r < 3; r++) {
      n[r] = 0;
      for (int i = 0; akb::KR[mode][r][i]; i++) { keys[r][i] = {K_CH, akb::KR[mode][r][i], (uint8_t)(3 + i * 11), 10}; n[r]++; }
    }
    struct C { uint8_t act; const char *s; uint8_t w; };
    static const C L[] = {{K_MODE, "", 21}, {K_SHIFT, "", 11}, {K_SPACE, "", 22}, {K_CH, ".", 9}, {K_CH, ",", 9}, {K_CH, "?", 9}, {K_BKSP, "", 15}, {K_ENTER, "", 17}};
    static const C S[] = {{K_MODE, "", 21}, {K_SPACE, "", 68}, {K_BKSP, "", 30}};  // в символах кнопки "ввод" нет
    const C *c = mode == 2 ? S : L;
    n[3] = mode == 2 ? 3 : 8;
    int x = 4;
    for (int i = 0; i < n[3]; i++) { keys[3][i] = {c[i].act, c[i].s, (uint8_t)x, c[i].w}; x += c[i].w + 1; }
  }
  void append(const char *s) {
    size_t l = strlen(text), k = strlen(s);
    if (l + k <= (size_t)cfg::AI_Q_BYTES) { memcpy(text + l, s, k + 1); }
  }
  Res act(const K &k) {
    switch (k.act) {
      case K_CH: {
        bool letter = isLetter(k.s);
        char t[4];
        if (shift && letter) upper(t, k.s); else strlcpy(t, k.s, 4);
        append(t);
        if (letter) { shift = false; cap = false; }
        if (k.s[0] == '.' || k.s[0] == '!' || k.s[0] == '?') cap = true;  // после конца предложения - заглавная
        break;
      }
      case K_SPACE: append(" "); if (cap) { shift = true; cap = false; } break;
      case K_SHIFT: shift = !shift; break;
      case K_MODE: mode = (mode + 1) % 3; row = 0; memset(mem, 0, sizeof mem); build(); break;
      case K_BKSP: {
        size_t l = strlen(text);
        if (l) { do l--; while (l && u8::isCont((uint8_t)text[l])); text[l] = 0; }
        if (!text[0]) { shift = true; cap = false; }
        break;
      }
      case K_ENTER:
        for (const char *p = text; *p; p++) if (*p != ' ') return SEND;
        return NONE;
    }
    return REDRAW;
  }
  void drawKey(Oled &o, const K &k, int ry, bool sel) const {
    int x = k.x, w = k.w, ix = x + (w - 7) / 2, col2 = sel ? OLED_CLEAR : OLED_FILL;
    if (sel) {  // плашка со срезанными углами
      o.rect(x + 1, ry, x + w - 2, ry + 7, OLED_FILL);
      o.rect(x, ry + 1, x + w - 1, ry + 6, OLED_FILL);
    } else if (k.act == K_SHIFT && shift) o.rect(x + 1, ry + 7, x + w - 2, ry + 7, OLED_FILL);  // регистр включён - подчёркивание
    const uint8_t *ic = nullptr;
    char t[4];
    const char *s = nullptr;
    switch (k.act) {
      case K_CH: upper(t, k.s); s = t; break;
      case K_MODE: s = akb::ML[mode]; break;
      case K_SHIFT: ic = akb::I_SHIFT; break;
      case K_BKSP: ic = akb::I_BKSP; break;
      case K_ENTER: ic = akb::I_ENTER; break;
      case K_SPACE:
        o.rect(x + 4, ry + 5, x + w - 5, ry + 5, col2);
        o.dot(x + 4, ry + 4, col2); o.dot(x + w - 5, ry + 4, col2);
        break;
    }
    if (ic) for (int r = 0; r < 5; r++) for (int c = 0; c < 7; c++) if (ic[r] & (0x40 >> c)) o.dot(ix + c, ry + 1 + r, col2);
    if (s) {
      if (sel) o.invertText(true);
      o.setCursor(x + (w - 6 * u8::length(s)) / 2, ry / 8);
      o.print(s);
      if (sel) o.invertText(false);
    }
  }
};

// ---- Единое меню (калькулятор: 0000): вкладки ИГРЫ / НЕЙРО / ФУНКЦИИ, иконки, переключатели, подсказка
class Hub {
 public:
  enum Item { FLAPPY, TETRIS, MARIO, AI, FLIP, RGB, RGBPIN, DELETE, COUNT };
  struct Info { bool flip, rgb, pin, aiOk; int bF, bT, bM; };
  void open() { sel = 0; }
  void move(int d) { sel = (sel + d + COUNT) % COUNT; }
  int selected() const { return sel; }
  void draw(Oled &o, const Info &in) const {
    static const char *const TAB[3] = {"ИГРЫ", "НЕЙРО", "ФУНКЦИИ"};
    static const uint8_t TX[3] = {0, 35, 76}, TW[3] = {34, 40, 52}, FIRST[3] = {FLAPPY, AI, FLIP}, CNT[3] = {3, 1, 4};
    static const char *const LBL[COUNT] = {"Flappy Bird", "Tetris", "Super Mario", "Чат с ИИ", "Поворот 180", "RGB подсветка", "Закреплять RGB", "Удалить файлы"};
    static const uint8_t IC[COUNT][7] = {
      {0x1C, 0x3E, 0x7A, 0x7F, 0x3E, 0x1C, 0x00},  // птица
      {0x60, 0x60, 0x7E, 0x1E, 0x1E, 0x06, 0x06},  // фигуры
      {0x1C, 0x3E, 0x6B, 0x7F, 0x1C, 0x1C, 0x1C},  // гриб
      {0x7F, 0x41, 0x55, 0x41, 0x7F, 0x30, 0x20},  // облачко чата
      {0x08, 0x1C, 0x3E, 0x00, 0x3E, 0x1C, 0x08},  // перевернуть
      {0x08, 0x2A, 0x1C, 0x77, 0x1C, 0x2A, 0x08},  // свет
      {0x1C, 0x22, 0x22, 0x7F, 0x77, 0x77, 0x7F},  // замок
      {0x1C, 0x7F, 0x22, 0x2A, 0x2A, 0x22, 0x3E}}; // корзина
    const int sec = sel <= MARIO ? 0 : sel == AI ? 1 : 2;
    for (int t = 0; t < 3; t++) {  // вкладки разделов
      int x = TX[t], w = TW[t], tx = x + (w - 6 * u8::length(TAB[t])) / 2;
      if (t == sec) { o.rect(x + 1, 0, x + w - 2, 7, OLED_FILL); o.rect(x, 1, x + w - 1, 7, OLED_FILL); o.invertText(true); }
      o.setCursor(tx, 0);
      o.print(TAB[t]);
      o.invertText(false);
    }
    o.rect(0, 11, 127, 11, OLED_FILL);
    for (int k = 0; k < CNT[sec]; k++) {  // пункты раздела: страницы 2..5
      int it = FIRST[sec] + k, page = 2 + k, y = page * 8;
      bool s = it == sel;
      int col = s ? OLED_CLEAR : OLED_FILL;
      if (s) { o.rect(1, y, 126, y + 7, OLED_FILL); o.rect(0, y + 1, 127, y + 6, OLED_FILL); o.invertText(true); }
      for (int r = 0; r < 7; r++) for (int c = 0; c < 7; c++) if (IC[it][r] & (0x40 >> c)) o.dot(3 + c, y + r, col);
      o.setCursor(14, page);
      o.print(LBL[it]);
      if (it <= MARIO) {  // рекорд справа
        char b[12];
        snprintf(b, sizeof b, "%d", it == FLAPPY ? in.bF : it == TETRIS ? in.bT : in.bM);
        o.setCursor(124 - 6 * (int)strlen(b), page);
        o.print(b);
      }
      o.invertText(false);
      if (it >= FLIP && it <= RGBPIN) {  // переключатель
        bool on = it == FLIP ? in.flip : it == RGB ? in.rgb : in.pin;
        int x0 = 111, ky = y + 2;
        o.rect(x0 + 1, y, x0 + 11, y, col); o.rect(x0 + 1, y + 6, x0 + 11, y + 6, col);
        o.rect(x0, y + 1, x0, y + 5, col); o.rect(x0 + 12, y + 1, x0 + 12, y + 5, col);
        if (on) { o.rect(x0 + 1, y + 1, x0 + 11, y + 5, col); o.rect(x0 + 8, ky, x0 + 10, ky + 2, s ? OLED_FILL : OLED_CLEAR); }
        else o.rect(x0 + 2, ky, x0 + 4, ky + 2, col);
      }
    }
  }

 private:
  int sel = 0;
};

class App : public WebHooks {
 public:
  App() : web(settings, files, *this) {}

  void begin() {
    Serial.begin(9600);
    randomSeed(micros());
    up.begin(cfg::PIN_UP);
    down.begin(cfg::PIN_DOWN);
    ok.begin(cfg::PIN_OK);
    Wire.begin(cfg::PIN_SDA, cfg::PIN_SCL);
    Wire.setClock(400000);
    oled.init();
    oled.clear();
    oled.update();
    if (!LittleFS.begin(true)) Serial.println("Ошибка LittleFS");
    WiFi.mode(WIFI_OFF);
    web.setup();
    settings.load();
    rgbOn = settings.rgbBoot;
    files.openAt(settings.lastFile);
    oled.setContrast(settings.brightness);
    applyOrientation();
    lastActivity = millis();
    draw();
  }

  void loop() {
    uint32_t now = millis();
    up.update(now);
    down.update(now);
    ok.update(now);

    rgbTick(now);
    if (dark && (up.stable || down.stable || ok.stable)) {  // пробуждение экрана (подсветка работала всё время)
      dark = false; oled.setPower(true); justWoke = true; lastActivity = now; draw();
    }
    if (web.active()) web.service();
    else if (!dark && settings.sleepEnabled && settings.sleepTimeoutMs() > 0 && now - lastActivity >= settings.sleepTimeoutMs()) {
      if (rgbOn) { oled.setPower(false); dark = true; }  // со светодиодом лёгкий сон не используем - он бы замер
      else enterLightSleep();
    }

    handleButtons(now);
    if (state == ST_FLAPPY) {
      lastActivity = now;
      if (flappy.tick(now)) draw();
      if (flappy.best > settings.best) { settings.best = flappy.best; settings.touch(now); }
    }
    if (state == ST_TETRIS) {
      lastActivity = now;
      if (tetris.tick(now)) draw();
      if (tetris.best > settings.tetBest) { settings.tetBest = tetris.best; settings.touch(now); }
    }
    if (state == ST_MARIO) {
      lastActivity = now;
      if (mario.tick(now)) draw();
      if (mario.best > settings.marBest) { settings.marBest = mario.best; settings.touch(now); }
    }
    if (state == ST_AIKB && kb.tick(now)) draw();  // мигающий курсор
    if (state == ST_PROMPT) {
      lastActivity = now;  // во время суфлёра не засыпаем
      if (!promptPaused && now - promptLast >= (uint32_t)settings.promptCs * 10UL) {
        promptLast = now;
        if (!reader.promptAdvance()) exitPrompt(); else draw();
      }
    }
    if ((state == ST_LIST || state == ST_MARK) && files.tickScroll(now)) draw();
    settings.tick(now);
    delay(5);
  }

  // WebHooks
  void filesChanged() override { files.refresh(); }
  void displayChanged() override { applyOrientation(); draw(); }
  void sleepChanged() override { lastActivity = millis(); }

 private:
  enum State { ST_LIST, ST_VIEW, ST_WIFI, ST_CALC, ST_MARK, ST_CONFIRM, ST_FLAPPY, ST_TETRIS, ST_PROMPT, ST_MARIO, ST_AIKB, ST_HUB };

  Oled oled;
  Settings settings;
  FileBrowser files;
  Reader reader;
  Calculator calc;
  FlappyBird flappy;
  Tetris tetris;
  Mario mario;
  AiKeyboard kb;
  Hub hub;
  bool aiAnswer = false, aiSaved = false;  // показан ответ ИИ / он уже сохранён в файлы
  bool backHub = false;  // запуск из меню 0000: выход возвращает в меню
  bool aiView = false;  // после ответа ИИ вернуться к клавиатуре
  WebUI web;
  Button up, down, ok;
  State state = ST_LIST;
  uint32_t lastActivity = 0;
  bool promptPaused = false, promptArm = false;
  uint32_t promptLast = 0;
  uint32_t bothSince = 0;
  bool bothHeld = false;
  bool justWoke = false;
  bool rgbOn = false, dark = false;
  uint32_t rgbLast = 0;
  int rgbHue = 0;

  // плавная радуга на встроенном светодиоде: полный круг ~10 с
  void rgbTick(uint32_t now) {
    if (!rgbOn || now - rgbLast < 20) return;
    rgbLast = now;
    rgbHue = (rgbHue + 3) % 1536;
    int f = rgbHue % 256, u = f, d = 255 - f, r, g, b;
    switch (rgbHue / 256) {
      case 0: r = 255; g = u; b = 0; break;
      case 1: r = d; g = 255; b = 0; break;
      case 2: r = 0; g = 255; b = u; break;
      case 3: r = 0; g = d; b = 255; break;
      case 4: r = u; g = 0; b = 255; break;
      default: r = 255; g = 0; b = d; break;
    }
    neopixelWrite(cfg::PIN_RGB, r * 64 / 255, g * 64 / 255, b * 64 / 255);  // яркость 64/255
  }
  bool bothDone = false;
  int confirmSel = 1;  // 0 = Да, 1 = Нет

  void applyOrientation() { oled.flipH(settings.leftHanded); oled.flipV(settings.leftHanded); }

  void draw() {
    oled.clear();
    switch (state) {
      case ST_LIST: case ST_MARK: files.draw(oled); break;
      case ST_CONFIRM: drawConfirm(); break;
      case ST_FLAPPY: flappy.draw(oled); break;
      case ST_TETRIS: tetris.draw(oled); break;
      case ST_MARIO: mario.draw(oled); break;
      case ST_AIKB: kb.draw(oled); break;
      case ST_HUB: hub.draw(oled, Hub::Info{settings.leftHanded, rgbOn, settings.rgbBoot, settings.aiReady(), settings.best, settings.tetBest, settings.marBest}); break;
      case ST_VIEW: case ST_PROMPT: reader.draw(oled); break;
      case ST_WIFI: drawWifiInfo(); break;
      case ST_CALC: calc.draw(oled); break;
    }
    oled.update();
  }

  void drawConfirm() {
    char b[40];
    oled.setCursor(0, 0); oled.print("Все выделенные только");
    oled.setCursor(0, 1); oled.print("что файлы вы хотите");
    snprintf(b, sizeof b, "удалить? (%d шт.)", files.markCount());
    oled.setCursor(0, 2); oled.print(b);
    oled.setCursor(0, 5); oled.print(confirmSel == 0 ? "> Да" : "  Да");
    oled.setCursor(0, 6); oled.print(confirmSel == 1 ? "> Нет" : "  Нет");
  }

  void deleteMarked() {
    for (int i = 0; i < files.markCount(); i++) {
      const char *p = files.markAt(i);
      if (p[0] != '/' || !strcmp(p, "/")) continue;
      File f = LittleFS.open(p);
      if (!f) continue;
      bool d = f.isDirectory();
      f.close();
      if (d) fsx::removeDirRecursive(p); else LittleFS.remove(p);
    }
    files.fixDir();
    if (settings.lastFile[0] && !LittleFS.exists(settings.lastFile)) { settings.lastFile[0] = 0; settings.touch(millis()); }
  }

  // секретные коды калькулятора (без WiFi)
  State exitTarget() { return backHub ? ST_HUB : web.active() ? ST_WIFI : ST_LIST; }

  void runHub(int id) {
    backHub = true;
    switch (id) {
      case Hub::FLAPPY: flappy.best = settings.best; flappy.reset(); state = ST_FLAPPY; break;
      case Hub::TETRIS: tetris.best = settings.tetBest; tetris.reset(); state = ST_TETRIS; break;
      case Hub::MARIO: mario.best = settings.marBest; mario.start(); state = ST_MARIO; break;
      case Hub::AI:
        if (!settings.aiReady()) showText("ИИ", "ИИ не настроен. Зажмите ОК в списке файлов, подключитесь к Wi-Fi устройства, откройте 192.168.4.1/settings и заполните: имя сети с интернетом, пароль и API-ключ ProxyAPI.", false);
        else { kb.clear(); kb.open(); state = ST_AIKB; }
        break;
      case Hub::FLIP: settings.leftHanded = !settings.leftHanded; applyOrientation(); settings.touch(millis()); break;
      case Hub::RGB: rgbOn = !rgbOn; if (!rgbOn) neopixelWrite(cfg::PIN_RGB, 0, 0, 0); break;
      case Hub::RGBPIN: settings.rgbBoot = !settings.rgbBoot; settings.touch(millis()); if (settings.rgbBoot) rgbOn = true; break;
      case Hub::DELETE: backHub = false; files.beginMark(); state = ST_MARK; break;
    }
  }

  bool secretCode(const char *c) {
    backHub = false;
    if (!strcmp(c, "0000")) {  // единое меню: игры, нейросеть, функции
      calc.reset();
      hub.open();
      state = ST_HUB;
      return true;
    }
    return false;
  }

  void drawWifiInfo() {
    char b[64];
    snprintf(b, sizeof b, "WiFi: %s", settings.ssid);
    oled.setCursor(0, 0); oled.print(b);
    const IPAddress &ip = web.ip();
    snprintf(b, sizeof b, "IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    oled.setCursor(0, 2); oled.print(b);
    snprintf(b, sizeof b, "Pass: %s", settings.pass[0] ? settings.pass : "None");
    oled.setCursor(0, 4); oled.print(b);
    snprintf(b, sizeof b, "Яркость: %d", settings.brightness);
    oled.setCursor(0, 6); oled.print(b);
  }

  void enterLightSleep() {
    oled.setPower(false);
    gpio_wakeup_enable((gpio_num_t)cfg::PIN_UP, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)cfg::PIN_DOWN, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)cfg::PIN_OK, GPIO_INTR_LOW_LEVEL); 
    esp_sleep_enable_gpio_wakeup(); 
    esp_light_sleep_start();
    justWoke = true;  
    lastActivity = millis();
    oled.setPower(true);
    draw();
  }

  void rememberSelection() {
    const Entry *e = files.current();
    strlcpy(settings.lastFile, (e && !e->isDir) ? e->path : "", sizeof settings.lastFile);
    settings.touch(millis());
  }

  void handleButtons(uint32_t now) {
    bool anyHeld = up.stable || down.stable || ok.stable;
    bool anyPressed = up.pressed || down.pressed || ok.pressed;
    if (justWoke) { if (!anyHeld) justWoke = false; return; }
    if (state == ST_TETRIS) tetris.fast = up.stable && down.stable;
    if (anyHeld || anyPressed || up.repeat || down.repeat) lastActivity = now;

    if (state == ST_FLAPPY || state == ST_TETRIS || state == ST_MARIO) {  // во всех играх: все кнопки вместе 1 с - выход
      static uint32_t allSince = 0;
      bool all = up.stable && down.stable && ok.stable;
      if (!all) allSince = 0; else if (!allSince) allSince = now;
      if (all && now - allSince >= 1000) {
        mario.inL = mario.inR = mario.inA = false;
        tetris.fast = false;
        bothHeld = false;
        justWoke = true;  // дождаться отпускания кнопок, чтобы не открылся калькулятор
        state = exitTarget(); draw();
        return;
      }
    }
    if (state == ST_MARIO) {  // ▲ влево, ▼ вправо, ОК прыжок
      Button &bl = settings.leftHanded ? down : up, &br = settings.leftHanded ? up : down;
      mario.inL = bl.stable; mario.inR = br.stable; mario.inA = ok.stable;
      if (ok.pressed) mario.jump();
      return;
    }
    if (state == ST_HUB) {  // меню 0000: ▲▼ выбор, ОК действие, зажать ОК - выход
      Button &nu = settings.leftHanded ? down : up, &nd = settings.leftHanded ? up : down;
      if (ok.longPress) { state = web.active() ? ST_WIFI : ST_LIST; draw(); return; }
      if (nu.pressed || nu.repeat) { hub.move(-1); draw(); }
      if (nd.pressed || nd.repeat) { hub.move(1); draw(); }
      if (ok.released && !ok.longFired) { runHub(hub.selected()); draw(); }
      return;
    }
    if (state == ST_AIKB) {  // клавиатура ИИ: ▲▼ выбор, ОК ввод, зажать ОК - выход
      Button &nu = settings.leftHanded ? down : up, &nd = settings.leftHanded ? up : down;
      if (ok.longPress) {  // зажать ОК: на клавише - отмена выбора (назад к строкам), на выборе строки - выход из ИИ
        if (kb.back()) draw();
        else { state = exitTarget(); draw(); }
        return;
      }
      AiKeyboard::Res r = kb.handle(nu.pressed || nu.repeat, nd.pressed || nd.repeat, ok.released && !ok.longFired);
      if (r == AiKeyboard::SEND) { aiAsk(kb.query()); lastActivity = millis(); draw(); }
      else if (r == AiKeyboard::REDRAW) draw();
      return;
    }
    if (up.stable && down.stable) {  // обе кнопки дольше 0.4 с - калькулятор
      if (state == ST_VIEW && reader.bar && aiAnswer) {  // ответ ИИ: обе кнопки ~0.6 с - сохранить в файлы
        if (!bothHeld) { bothHeld = true; bothDone = false; bothSince = now; }
        else if (!bothDone && now - bothSince > 600) {
          bothDone = true;
          char t[40];
          if (aiSaved) drawStatus("Уже сохранено", "в файлах", 1);
          else if (saveAi(t, sizeof t)) { aiSaved = true; drawStatus("Сохранено", t, 1); }
          else drawStatus("Не удалось", "нет места", 1);
          delay(900);
          draw();
        }
        return;
      }
      if (state == ST_TETRIS) {  // в тетрисе обе кнопки = ускорение падения в 3 раза
        if (!bothHeld) { bothHeld = true; tetris.chordStart(now); }
        return;
      }
      if (!bothHeld) { bothHeld = true; bothDone = false; bothSince = now; }
      else if (!bothDone && now - bothSince > 400) {
        bothDone = true;
        if (state == ST_MARK || state == ST_CONFIRM) { files.endMark(); state = ST_LIST; draw(); }  // выход из режима удаления
        else if (state != ST_CALC && state != ST_FLAPPY && state != ST_TETRIS && state != ST_PROMPT) { calc.reset(); state = ST_CALC; draw(); }
      }
      return;
    }
    bothHeld = false;

    Button &navUp = settings.leftHanded ? down : up;
    Button &navDown = settings.leftHanded ? up : down;
    bool upStep = navUp.pressed || navUp.repeat, downStep = navDown.pressed || navDown.repeat;

    switch (state) {
      case ST_LIST:
        if (upStep) { files.move(-1); rememberSelection(); draw(); }
        if (downStep) { files.move(1); rememberSelection(); draw(); }
        if (ok.released && !ok.longFired) openSelected();
        if (ok.longPress) toggleWifi();
        break;
      case ST_VIEW:
        if (navUp.pressed) promptArm = !reader.bar && reader.canPrompt();  // нажатие НАЧАЛОСЬ на первой странице?
        if (navUp.longPress && promptArm) {  // ▲ зажата на первой странице — запуск суфлёра
          promptArm = false;
          reader.promptStart();
          state = ST_PROMPT;
          promptPaused = false;
          promptLast = now;
          draw();
          break;
        }
        if (upStep && reader.prev()) draw();
        if (downStep && reader.next()) draw();
        if (reader.bar && ok.longPress) { state = exitTarget(); draw(); break; }  // ИИ: зажать ОК - выход
        if (ok.released && !ok.longFired) { state = reader.bar ? (aiView ? ST_AIKB : exitTarget()) : ST_LIST; draw(); }
        break;
      case ST_PROMPT:
        if (navUp.pressed) { promptPaused = !promptPaused; promptLast = now; }  // пауза / продолжить
        if (downStep) {  // ▼ — вручную на строку вниз (авто-прокрутка встаёт на паузу)
          promptPaused = true;
          if (!reader.promptAdvance()) exitPrompt(); else draw();
        }
        if (ok.released && !ok.longFired) exitPrompt();
        break;
      case ST_WIFI:
        if (upStep) changeBrightness(15);
        if (downStep) changeBrightness(-15);
        if (ok.longPress) toggleWifi();
        break;
      case ST_FLAPPY:
        if (navUp.pressed) { flappy.flap(); draw(); }
        if (navDown.pressed) { flappy.pause(); draw(); }
        break;
      case ST_TETRIS:
        if (up.pressed || up.repeat) { tetris.move(1, now); draw(); }     // фигура вверх по экрану
        if (down.pressed || down.repeat) { tetris.move(-1, now); draw(); }  // вниз по экрану
        if (ok.released && !ok.longFired) { tetris.rotate(); draw(); }  // быстрое нажатие - поворот
        break;
      case ST_MARK:
        if (upStep) { files.move(-1); draw(); }
        if (downStep) { files.move(1); draw(); }
        if (ok.released && !ok.longFired) { files.toggleMark(); draw(); }  // выделить/снять
        if (ok.longPress) {
          const Entry *e = files.current();
          if (e) {
            if (e->isDir) files.enterSelected();  // зайти в папку (или ".." назад)
            else {
              if (!files.markCount()) files.toggleMark();
              confirmSel = 1;
              state = ST_CONFIRM;
            }
            draw();
          }
        }
        break;
      case ST_CONFIRM:
        if (upStep || downStep) { confirmSel ^= 1; draw(); }
        if (ok.released && !ok.longFired) {
          if (confirmSel == 0) deleteMarked();
          files.endMark();
          state = ST_LIST;
          rememberSelection();
          draw();
        }
        break;
      case ST_CALC:
        if (upStep) { calc.moveSel(-1); draw(); }
        if (downStep) { calc.moveSel(1); draw(); }
        if (ok.released && !ok.longFired) {
          if (!(!strcmp(calc.selectedKey(), "=") && secretCode(calc.text()))) calc.pressSelected();
          draw();
        }
        if (ok.longPress) { state = web.active() ? ST_WIFI : ST_LIST; draw(); }
        break;
    }
  }

  void exitPrompt() {
    reader.promptStop();
    state = ST_VIEW;  // страница та же, что была до суфлёра
    draw();
  }

  // ---- ИИ: ProxyAPI (OpenAI-совместимый /chat/completions)
  void drawStatus(const char *a, const char *b, int step) {
    oled.clear();
    oled.rect(6, 12, 121, 51, OLED_STROKE);
    oled.setCursor((128 - u8::length(a) * 6) / 2, 2); oled.print(a);
    oled.setCursor((128 - u8::length(b) * 6) / 2, 3); oled.print(b);
    for (int i = 0; i < 3; i++) oled.rect(49 + i * 10, 38, 52 + i * 10, 41, i == step % 3 ? OLED_FILL : OLED_STROKE);
    oled.update();
  }

  // ответ/ошибка/уведомление: пишем во временный файл и показываем читалкой (листание страниц как в шпаргалках)
  void showText(const char *title, const String &body, bool backToKb) {
    aiAnswer = false;
    File f = LittleFS.open(cfg::AI_PATH, "w");
    if (f) { f.print(title); f.print('\n'); f.print(body); f.close(); }
    reader.openText(cfg::AI_PATH);
    reader.bar = true;  // полоса прокрутки справа
    aiView = backToKb;
    state = ST_VIEW;
  }

  // сохранить ответ ИИ в файлы: "Ответ от ИИ", затем "Ответ от ИИ (1)", "(2)" ...
  bool saveAi(char *title, size_t cap) {
    File in = LittleFS.open(cfg::AI_PATH, "r");
    if (!in) return false;
    in.readStringUntil('\n');  // пропустить служебный заголовок
    if (!fsx::hasSpace((long)in.size() + 80)) { in.close(); return false; }
    static const char P[] = "Ответ от ИИ (";
    bool base = false;
    int maxN = 0;
    unsigned long maxId = 0;
    fsx::scanDir("/", [&](const char *p, File &f) {
      if (f.isDirectory()) return;
      const char *name = str::baseName(p);
      if (!strncmp(name, "d_", 2)) { unsigned long id = strtoul(name + 2, nullptr, 10); if (id > maxId) maxId = id; }
      char t[40];
      int n = f.readBytesUntil('\n', t, sizeof t - 1);
      t[n > 0 ? n : 0] = 0;
      if (n > 0 && t[n - 1] == '\r') t[n - 1] = 0;
      if (!strcmp(t, "Ответ от ИИ")) base = true;
      else if (!strncmp(t, P, sizeof P - 1)) { int k = atoi(t + sizeof P - 1); if (k > maxN) maxN = k; }
    });
    if (!base) strlcpy(title, "Ответ от ИИ", cap);
    else snprintf(title, cap, "Ответ от ИИ (%d)", maxN + 1);
    char path[32];
    snprintf(path, sizeof path, "/d_%lu.txt", maxId + 1);
    File out = LittleFS.open(path, "w");
    if (!out) { in.close(); return false; }
    out.print(title);
    out.print('\n');
    uint8_t buf[128];
    int n;
    while ((n = in.read(buf, sizeof buf)) > 0) out.write(buf, n);
    out.close();
    in.close();
    files.refresh();
    return true;
  }

  void aiDisconnect(bool apWas) {
    if (apWas) WiFi.disconnect(false);  // точка доступа настроек продолжает работать
    else { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); }
  }

  void aiAsk(const char *q) {
    const bool apWas = web.active();
    drawStatus("Подключение", "к Wi-Fi", 0);
    WiFi.mode(apWas ? WIFI_AP_STA : WIFI_STA);
    WiFi.begin(settings.staSsid, settings.staPass[0] ? settings.staPass : nullptr);
    uint32_t t0 = millis(), tDot = t0;
    int step = 0;
    bool cancel = false;
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
      delay(50);
      rgbTick(millis());
      if (millis() - tDot >= 300) { tDot = millis(); drawStatus("Подключение", "к Wi-Fi", ++step); }
      if (!digitalRead(cfg::PIN_UP) || !digitalRead(cfg::PIN_DOWN) || !digitalRead(cfg::PIN_OK)) { cancel = true; break; }  // любая кнопка - отмена
    }
    if (cancel || WiFi.status() != WL_CONNECTED) {
      aiDisconnect(apWas);
      justWoke = true;  // дождаться отпускания кнопок
      if (!cancel) showText("Ошибка ИИ", String("Не удалось подключиться к сети \"") + settings.staSsid + "\". Проверьте имя и пароль в веб-настройках.", true);
      return;
    }
    drawStatus("Запрос к ИИ", "ждите ответ", 1);
    String body;
    body.reserve(strlen(q) + 400);
    // ProxyAPI принимает OpenAI Chat Completions на /v1/chat/completions.
    // Для короткого имени явно добавляем вендора OpenAI.
    String modelId = settings.model;
    if (modelId.indexOf('/') < 0) modelId = String("openai/") + modelId;
    body += "{\"model\":\""; body += modelId;
    body += "\",\"max_tokens\":600,\"messages\":[{\"role\":\"system\",\"content\":\"Ты помощник на маленьком OLED-экране. Отвечай кратко и по делу на языке вопроса, простым текстом: без markdown, без звёздочек, без эмодзи.\"},{\"role\":\"user\",\"content\":\"";
    for (const char *p = q; *p; p++) {
      const uint8_t c = (uint8_t)*p;
      if (*p == '"' || *p == '\\') { body += '\\'; body += *p; }
      else if (c == '\n') body += "\\n";
      else if (c == '\r') body += "\\r";
      else if (c == '\t') body += "\\t";
      else if (c >= 0x20) body += *p;
      else body += ' ';
    }
    body += "\"}]}";
    WiFiClientSecure cli;
    cli.setInsecure();  // без проверки сертификата (экономит память и не требует хранить корневые сертификаты)
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(30000);
    String resp;
    int code = -1;
    if (http.begin(cli, "https://api.proxyapi.ru/v1/chat/completions")) {
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Authorization", String("Bearer ") + settings.apiKey);
      code = http.POST(body);
      if (code > 0) resp = http.getString();
      http.end();
    }
    aiDisconnect(apWas);
    String ans;
    if (code == 200 && aijson::str(resp.c_str(), "content", ans, strstr(resp.c_str(), "\"message\"")) && ans.length()) {
      kb.clear();
      showText("Ответ ИИ", ans, true);
      aiAnswer = true; aiSaved = false;
      return;
    }
    String e, m;
    aijson::str(resp.c_str(), "message", e);
    if (code <= 0) m = "Нет связи с сервером ProxyAPI. Проверьте интернет (код " + String(code) + ").";
    else if (code == 401) m = "Неверный API-ключ (401).";
    else if (code == 402) m = "Недостаточно средств на балансе ProxyAPI (402).";
    else if (code == 429) m = "Слишком много запросов (429), подождите немного.";
    else if (code == 200) m = "Пустой ответ от ИИ.";
    else m = "Ошибка сервера, код " + String(code) + ".";
    if (e.length()) { m += "\n"; m += e; }
    // ProxyAPI возвращает подробности ошибки в JSON; покажем тело ответа,
    // если поле message не удалось извлечь нашим компактным JSON-парсером.
    if (code >= 400 && !resp.isEmpty() && e.isEmpty()) {
      m += "\nОтвет API: ";
      m += resp.substring(0, 220);
    }
    showText("Ошибка ИИ", m, true);
  }

  void openSelected() {
    const Entry *e = files.current();
    if (!e) return;
    if (e->isDir) { files.enterSelected(); draw(); return; }
    if (e->isImage) reader.openImage(e->path);
    else
      reader.openText(e->path);
    state = ST_VIEW;
    rememberSelection();
    draw();
  }

  void changeBrightness(int delta) {
    settings.brightness = (uint8_t)constrain((int)settings.brightness + delta, 5, 255);
    oled.setContrast(settings.brightness);
    settings.touch(millis());
    draw();
  }

  void toggleWifi() {
    if (!web.active()) { web.start(); state = ST_WIFI; }
    else { web.stop(); state = ST_LIST; files.refresh(); }
    lastActivity = millis();
    draw();
  }
};

static App app;  

void setup() { app.begin(); }
void loop() { app.loop(); }