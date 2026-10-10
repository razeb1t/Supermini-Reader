#include <Arduino.h>
#include <Wire.h>
#include <GyverOLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <algorithm>
#include <esp_sleep.h>
#include <math.h>

namespace cfg {
constexpr uint8_t PIN_SDA = 11, PIN_SCL = 13, PIN_UP = 5, PIN_DOWN = 7, PIN_OK = 6;
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
    if (!strstr(p, cfg::SETTINGS_PATH + 1)) fn(p, f);
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
  char ssid[33];
  char pass[65];
  char lastFile[cfg::PATH_LEN];

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
    char l[10][cfg::PATH_LEN];
    int got = 0;
    while (got < 10 && f.available()) {
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
    char line[32];
    if (result[0]) snprintf(line, sizeof line, "=%s", result);
    else strlcpy(line, disp, sizeof line);
    int len = u8::length(line);
    if (len > 22) { char t[32]; u8::copy(t, sizeof t, line, len - 21, 21); strlcpy(line, t, sizeof line); }
    oled.setCursor(0, 0);
    oled.print(line);
    oled.line(0, 20, 127, 20);

    const int BTN_W = 32, CHAR_W = 6, CHAR_H = 8, BTN_CHARS = 5, FIRST_ROW = 3;
    for (int row = 0; row < ROWS; row++) {
      for (int col = 0; col < COLS; col++) {
        const char *label = KEYS[row][col];
        int len2 = (int)strlen(label);
        if (len2 > BTN_CHARS) len2 = BTN_CHARS;
        char buf[BTN_CHARS + 1];
        memset(buf, ' ', BTN_CHARS);
        buf[BTN_CHARS] = 0;
        memcpy(buf + (BTN_CHARS - len2) / 2, label, (size_t)len2);

        int colX = col * BTN_W, textX = colX + (BTN_W - BTN_CHARS * CHAR_W) / 2;
        int lineY = FIRST_ROW + row, btnY = lineY * CHAR_H;
        bool isSel = (row * COLS + col) == sel;
        if (isSel) {
          oled.rect(colX + 1, btnY - 1, colX + BTN_W - 2, btnY + CHAR_H - 1, OLED_FILL);
          oled.invertText(true);
        }
        oled.setCursor(textX, lineY);
        oled.print(buf);
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
      if (wchars == 0) { have = false; return true; }  // пустая строка в тексте
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
    return true;
  }
};

class Reader {
 public:
  void openText(const char *path) {
    strlcpy(file, path, sizeof file);
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
  void draw(Oled &oled) {
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
    f.close();
    if (rows == 0 && page == 0) msg(oled, "Пусто");
  }

 private:
  char file[cfg::PATH_LEN] = "";
  bool isImage = false;
  int page = 0;
  bool hasNext = false;
  uint32_t nextOff = 0;
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
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
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
    on("/upload", HTTP_POST, &WebUI::doUpload);
    on("/mkdir", HTTP_POST, &WebUI::doMkdir);
    on("/rmdir", HTTP_POST, &WebUI::doRmdir);
    on("/save", HTTP_POST, &WebUI::doSave);
    on("/delete", HTTP_POST, &WebUI::doDelete);
    on("/save_wifi", HTTP_POST, &WebUI::doSaveWifi);
    on("/save_lefty", HTTP_POST, &WebUI::doSaveLefty);
    on("/save_sleep", HTTP_POST, &WebUI::doSaveSleep);
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
      {"wifierr", true, "Ошибка: имя сети 1-32 символа, пароль пустой или 8-63 символа!"}};
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

    o.raw("<div class='card'><h3>Добавить шпаргалку</h3><form method='POST' action='/upload'>");
    hidden(o, "dir", dir);
    o.raw("<input type='text' name='title' placeholder='Заголовок шпаргалки' required>"
          "<textarea name='content' placeholder='Содержимое файла'></textarea>"
          "<button type='submit'>Загрузить файл</button></form></div>");
    o.raw("<div class='card'><h3>Загрузить картинку (128x64)</h3><form method='POST' action='/upload_img'>");
    hidden(o, "dir", dir);
    o.raw("<input type='text' name='imgname' placeholder='Имя файла (без .h)' required>"
          "<textarea name='imgcode' placeholder='Вставьте байты массива: 0x3C, 0x00, ... (ровно 1024 байта)'></textarea>"
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
    o.raw("<h2>Изменить шпаргалку</h2><form method='POST' action='/save'>");
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

  void doUpload() {
    char dir[DIR_BUF];
    argDir(dir);
    String title = server.arg("title");
    title.trim();
    String content = server.arg("content");
    if (!title.length()) { redirect(dir, "empty"); return; }
    if (!fsx::hasSpace((long)(title.length() + content.length() + 8))) { redirect(dir, "nospace"); return; }
    char path[cfg::PATH_LEN * 2];
    newFileName(dir, path, sizeof path);
    if (strlen(path) >= (size_t)cfg::PATH_LEN) { redirect(dir, "toolong"); return; }
    writeText(path, title.c_str(), content);
    hooks.filesChanged();
    redirect(dir, "ok");
  }

  void doSave() {
    String fn = server.arg("f"), title = server.arg("title"), content = server.arg("content");
    char dir[DIR_BUF];
    argDir(dir);
    title.trim();
    if (!title.length() || !str::safePath(fn.c_str()) || !LittleFS.exists(fn.c_str())) { redirect(dir, "empty"); return; }
    size_t oldSize = 0;
    File f = LittleFS.open(fn.c_str(), "r");
    if (f) { oldSize = f.size(); f.close(); }
    long delta = (long)(title.length() + content.length() + 2) - (long)oldSize;
    if (delta > 0 && !fsx::hasSpace(delta)) { redirect(dir, "nospace"); return; }
    writeText(fn.c_str(), title.c_str(), content);
    hooks.filesChanged();
    redirect(dir, "ok");
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
  static void writeBin(const char *path, const uint8_t *data, size_t len) {
    File f = LittleFS.open(path, "w");
    if (!f) return;
    f.write(data, len);
    f.close();
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
    if (strlen(path) >= (size_t)cfg::PATH_LEN) { redirect(dir, "toolong"); return; }
    writeBin(path, imgA, cfg::IMG_BYTES);
    hooks.filesChanged();
    redirect(dir, "ok");
  }

  void doSaveImg() {
    String fn = server.arg("f"), code = server.arg("imgcode");
    char dir[DIR_BUF];
    argDir(dir);
    if (!str::safePath(fn.c_str()) || !LittleFS.exists(fn.c_str())) { redirect(dir, "empty"); return; }
    if (!parseImage(code)) { redirect(dir, "imgsize"); return; }
    writeBin(fn.c_str(), imgA, cfg::IMG_BYTES);
    hooks.filesChanged();
    redirect(dir, "ok");
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
    o.raw("'></div></div><button type='submit'>Сохранить</button></form></div></body></html>");
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
  void doSaveTheme() {
    if (server.hasArg("theme")) {
      settings.webTheme = constrain((int)server.arg("theme").toInt(), 0, 8);
      settings.touch(millis());
    }
    redirectSettings("saved");
  }
};

class Game {
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

    if (web.active()) web.service();
    else if (settings.sleepEnabled && settings.sleepTimeoutMs() > 0 && now - lastActivity >= settings.sleepTimeoutMs()) enterLightSleep();

    handleButtons(now);
    if (state == ST_GAME) {
      lastActivity = now;
      if (game.tick(now)) draw();
      if (game.best > settings.best) { settings.best = game.best; settings.touch(now); }
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
  enum State { ST_LIST, ST_VIEW, ST_WIFI, ST_CALC, ST_MARK, ST_CONFIRM, ST_GAME };

  Oled oled;
  Settings settings;
  FileBrowser files;
  Reader reader;
  Calculator calc;
  Game game;
  WebUI web;
  Button up, down, ok;
  State state = ST_LIST;
  uint32_t lastActivity = 0;
  uint32_t bothSince = 0;
  bool bothHeld = false;
  bool justWoke = false;
  bool bothDone = false;
  int confirmSel = 1;  // 0 = Да, 1 = Нет

  void applyOrientation() { oled.flipH(settings.leftHanded); oled.flipV(settings.leftHanded); }

  void draw() {
    oled.clear();
    switch (state) {
      case ST_LIST: case ST_MARK: files.draw(oled); break;
      case ST_CONFIRM: drawConfirm(); break;
      case ST_GAME: game.draw(oled); break;
      case ST_VIEW: reader.draw(oled); break;
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
  bool secretCode(const char *c) {
    if (!strcmp(c, "1111")) {  // разворот экрана на 180°
      settings.leftHanded = !settings.leftHanded;
      applyOrientation();
      settings.touch(millis());
      calc.reset();
      return true;
    }
    if (!strcmp(c, "0000")) {  // Flappy Bird
      calc.reset();
      game.best = settings.best;
      game.reset();
      state = ST_GAME;
      return true;
    }
    if (!strcmp(c, "2222")) {  // режим удаления
      calc.reset();
      files.beginMark();
      state = ST_MARK;
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
    if (anyHeld || anyPressed || up.repeat || down.repeat) lastActivity = now;

    if (up.stable && down.stable) {  // обе кнопки дольше 0.4 с - калькулятор
      if (!bothHeld) { bothHeld = true; bothDone = false; bothSince = now; }
      else if (!bothDone && now - bothSince > 400) {
        bothDone = true;
        if (state == ST_MARK || state == ST_CONFIRM) { files.endMark(); state = ST_LIST; draw(); }  // выход из режима удаления
        else if (state != ST_CALC && state != ST_GAME) { calc.reset(); state = ST_CALC; draw(); }
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
        if (upStep && reader.prev()) draw();
        if (downStep && reader.next()) draw();
        if (ok.released && !ok.longFired) { state = ST_LIST; draw(); }
        break;
      case ST_WIFI:
        if (upStep) changeBrightness(15);
        if (downStep) changeBrightness(-15);
        if (ok.longPress) toggleWifi();
        break;
      case ST_GAME:
        if (navUp.pressed) { game.flap(); draw(); }
        if (navDown.pressed) { game.pause(); draw(); }
        if (ok.released && !ok.longFired) { state = web.active() ? ST_WIFI : ST_LIST; draw(); }
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