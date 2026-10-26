// Emulated Watchy library: same class shape as the real one (Watchy v1.4.x, V3 hardware),
// with the hardware swapped for simulated parts driven by the emulator.
#ifndef WATCHY_H
#define WATCHY_H

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <TimeLib.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include "DSEG7_Classic_Bold_53.h"

#define RTC_DATA_ATTR

#define GxEPD_BLACK 0x0000
#define GxEPD_WHITE 0xFFFF

// config.h (V3 / ESP32-S3 values)
#define WATCHFACE_STATE -1
#define MAIN_MENU_STATE 0
#define APP_STATE       1
#define FW_UPDATE_STATE 2
#define MENU_HEIGHT     25
#define MENU_LENGTH     6
#define HOUR_12_24      24
#define MENU_BTN_MASK (BIT64(7))
#define BACK_BTN_MASK (BIT64(6))
#define UP_BTN_MASK   (BIT64(0))
#define DOWN_BTN_MASK (BIT64(8))
#define BTN_PIN_MASK  MENU_BTN_MASK|BACK_BTN_MASK|UP_BTN_MASK|DOWN_BTN_MASK
#define MENU_BTN_PIN  7
#define BACK_BTN_PIN  6
#define UP_BTN_PIN    0
#define DOWN_BTN_PIN  8
#define ACC_INT_1_PIN 14
#define ACC_INT_MASK  (BIT64(14))

typedef enum {
    ESP_SLEEP_WAKEUP_UNDEFINED = 0,
    ESP_SLEEP_WAKEUP_ALL,
    ESP_SLEEP_WAKEUP_EXT0,
    ESP_SLEEP_WAKEUP_EXT1,
    ESP_SLEEP_WAKEUP_TIMER,
} esp_sleep_wakeup_cause_t;
esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause();
uint64_t esp_sleep_get_ext1_wakeup_status();

// deep-sleep wake-up configuration, recorded so the emulator knows what may wake the watch
typedef enum { ESP_EXT1_WAKEUP_ANY_LOW = 0, ESP_EXT1_WAKEUP_ANY_HIGH = 1 } esp_sleep_ext1_wakeup_mode_t;
typedef int esp_err_t;
typedef void (*esp_deep_sleep_cb_t)(void);
esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t io_mask, esp_sleep_ext1_wakeup_mode_t level_mode);
esp_err_t esp_deep_sleep_register_hook(esp_deep_sleep_cb_t cb);
[[noreturn]] void esp_deep_sleep_start(); // ends the wake-up, like the real chip powering down
struct EmuDeepSleep {};
typedef int gpio_num_t;
int gpio_get_level(gpio_num_t pin); // the accelerometer's interrupt line reads idle (high)

typedef enum { WIFI_OFF, WIFI_STA, WIFI_AP, WIFI_AP_STA } wifi_mode_t;
bool emuLiveWeather(); // the emulator's "Live weather" switch: ask the real service instead of the panel
struct EmuWiFi {
    void mode(wifi_mode_t) {}
    String SSID() { return "EmuNet"; }
};
extern EmuWiFi WiFi;
void btStop();

struct bma4_accel {
    int16_t x, y, z;
};
typedef struct bma4_accel Accel;

class BMA423 {
public:
    uint32_t getCounter();
    bool resetStepCounter();
    bool getAccel(Accel &acc);
    uint8_t getDirection() { return 0; }
};

typedef struct weatherData {
    int8_t temperature;
    int16_t weatherConditionCode;
    bool isMetric;
    String weatherDescription;
    bool external;
    tmElements_t sunrise;
    tmElements_t sunset;
} weatherData;

typedef struct watchySettings {
    String cityID;
    String lat;
    String lon;
    String weatherAPIKey;
    String weatherURL;
    String weatherUnit;
    String weatherLang;
    int8_t weatherUpdateInterval;
    String ntpServer;
    int gmtOffset;
    bool vibrateOClock;
} watchySettings;

class EmuEPD {
public:
    void initWatchy() {}
    void asyncPowerOn() {}
};

// 200x200 1-bit e-paper; every display() call is captured as a frame
class EmuDisplay : public Adafruit_GFX {
public:
    static const int16_t WIDTH = 200;
    static const int16_t HEIGHT = 200;
    EmuDisplay() : Adafruit_GFX(WIDTH, HEIGHT) { memset(buffer, 0xFF, sizeof(buffer)); }
    void drawPixel(int16_t x, int16_t y, uint16_t color) override;
    void display(bool partial_update_mode = false);
    void setFullWindow() {}
    void setPartialWindow(int16_t, int16_t, int16_t, int16_t) {}
    void hibernate() {}
    void powerOff() {}
    EmuEPD epd2;
    uint8_t buffer[WIDTH * HEIGHT / 8];
};

class EmuRTC {
public:
    void init() {}
    void config(String datetime);
    void clearAlarm() {}
    void read(tmElements_t &tm);
    void set(tmElements_t tm);
    uint8_t temperature() { return 25; }
};

class Watchy {
public:
    static EmuRTC RTC;
    static EmuDisplay display;
    tmElements_t currentTime;
    watchySettings settings;

public:
    explicit Watchy(const watchySettings &s) : settings(s) {}
    virtual ~Watchy() {}
    void init(String datetime = "");
    void deepSleep();
    float getBatteryVoltage();
    uint8_t getBoardRevision() { return 3; }
    void vibMotor(uint8_t intervalMs = 100, uint8_t length = 20);

    virtual void handleButtonPress();
    void showMenu(byte menuIndex, bool partialRefresh);
    void showFastMenu(byte menuIndex) { showMenu(menuIndex, true); }
    void showAbout();
    void showBuzz();
    void showAccelerometer();
    void showUpdateFW() {}
    void showSyncNTP();
    bool syncNTP();
    bool syncNTP(long gmt);
    bool syncNTP(long gmt, String ntpServer);
    void setTime();
    void setupWifi();
    bool connectWiFi();
    weatherData getWeatherData();
    void updateFWBegin() {}

    void showWatchFace(bool partialRefresh);
    virtual void drawWatchFace();

private:
    void showPlaceholderApp(const char *title, const char *line1, const char *line2);
};

extern RTC_DATA_ATTR int guiState;
extern RTC_DATA_ATTR int menuIndex;
extern RTC_DATA_ATTR BMA423 sensor;
extern RTC_DATA_ATTR bool WIFI_CONFIGURED;
extern RTC_DATA_ATTR bool BLE_CONFIGURED;
extern RTC_DATA_ATTR bool USB_PLUGGED_IN;
extern RTC_DATA_ATTR long gmtOffset;

#endif
