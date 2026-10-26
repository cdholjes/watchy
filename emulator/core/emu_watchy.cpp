// Emulated Watchy library + the emulator's command loop.
//
// The face's own setup() runs once per wake-up, exactly like on the watch, where every
// minute tick / button press boots the chip from deep sleep. Commands arrive on stdin
// one per line and each reply is one JSON line on stdout.
#include <Watchy.h>
#include <stdio.h>
#include <iostream>
#include <sstream>
#include "emu.h"

void setup(); // from the watch face's .ino

EmuState emu;

EmuRTC Watchy::RTC;
EmuDisplay Watchy::display;
EmuWiFi WiFi;

RTC_DATA_ATTR int guiState;
RTC_DATA_ATTR int menuIndex;
RTC_DATA_ATTR BMA423 sensor;
RTC_DATA_ATTR bool WIFI_CONFIGURED;
RTC_DATA_ATTR bool BLE_CONFIGURED;
RTC_DATA_ATTR bool USB_PLUGGED_IN = false;
RTC_DATA_ATTR long gmtOffset = 0;
RTC_DATA_ATTR bool alreadyInMenu = true;

// ---------------- simulated hardware ----------------

void emuLog(const std::string &line) { emu.log.push_back(line); }
double emuWorldNow() { return emu.worldEpochAtZero + emu.ms / 1000.0; }
double emuRtcNow() { return emu.rtcEpochAtZero + emu.ms / 1000.0; }
void emuSetRtc(double epoch) { emu.rtcEpochAtZero = epoch - emu.ms / 1000.0; }

esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause() { return (esp_sleep_wakeup_cause_t)emu.wakeCause; }
uint64_t esp_sleep_get_ext1_wakeup_status() { return emu.wakeButtons; }
void btStop() {}
bool emuLiveWeather() { return emu.liveWeather && emu.wifiAvailable; }

uint32_t BMA423::getCounter() { return emu.steps; }

bool BMA423::getAccel(Accel &acc) {
    if (!emu.accelFile.empty()) {
        if (FILE *f = fopen(emu.accelFile.c_str(), "r")) {
            int x, y, z;
            if (fscanf(f, "%d %d %d", &x, &y, &z) == 3) {
                emu.accel[0] = x; emu.accel[1] = y; emu.accel[2] = z;
            }
            fclose(f);
        }
    }
    acc.x = emu.accel[0];
    acc.y = emu.accel[1];
    acc.z = emu.accel[2];
    return true;
}
bool BMA423::resetStepCounter() { emu.steps = 0; emuLog("step counter reset"); return true; }

void EmuDisplay::drawPixel(int16_t x, int16_t y, uint16_t color) {
    if (x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT) return;
    uint16_t i = y * (WIDTH / 8) + x / 8;
    uint8_t bit = 0x80 >> (x & 7);
    if (color != GxEPD_BLACK) buffer[i] |= bit;
    else buffer[i] &= ~bit;
}

void EmuDisplay::display(bool partial) {
    EmuFrame f;
    f.t = (uint32_t)(emu.ms - emu.wakeStartMs);
    f.partial = partial;
    f.bits.assign(buffer, buffer + sizeof(buffer));
    if (emu.realtime) emuStreamFrame(f);
    else emu.frames.push_back(std::move(f));
    delay(partial ? EMU_PARTIAL_REFRESH_MS : EMU_FULL_REFRESH_MS);
}

void EmuRTC::config(String datetime) {
    // the real ESP32-S3 RTC is wiped on boot; with an empty string it ends up near the epoch
    struct tm t = {};
    if (strptime(datetime.c_str(), "%Y:%m:%d:%H:%M:%S", &t)) {
        tmElements_t tm;
        tm.Year = CalendarYrToTm(t.tm_year + 1900);
        tm.Month = t.tm_mon + 1;
        tm.Day = t.tm_mday;
        tm.Hour = t.tm_hour;
        tm.Minute = t.tm_min;
        tm.Second = t.tm_sec;
        emuSetRtc(makeTime(tm));
    } else {
        emuSetRtc(0);
    }
    emuLog("RTC reset on boot");
}

void EmuRTC::read(tmElements_t &tm) { breakTime((time_t)emuRtcNow(), tm); }
void EmuRTC::set(tmElements_t tm) { emuSetRtc(makeTime(tm)); }

// ---------------- Watchy library (ported from Watchy.cpp, minus radio/sensor plumbing) ----------------

void Watchy::init(String datetime) {
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    RTC.init();
    display.epd2.initWatchy();

    switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_TIMER:
        RTC.read(currentTime);
        switch (guiState) {
        case WATCHFACE_STATE:
            showWatchFace(true);
            if (settings.vibrateOClock && currentTime.Minute == 0) {
                vibMotor(75, 4);
            }
            break;
        case MAIN_MENU_STATE:
            if (alreadyInMenu) {
                guiState = WATCHFACE_STATE;
                showWatchFace(false);
            } else {
                alreadyInMenu = true;
            }
            break;
        }
        break;
    case ESP_SLEEP_WAKEUP_EXT1:
        handleButtonPress();
        break;
    case ESP_SLEEP_WAKEUP_EXT0:
        USB_PLUGGED_IN = emu.usbPlugged;
        if (guiState == WATCHFACE_STATE) {
            RTC.read(currentTime);
            showWatchFace(true);
        }
        break;
    default:
        RTC.config(datetime);
        USB_PLUGGED_IN = emu.usbPlugged;
        gmtOffset = settings.gmtOffset;
        RTC.read(currentTime);
        showWatchFace(false);
        vibMotor(75, 4);
        break;
    }
    deepSleep();
}

void Watchy::deepSleep() {
    display.hibernate();
    esp_sleep_enable_ext1_wakeup(BTN_PIN_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

void esp_deep_sleep_start() {
    for (auto hook : emu.sleepHooks) hook();
    throw EmuDeepSleep{}; // unwinds back to wake(), the way power-down abandons whatever was running
}

esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t io_mask, esp_sleep_ext1_wakeup_mode_t) { emu.ext1Mask = io_mask; return 0; }
esp_err_t esp_deep_sleep_register_hook(esp_deep_sleep_cb_t cb) { emu.sleepHooks.push_back(cb); return 0; }
int gpio_get_level(gpio_num_t) { return 1; }

float Watchy::getBatteryVoltage() { return emu.batteryVolts; }

void Watchy::vibMotor(uint8_t intervalMs, uint8_t length) {
    uint32_t ms = (uint32_t)intervalMs * length;
    EmuVibe v = {(uint32_t)(emu.ms - emu.wakeStartMs), ms};
    if (emu.realtime) emuStreamVibe(v);
    else emu.vibes.push_back(v);
    delay(ms);
}

void Watchy::showWatchFace(bool partialRefresh) {
    display.setFullWindow();
    display.epd2.asyncPowerOn();
    drawWatchFace();
    display.display(partialRefresh);
    guiState = WATCHFACE_STATE;
}

void Watchy::drawWatchFace() {
    display.setFont(&DSEG7_Classic_Bold_53);
    display.setCursor(5, 53 + 60);
    if (currentTime.Hour < 10) display.print("0");
    display.print(currentTime.Hour);
    display.print(":");
    if (currentTime.Minute < 10) display.print("0");
    display.println(currentTime.Minute);
}

void Watchy::handleButtonPress() {
    uint64_t wakeupBit = esp_sleep_get_ext1_wakeup_status();
    if (wakeupBit & MENU_BTN_MASK) {
        if (guiState == WATCHFACE_STATE) {
            showMenu(menuIndex, false);
        } else if (guiState == MAIN_MENU_STATE) {
            switch (menuIndex) {
            case 0: showAbout(); break;
            case 1: showBuzz(); break;
            case 2: showAccelerometer(); break;
            case 3: setTime(); break;
            case 4: setupWifi(); break;
            case 5: showSyncNTP(); break;
            default: break;
            }
        }
    } else if (wakeupBit & BACK_BTN_MASK) {
        if (guiState == MAIN_MENU_STATE) {
            RTC.read(currentTime);
            showWatchFace(false);
        } else if (guiState == APP_STATE || guiState == FW_UPDATE_STATE) {
            showMenu(menuIndex, false);
        }
    } else if (wakeupBit & UP_BTN_MASK) {
        if (guiState == MAIN_MENU_STATE) {
            menuIndex--;
            if (menuIndex < 0) menuIndex = MENU_LENGTH - 1;
            showMenu(menuIndex, true);
        }
    } else if (wakeupBit & DOWN_BTN_MASK) {
        if (guiState == MAIN_MENU_STATE) {
            menuIndex++;
            if (menuIndex > MENU_LENGTH - 1) menuIndex = 0;
            showMenu(menuIndex, true);
        }
    }
}

void Watchy::showMenu(byte menuIndex, bool partialRefresh) {
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);

    int16_t x1, y1;
    uint16_t w, h;
    int16_t yPos;

    const char *menuItems[] = {"About Watchy", "Vibrate Motor", "Show Accelerometer",
                               "Set Time", "Setup WiFi", "Sync NTP"};
    for (int i = 0; i < MENU_LENGTH; i++) {
        yPos = MENU_HEIGHT + (MENU_HEIGHT * i);
        display.setCursor(0, yPos);
        if (i == menuIndex) {
            display.getTextBounds(menuItems[i], 0, yPos, &x1, &y1, &w, &h);
            display.fillRect(x1 - 1, y1 - 10, 200, h + 15, GxEPD_WHITE);
            display.setTextColor(GxEPD_BLACK);
            display.println(menuItems[i]);
        } else {
            display.setTextColor(GxEPD_WHITE);
            display.println(menuItems[i]);
        }
    }

    display.display(partialRefresh);

    guiState = MAIN_MENU_STATE;
    alreadyInMenu = false;
}

void Watchy::showPlaceholderApp(const char *title, const char *line1, const char *line2) {
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(0, 20);
    display.println(title);
    display.println();
    display.println(line1);
    display.println(line2);
    display.display(false);
    guiState = APP_STATE;
}

void Watchy::showAbout() {
    char v[32];
    snprintf(v, sizeof(v), "Batt: %.2fV", emu.batteryVolts);
    showPlaceholderApp("About Watchy", "Emulator (V3)", v);
}

void Watchy::showBuzz() {
    showPlaceholderApp("Buzz!", "", "");
    vibMotor();
    showMenu(menuIndex, false);
}

void Watchy::showAccelerometer() { showPlaceholderApp("Accelerometer", "Not emulated.", ""); }
void Watchy::setTime() { showPlaceholderApp("Set Time", "Use the time", "controls instead."); }
void Watchy::setupWifi() { showPlaceholderApp("Setup WiFi", "Use the WiFi", "toggle instead."); }

void Watchy::showSyncNTP() {
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(0, 30);
    display.println("Syncing NTP... ");
    display.display(true);
    if (connectWiFi()) {
        display.println(syncNTP() ? "NTP Sync Success" : "NTP Sync Failed");
    } else {
        display.println("WiFi Not Configured");
    }
    display.display(true);
    delay(3000);
    guiState = APP_STATE;
}

bool Watchy::connectWiFi() {
    delay(EMU_WIFI_CONNECT_MS);
    WIFI_CONFIGURED = emu.wifiAvailable;
    emuLog(WIFI_CONFIGURED ? "WiFi connected" : "WiFi unavailable");
    return WIFI_CONFIGURED;
}

bool Watchy::syncNTP() { return syncNTP(gmtOffset, settings.ntpServer); }
bool Watchy::syncNTP(long gmt) { return syncNTP(gmt, settings.ntpServer); }
bool Watchy::syncNTP(long, String) {
    // the emulator's "world" clock is already local time, so the offset is ignored
    if (!emu.wifiAvailable) return false;
    emuSetRtc(emuWorldNow());
    emuLog("NTP synced");
    return true;
}

weatherData Watchy::getWeatherData() {
    weatherData w = {};
    w.temperature = emu.temperature;
    w.weatherConditionCode = emu.weatherCode;
    w.isMetric = emu.metric;
    w.weatherDescription = "emulated";
    w.external = emu.wifiAvailable; // like the real thing, only an answer from the service counts
    // the service reports sunrise and sunset as UTC timestamps
    double midnight = floor(emuRtcNow() / 86400) * 86400;
    breakTime((time_t)(midnight + emu.sunriseMinute * 60 - gmtOffset), w.sunrise);
    breakTime((time_t)(midnight + emu.sunsetMinute * 60 - gmtOffset), w.sunset);
    return w;
}

// ---------------- command loop ----------------

static std::string base64(const std::vector<uint8_t> &in) {
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
    }
    if (i < in.size()) {
        uint32_t v = in[i] << 16 | (i + 1 < in.size() ? in[i + 1] << 8 : 0);
        out += tbl[v >> 18]; out += tbl[(v >> 12) & 63];
        out += i + 1 < in.size() ? tbl[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

static std::string jsonString(const std::string &s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if ((unsigned char)c < 0x20) out += ' ';
        else out += c;
    }
    return out + "\"";
}

static std::string formatTime(double epoch) {
    tmElements_t tm;
    breakTime((time_t)epoch, tm);
    char b[32];
    snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d", tmYearToCalendar(tm.Year), tm.Month, tm.Day, tm.Hour, tm.Minute, tm.Second);
    return b;
}

static void wake(int cause, uint64_t buttons = 0) {
    emu.wakeCause = cause;
    emu.wakeButtons = buttons;
    emu.wakeStartMs = emu.ms;
    emu.sleepHooks.clear(); // hooks live in RAM, gone after deep sleep
    try {
        setup();
    } catch (const EmuDeepSleep &) {
    }
}

// streamed lines are told apart from command replies by their first key
void emuStreamFrame(const EmuFrame &f) {
    std::ostringstream o;
    o << "{\"frame\":{\"t\":" << f.t << ",\"partial\":" << (f.partial ? "true" : "false")
      << ",\"bits\":\"" << base64(f.bits) << "\"}}\n";
    fputs(o.str().c_str(), stdout);
    fflush(stdout);
}

void emuStreamVibe(const EmuVibe &v) {
    printf("{\"vibe\":{\"t\":%u,\"ms\":%u}}\n", v.t, v.ms);
    fflush(stdout);
}

static void reply(bool ok, const std::string &error = "") {
    std::ostringstream o;
    o << "{\"ok\":" << (ok ? "true" : "false");
    if (!error.empty()) o << ",\"error\":" << jsonString(error);
    o << ",\"frames\":[";
    for (size_t i = 0; i < emu.frames.size(); i++) {
        auto &f = emu.frames[i];
        o << (i ? "," : "") << "{\"t\":" << f.t << ",\"partial\":" << (f.partial ? "true" : "false")
          << ",\"bits\":\"" << base64(f.bits) << "\"}";
    }
    o << "],\"vibes\":[";
    for (size_t i = 0; i < emu.vibes.size(); i++) {
        o << (i ? "," : "") << "{\"t\":" << emu.vibes[i].t << ",\"ms\":" << emu.vibes[i].ms << "}";
    }
    o << "],\"log\":[";
    for (size_t i = 0; i < emu.log.size(); i++) o << (i ? "," : "") << jsonString(emu.log[i]);
    o << "],\"awakeMs\":" << (emu.ms - emu.wakeStartMs)
      << ",\"rtc\":" << jsonString(formatTime(emuRtcNow()))
      << ",\"world\":" << jsonString(formatTime(emuWorldNow()))
      << ",\"guiState\":" << guiState
      << ",\"wifiConfigured\":" << (WIFI_CONFIGURED ? "true" : "false")
      << ",\"usb\":" << (emu.usbPlugged ? "true" : "false")
      << "}\n";
    fputs(o.str().c_str(), stdout);
    fflush(stdout);
}

int main() {
    // start with the watch and the world agreeing on the host's local time
    time_t host = time(nullptr);
    struct tm local;
    localtime_r(&host, &local);
    double localEpoch = (double)host + local.tm_gmtoff;
    emu.worldEpochAtZero = localEpoch;
    emu.rtcEpochAtZero = localEpoch;
    randomSeed((unsigned long)host);
    emu.realtime = getenv("EMU_REALTIME") && atoi(getenv("EMU_REALTIME"));
    if (getenv("EMU_ACCEL_FILE")) emu.accelFile = getenv("EMU_ACCEL_FILE");
    if (getenv("EMU_HTTP_CACHE")) emu.httpCacheDir = getenv("EMU_HTTP_CACHE");

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string cmd;
        in >> cmd;
        emu.frames.clear();
        emu.vibes.clear();
        emu.log.clear();
        emu.wakeStartMs = emu.ms;

        if (cmd == "boot") {
            // a power-on reset clears RTC memory
            guiState = 0;
            menuIndex = 0;
            WIFI_CONFIGURED = false;
            alreadyInMenu = true;
            wake(ESP_SLEEP_WAKEUP_UNDEFINED);
            reply(true);
        } else if (cmd == "tick") {
            // sleep until the RTC's next minute boundary, then wake on the alarm
            double now = emuRtcNow();
            double next = (double)(((long long)now / 60 + 1) * 60);
            emu.ms += (uint64_t)((next - now) * 1000.0 + 0.5);
            wake(ESP_SLEEP_WAKEUP_TIMER);
            reply(true);
        } else if (cmd == "button") {
            std::string b;
            in >> b;
            uint64_t mask = b == "menu" ? MENU_BTN_MASK : b == "back" ? BACK_BTN_MASK : b == "up" ? UP_BTN_MASK : b == "down" ? DOWN_BTN_MASK : 0;
            if (!mask) { reply(false, "unknown button " + b); continue; }
            wake(ESP_SLEEP_WAKEUP_EXT1, mask);
            reply(true);
        } else if (cmd == "motion") {
            // the accelerometer's any-motion interrupt; only wakes the watch if the face armed it
            if (!(emu.ext1Mask & ACC_INT_MASK)) { emuLog("motion ignored: the face didn't arm motion wake-up"); reply(true); continue; }
            wake(ESP_SLEEP_WAKEUP_EXT1, ACC_INT_MASK);
            reply(true);
        } else if (cmd == "redraw") {
            // a minute-tick wake-up without moving the clock on, so new sensor values show straight away
            wake(ESP_SLEEP_WAKEUP_TIMER);
            reply(true);
        } else if (cmd == "usb") {
            int v = 0;
            in >> v;
            emu.usbPlugged = v;
            wake(ESP_SLEEP_WAKEUP_EXT0);
            reply(true);
        } else if (cmd == "settime") {
            // sets both the world and the watch, like setting the time and syncing
            int Y, M, D, h, m, s;
            if (!(in >> Y >> M >> D >> h >> m >> s)) { reply(false, "settime Y M D h m s"); continue; }
            tmElements_t tm;
            tm.Year = CalendarYrToTm(Y); tm.Month = M; tm.Day = D; tm.Hour = h; tm.Minute = m; tm.Second = s;
            double e = makeTime(tm);
            emu.worldEpochAtZero = e - emu.ms / 1000.0;
            emuSetRtc(e);
            reply(true);
        } else if (cmd == "sleep") {
            double secs = 0;
            in >> secs;
            emu.ms += (uint64_t)(secs * 1000);
            reply(true);
        } else if (cmd == "battery") {
            in >> emu.batteryVolts;
            reply(true);
        } else if (cmd == "steps") {
            in >> emu.steps;
            reply(true);
        } else if (cmd == "weather") {
            int metric = 1;
            in >> emu.temperature >> emu.weatherCode >> metric;
            emu.metric = metric;
            reply(true);
        } else if (cmd == "wifi") {
            int v = 1;
            in >> v;
            emu.wifiAvailable = v;
            reply(true);
        } else if (cmd == "sun") {
            // sun <sunrise minute of day> <sunset minute of day>, local time
            in >> emu.sunriseMinute >> emu.sunsetMinute;
            reply(true);
        } else if (cmd == "liveweather") {
            int v = 1;
            in >> v;
            emu.liveWeather = v;
            reply(true);
        } else if (cmd == "accel") {
            int x = 0, y = 0, z = 1024;
            in >> x >> y >> z;
            emu.accel[0] = x; emu.accel[1] = y; emu.accel[2] = z;
            reply(true);
        } else if (cmd == "state") {
            reply(true);
        } else {
            reply(false, "unknown command " + cmd);
        }
    }
    return 0;
}
