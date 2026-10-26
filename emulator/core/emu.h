// Shared simulated-hardware state for the emulator.
#ifndef EMU_H
#define EMU_H

#include <stdint.h>
#include <string>
#include <vector>
#include <time.h>

// how long the panel is busy for each refresh; these drive both the virtual clock and playback
#define EMU_PARTIAL_REFRESH_MS 400
#define EMU_FULL_REFRESH_MS 1800
#define EMU_WIFI_CONNECT_MS 1500

struct EmuFrame {
    uint32_t t;   // ms since the wake-up started
    bool partial;
    std::vector<uint8_t> bits;
};

struct EmuVibe {
    uint32_t t;
    uint32_t ms;
};

struct EmuState {
    uint64_t ms = 0;            // virtual monotonic clock
    double worldEpochAtZero = 0; // "true" local time (seconds) when ms == 0; what NTP returns
    double rtcEpochAtZero = 0;   // the watch's own clock, which can drift from the world or be wiped

    int wakeCause = 0;
    uint64_t wakeButtons = 0;
    uint64_t wakeStartMs = 0;

    float batteryVolts = 4.0f;
    uint32_t steps = 1234;
    int temperature = 21;
    int weatherCode = 800;
    bool metric = true;
    bool wifiAvailable = true;
    bool liveWeather = true;   // faces ask the real weather service (through the Mac) rather than the panel
    std::string httpCacheDir;  // where HTTP answers are cached; none if empty
    int sunriseMinute = 7 * 60, sunsetMinute = 18 * 60 + 30; // local time of day
    bool usbPlugged = false;

    uint64_t ext1Mask = 0;  // pins armed to wake the watch from its last deep sleep
    std::vector<void (*)(void)> sleepHooks;
    int16_t accel[3] = {0, 0, -1024}; // raw reading as the Watchy library reports it, 1024 = 1g; flat and face up (which reads z = -1g)

    // real-time mode: delay() really waits and frames are streamed as they're drawn, so the page can
    // steer the accelerometer while a long animation runs
    bool realtime = false;
    std::string accelFile; // in real-time mode the accelerometer is re-read from here ("x y z")

    std::vector<EmuFrame> frames;
    std::vector<EmuVibe> vibes;
    std::vector<std::string> log;
};

extern EmuState emu;

void emuLog(const std::string &line);
void emuStreamFrame(const EmuFrame &f);
void emuStreamVibe(const EmuVibe &v);
double emuWorldNow();
double emuRtcNow();
void emuSetRtc(double epoch);

#endif
