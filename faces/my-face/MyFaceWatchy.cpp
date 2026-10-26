#include "MyFaceWatchy.h"
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <WiFiClientSecure.h>
#ifndef EMULATOR
#include <Wire.h>
#include <driver/gpio.h>
#endif

// The face looks straight out of the screen and is lit like a clock: a broad key light comes from
// the hour hand's direction and a thin rim light catches the edge of the face from the minute hand.
// Brightness per pixel comes from the baked normal map, then blue-noise dithering turns it into dots.
// The head turns with the watch's tilt by switching between maps baked at other angles: moving the watch
// wakes it to follow, and Back plays the light round the face then a live minute with a second hand.
// The weather sets the light (hard sun, soft cloud, rain or snow falling behind the head), the screen
// turns dark between sunset and sunrise, and the minute ticks double as the battery gauge.

#define KEY_ELEVATION 35.0f  // degrees the key light sits in front of the face; lower = harsher side light
#define RIM_ELEVATION 5.0f   // the rim light grazes the face from almost exactly the side
#define RIM_POWER 6          // higher = thinner rim highlight
// brightness of the side facing away from both lights, the key light and the rim light (0-255),
// under a clear sky and a fully overcast one (the light moves between them with the cloud cover):
// cloud doesn't darken the face, it lifts the shadows and softens the light
#define CLEAR_LIGHT    18.0f, 235.0f, 150.0f
#define OVERCAST_LIGHT 125.0f, 140.0f, 60.0f
#define STORM_LIGHT    8.0f, 225.0f, 190.0f  // thunderstorms are the exception: dark and dramatic
#define FOG_LIGHT      125.0f, 135.0f, 25.0f // fog: hardly any shadow, the head fading into it
#define FOG_DENSITY 70                       // darkest haze band in the thickest fog, 0-255, as a share of dotted pixels
#define FOG_BAND_PX 34                       // distance between haze bands
#define LOW_BATTERY_PERCENT 15     // below this the face dims, as a hint to charge
#define LOW_BATTERY_DIMMING 0.55f  // key and rim light are multiplied by this

#define DEFAULT_SUNRISE (7 * 60)   // minutes of the day, until the weather has told us the real times
#define DEFAULT_SUNSET (19 * 60)
#define TEMPERATURE_X 23           // centre of the temperature, in the bottom-left corner outside the dial
#define TEMPERATURE_Y 190
#define SECOND_MARKER_RADIUS 7     // the second hand's fuzzy dot
#define JOKE_URL "https://icanhazdadjoke.com/"
#define JOKE_USER_AGENT "Watchy my-face (personal watch face)"
#define JOKE_SECONDS 60            // how long a joke stays up before the face comes back
#define JOKE_WIDTH 184             // the joke's text box, centred on the screen
#define JOKE_HEIGHT 176
#define DEGREE_GREY 215
#define CHARGING_X 182             // the charging bolt, in the bottom-right corner outside the dial
#define CHARGING_Y 176             // share of dots in the temperature's degree sign, 0-255
#define FOG_VISIBILITY 5000        // metres; below this it counts as foggy whatever the conditions say

#define LIVE_SECONDS 60       // how long Back shows the ticking second hand before the watch sleeps again
#define ORBIT_FRAMES 12       // frames for the key light's lap round the face at the start of it

#define TILT_YAW_SIGN 1       // flip to -1 if the head turns the wrong way when the watch tilts left/right
#define TILT_PITCH_SIGN 1     // flip to -1 if it nods the wrong way when the watch tilts toward/away from you
#define TILT_GAIN 1.0f        // degrees the head turns per degree the watch tilts
#define TILT_SMOOTHING 0.35f  // 0-1, how much each accelerometer reading counts; lower = steadier but laggier
#define TILT_HYSTERESIS 0.2f  // fraction of a step past halfway before the head commits to the next angle

#define MOTION_THRESHOLD 0x100      // any-motion threshold in 1/2048 g (about 125 mg): a deliberate movement, not typing
#define MOTION_DURATION 4           // samples (20 ms each) the motion has to last before it wakes the watch
#define MOTION_WAKES_PER_MINUTE 6   // after this many looks at the watch, motion is ignored until the next minute tick
#define IGNORED_WAKES_PER_MINUTE 10 // and after this many wakes that weren't a look (walking, reaching for things)
#define RAISE_WINDOW_MS 600         // after a motion wake, how long the watch has to come up to be looked at
#define RAISE_FACING_UP 0.55f       // the screen has to face at least this much upward (1 = flat, screen up)
#define RAISE_MIN_TURN_DEG 25.0f    // having turned at least this far from where it last rested
#define RAISE_HOLD_MS 120           // and stayed there this long
#define MOTION_SETTLE_MS 700        // after a motion wake, sleep again once the head hasn't moved for this long
#define MOTION_FOLLOW_MS 4000       // and never stay awake longer than this
#define EASE_STEP 1                 // baked angles per frame as the head turns back to face you once still
#define STRAIGHT_ON_VIEW ((FACE_PITCH_STEPS / 2) * FACE_YAW_STEPS + FACE_YAW_STEPS / 2)

#define MEAL_FADE_MINUTES 30  // the mouth opens over this long before lunch and dinner, and closes after (see MEALS)

#define HOUR_NUMBER_X 32      // centre of the stippled hour number, left of the head
#define MINUTE_NUMBER_X 170   // and the minutes, right of the head
#define NUMBER_Y 100

#define DIAL_CENTER 100
#define DIAL_OUTER 99
#define MINUTE_TICK_INNER 94
#define HOUR_TICK_INNER 90

static const float DEG = 0.0174532925f;

// ---------- conditions: weather, day and night, battery ----------

// The weather as OpenWeatherMap last reported it, kept through deep sleep. The service can report
// several conditions at once (rain and mist, a thunderstorm with rain), plus measured amounts, so the
// face layers its effects: cloud cover softens the light, fog thickens as visibility drops, rain gets
// denser with the amount falling and slants with the wind, and thunderstorms add lightning.
struct WeatherReport {
    bool known;
    uint32_t fetchedAt;        // local time of the last answer (seconds, like now())
    char place[24];
    char summary[48];          // the conditions in words, e.g. "light rain, mist"
    int16_t codes[4];          // condition ids: 2xx thunder, 3xx drizzle, 5xx rain, 6xx snow, 7xx fog/haze, 800 clear, 80x clouds
    uint8_t codeCount;
    int8_t temperature, feelsLike;
    uint8_t humidity, clouds;  // percent
    float rain, snow;          // mm in the last hour
    uint16_t visibility;       // metres, 10000 at most
    float wind;                // in the units asked for: m/s for metric, mph for imperial
    uint16_t windFrom;         // degrees the wind blows from
    int16_t sunrise, sunset;   // local minute of the day
    bool metric;
};
static RTC_DATA_ATTR WeatherReport weather = {};
static RTC_DATA_ATTR uint8_t batteryLevel = 100; // percent

static bool hasCondition(int16_t from, int16_t to){
    for(uint8_t i = 0; i < weather.codeCount; i++){
        if(weather.codes[i] >= from && weather.codes[i] < to) return true;
    }
    return false;
}

enum Precipitation { NO_PRECIPITATION, RAIN, SNOW };

// how everything on the face is drawn right now
struct Look {
    float ambient, key, rim;     // face lighting
    bool night;                  // dark mode: black background, white ink
    uint16_t ink, paper;
    uint8_t fog;                 // 0-255, darkest haze band
    Precipitation precipitation;
    uint8_t drops, dropLength;   // streaks or flakes, and how long the streaks are
    float slant;                 // sideways drift per pixel of fall
    bool lightning;
    uint8_t battery;
};
static Look look;

static float mix(float a, float b, float t){
    return a + (b - a) * t;
}

static void setLight(float ambient, float key, float rim){
    look.ambient = ambient;
    look.key = key;
    look.rim = rim;
}

static void prepareLook(const tmElements_t &now){
    int minute = now.Hour * 60 + now.Minute;
    int16_t sunrise = weather.known ? weather.sunrise : DEFAULT_SUNRISE;
    int16_t sunset = weather.known ? weather.sunset : DEFAULT_SUNSET;
    look.night = minute < sunrise || minute >= sunset;
    look.ink = look.night ? GxEPD_WHITE : GxEPD_BLACK;
    look.paper = look.night ? GxEPD_BLACK : GxEPD_WHITE;
    look.battery = batteryLevel;

    // light: from hard sun to soft overcast with the cloud cover; storms and fog have their own
    static const float clear[] = {CLEAR_LIGHT}, overcast[] = {OVERCAST_LIGHT};
    float cloud = weather.known ? weather.clouds / 100.0f : 0;
    setLight(mix(clear[0], overcast[0], cloud), mix(clear[1], overcast[1], cloud), mix(clear[2], overcast[2], cloud));

    look.fog = 0;
    bool foggy = hasCondition(700, 800) || (weather.known && weather.visibility < FOG_VISIBILITY);
    if(foggy){
        float thickness = constrain((10000.0f - weather.visibility) / (10000.0f - 1000.0f), 0.35f, 1.0f);
        look.fog = FOG_DENSITY * thickness;
        setLight(FOG_LIGHT);
    }

    look.lightning = hasCondition(200, 300);
    if(look.lightning){
        setLight(STORM_LIGHT);
    }

    // rain or snow, as much as was measured; if the amount is missing, a typical one for the condition
    bool snowing = hasCondition(600, 700) || weather.snow > 0;
    bool raining = hasCondition(200, 600) || weather.rain > 0;
    look.precipitation = snowing ? SNOW : raining ? RAIN : NO_PRECIPITATION;
    float rate = snowing ? weather.snow : weather.rain;
    if(rate <= 0){
        rate = hasCondition(502, 505) || hasCondition(521, 532) ? 6.0f // heavy rain and showers
             : look.lightning ? 4.0f
             : hasCondition(500, 600) || snowing ? 1.5f
             : 0.3f; // drizzle
    }
    look.drops = constrain(20 + rate * 15, 20, 100);
    look.dropLength = hasCondition(300, 400) && !hasCondition(500, 600) ? 3 : constrain(5 + rate * 1.8f, 5, 16);
    float windMs = weather.metric ? weather.wind : weather.wind * 0.447f;
    look.slant = 0.15f + 0.6f * constrain(windMs / 12.0f, 0.0f, 1.0f);

    if(look.battery < LOW_BATTERY_PERCENT){
        look.key *= LOW_BATTERY_DIMMING;
        look.rim *= LOW_BATTERY_DIMMING;
    }
}

// rough charge left from the cell voltage (a LiPo's curve, flattened into even steps). The V3's
// battery reading tops out at about 3.96V (the ADC maxes out near 3.1V behind the 1.28 divider),
// so everything from there up to a full 4.2V reads the same and counts as full.
static uint8_t batteryPercent(float volts){
    static const float STEPS[] = {3.50f, 3.60f, 3.68f, 3.75f, 3.82f, 3.89f, 3.95f}; // 0%, 16.7%, ... 100%
    const int n = sizeof(STEPS) / sizeof(STEPS[0]);
    if(volts <= STEPS[0]) return 0;
    for(int i = 1; i < n; i++){
        if(volts < STEPS[i]){
            return (uint8_t)(100.0f * (i - 1 + (volts - STEPS[i - 1]) / (STEPS[i] - STEPS[i - 1])) / (n - 1));
        }
    }
    return 100;
}

extern RTC_DATA_ATTR long gmtOffset; // Watchy.cpp: seconds from UTC, set from the weather service's answer

// minutes into the local day of a UTC timestamp from the weather service
static int16_t localMinuteOfDay(uint32_t utc){
    return (int16_t)(((utc + gmtOffset) % 86400) / 60);
}

static uint32_t localNow(const tmElements_t &now){
    tmElements_t t = now;
    return makeTime(t);
}

// ask OpenWeatherMap for the current weather; false if there's no Wi-Fi or no answer
bool MyFaceWatchy::askWeatherService(){
    if(!connectWiFi()){
        return false;
    }
    String url = settings.weatherURL;
    url.replace("{cityID}", settings.cityID);
    url.replace("{lat}", settings.lat);
    url.replace("{lon}", settings.lon);
    url.replace("{units}", settings.weatherUnit);
    url.replace("{lang}", settings.weatherLang);
    url.replace("{apiKey}", settings.weatherAPIKey);
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.begin(url);
    bool ok = http.GET() == 200;
    if(ok){
        JSONVar r = JSON.parse(http.getString());
        JSONVar conditions = r["weather"];
        weather.codeCount = 0;
        weather.summary[0] = 0;
        for(int i = 0; i < conditions.length() && weather.codeCount < 4; i++){
            weather.codes[weather.codeCount++] = (int)conditions[i]["id"];
            if(i){
                strlcat(weather.summary, ", ", sizeof(weather.summary));
            }
            strlcat(weather.summary, (const char *)conditions[i]["description"], sizeof(weather.summary));
        }
        strlcpy(weather.place, (const char *)r["name"], sizeof(weather.place));
        weather.temperature = (int)round((double)r["main"]["temp"]);
        weather.feelsLike = (int)round((double)r["main"]["feels_like"]);
        weather.humidity = (int)r["main"]["humidity"];
        weather.clouds = r.hasOwnProperty("clouds") ? (int)r["clouds"]["all"] : 0;
        weather.rain = r.hasOwnProperty("rain") && r["rain"].hasOwnProperty("1h") ? (double)r["rain"]["1h"] : 0;
        weather.snow = r.hasOwnProperty("snow") && r["snow"].hasOwnProperty("1h") ? (double)r["snow"]["1h"] : 0;
        weather.visibility = r.hasOwnProperty("visibility") ? (int)r["visibility"] : 10000;
        weather.wind = (double)r["wind"]["speed"];
        weather.windFrom = r["wind"].hasOwnProperty("deg") ? (int)r["wind"]["deg"] : 0;
        gmtOffset = (int)r["timezone"]; // the city's time zone, daylight saving included
        weather.sunrise = localMinuteOfDay((uint32_t)(double)r["sys"]["sunrise"]);
        weather.sunset = localMinuteOfDay((uint32_t)(double)r["sys"]["sunset"]);
        syncNTP(gmtOffset); // while we're online, like the library does
    }
    http.end();
    WiFi.mode(WIFI_OFF);
    btStop();
    return ok;
}

#ifdef EMULATOR
// the emulator's weather panel, standing in for the service when its "Live weather" switch is off
bool MyFaceWatchy::readWeatherPanel(){
    weatherData w = getWeatherData();
    if(!w.external){
        return false;
    }
    int16_t code = w.weatherConditionCode;
    weather.codeCount = 1;
    weather.codes[0] = code;
    weather.temperature = weather.feelsLike = w.temperature;
    weather.humidity = 60;
    weather.clouds = code == 800 ? 0 : code == 801 ? 20 : code == 802 ? 45 : code >= 803 ? 90 : 100;
    weather.rain = weather.snow = 0;
    weather.visibility = code >= 700 && code < 800 ? 1500 : 10000;
    weather.wind = weather.metric ? 3 : 7;
    weather.windFrom = 270;
    weather.sunrise = localMinuteOfDay(makeTime(w.sunrise));
    weather.sunset = localMinuteOfDay(makeTime(w.sunset));
    strlcpy(weather.place, "Emulator", sizeof(weather.place));
    snprintf(weather.summary, sizeof(weather.summary), "condition %d", code);
    return true;
}
#endif

bool MyFaceWatchy::fetchWeather(){
    weather.metric = settings.weatherUnit == "metric";
#ifdef EMULATOR
    bool ok = emuLiveWeather() ? askWeatherService() : readWeatherPanel();
#else
    bool ok = askWeatherService();
#endif
    if(!ok){
        return false;
    }
    RTC.read(currentTime);
    weather.known = true;
    weather.fetchedAt = localNow(currentTime);
    return true;
}

// called on the minute tick: the battery every time, the weather every WEATHER_UPDATE_INTERVAL minutes
void MyFaceWatchy::updateConditions(){
    batteryLevel = batteryPercent(getBatteryVoltage());
#ifndef EMULATOR
    pinMode(CHRG_STATUS_PIN, INPUT);
    if(USB_PLUGGED_IN && digitalRead(CHRG_STATUS_PIN) == HIGH){
        batteryLevel = 100; // on the cable and the charger says it's done
    }
#endif
#ifdef EMULATOR
    bool due = true; // so changes in the emulator's weather panel show straight away
#else
    bool due = !weather.known || localNow(currentTime) - weather.fetchedAt >= settings.weatherUpdateInterval * 60UL;
#endif
    if(due){
        fetchWeather(); // on failure, keep what we knew
    }
}

// cheap pseudo-random numbers, so the rain falls somewhere new each redraw
static uint32_t scatter(uint32_t &state){
    state = state * 1664525u + 1013904223u;
    return state >> 8;
}

// drifting horizontal bands of haze across the background, dotted with the blue noise (offset so the
// dots don't line up with the face's), moving on a little each redraw
static void drawFog(Adafruit_GFX &g, uint32_t seed){
    if(!look.fog){
        return;
    }
    float drift = (seed % 60) * FOG_BAND_PX / 60.0f;
    for(int16_t y = 0; y < 200; y++){
        float band = 0.5f + 0.5f * sinf((y + drift) * 2 * PI / FOG_BAND_PX);
        uint8_t density = look.fog * band * band;
        for(int16_t x = 0; x < 200; x++){
            if(hypotf(x - DIAL_CENTER, y - DIAL_CENTER) > HOUR_TICK_INNER - 2){
                continue;
            }
            if(pgm_read_byte(&BlueNoise200[((y + 50) % 200) * 200 + (x + 150) % 200]) < density){
                g.drawPixel(x, y, look.ink);
            }
        }
    }
}

// a zigzag lightning bolt in the sky above one of the numbers, on a side that changes each redraw
static void drawLightning(Adafruit_GFX &g, uint32_t seed){
    static const int8_t BOLT[][2] = {{0, 0}, {-7, 16}, {2, 17}, {-6, 33}, {1, 34}, {-8, 50}};
    int16_t x0 = seed % 2 ? 54 : 150, y0 = 26;
    for(uint8_t i = 1; i < sizeof(BOLT) / sizeof(BOLT[0]); i++){
        for(int8_t w = 0; w < 2; w++){ // two pixels wide
            g.drawLine(x0 + BOLT[i - 1][0] + w, y0 + BOLT[i - 1][1], x0 + BOLT[i][0] + w, y0 + BOLT[i][1], look.ink);
        }
    }
}

// rain or snow in the background, inside the dial and clear of the numbers (the head covers whatever
// falls behind it): rain is slanted streaks, longer and denser the harder it falls, snow little crosses
static void drawPrecipitation(Adafruit_GFX &g, uint32_t seed){
    if(look.lightning){
        drawLightning(g, seed);
    }
    if(look.precipitation == NO_PRECIPITATION){
        return;
    }
    uint32_t state = seed * 2654435761u + 1;
    for(uint8_t i = 0; i < look.drops; i++){
        int16_t x = scatter(state) % 200, y = scatter(state) % 200;
        if(hypotf(x - DIAL_CENTER, y - DIAL_CENTER) > HOUR_TICK_INNER - 6){
            continue;
        }
        if(abs(y - NUMBER_Y) < 18 && (abs(x - HOUR_NUMBER_X) < 28 || abs(x - MINUTE_NUMBER_X) < 28)){
            continue;
        }
        if(look.precipitation == SNOW){
            g.drawPixel(x, y, look.ink);
            g.drawPixel(x - 1, y, look.ink);
            g.drawPixel(x + 1, y, look.ink);
            g.drawPixel(x, y - 1, look.ink);
            g.drawPixel(x, y + 1, look.ink);
        }else{
            g.drawLine(x, y, x - look.dropLength * look.slant, y + look.dropLength, look.ink);
        }
    }
}

// direction toward a light at a clock angle (0 = 12 o'clock, clockwise), raised `elevation` toward the viewer
static void clockLight(float angle, float elevation, float out[3]){
    out[0] = sinf(angle) * cosf(elevation * DEG);
    out[1] = cosf(angle) * cosf(elevation * DEG);
    out[2] = sinf(elevation * DEG);
}

// Around meal times the face dissolves, dot by dot, into the open-mouth scan: each pixel takes its
// normal from one scan or the other, the share of open-mouth pixels following the time.
static const uint16_t MEALS[][2] = {{12 * 60, 13 * 60}, {18 * 60, 19 * 60}}; // minutes of the day, mouth fully open

// 0 (closed) to 255 (open): opening over MEAL_FADE_MINUTES before each meal and closing over the same after
static uint8_t mouthOpenness(const tmElements_t &t){
    int now = t.Hour * 60 + t.Minute;
    float open = 0;
    for(const auto &meal : MEALS){
        float o = 0;
        if(now >= meal[0] && now <= meal[1]){
            o = 1;
        }else if(now < meal[0] && now >= meal[0] - MEAL_FADE_MINUTES){
            o = (now - (meal[0] - MEAL_FADE_MINUTES)) / (float)MEAL_FADE_MINUTES;
        }else if(now > meal[1] && now <= meal[1] + MEAL_FADE_MINUTES){
            o = 1 - (now - meal[1]) / (float)MEAL_FADE_MINUTES;
        }
        open = max(open, o);
    }
    return (uint8_t)(open * 255);
}

// the normal at a pixel of the view's box, from whichever scan this pixel shows; null outside the box
static const int8_t *facePixel(uint8_t view, int16_t bx, int16_t by, uint8_t mouth){
    if(bx < 0 || by < 0 || bx >= FACE_BOX_W || by >= FACE_BOX_H){
        return nullptr;
    }
    // the blue noise again, but shifted half a screen so the dissolve doesn't line up with the shading dots
    uint8_t noise = pgm_read_byte(&BlueNoise200[((FACE_BOX_Y + by + 100) % 200) * 200 + (FACE_BOX_X + bx + 100) % 200]);
    bool open = mouth == 255 || noise < mouth;
    return &(open ? FACE_VIEWS_MOUTH : FACE_VIEWS)[view][(by * FACE_BOX_W + bx) * 2];
}

// is there head at this pixel? (anything outside the box is background)
static bool isHead(uint8_t view, int16_t bx, int16_t by, uint8_t mouth){
    const int8_t *normal = facePixel(view, bx, by, mouth);
    return normal && (int8_t)pgm_read_byte(&normal[0]) != -128;
}

void MyFaceWatchy::drawFace(float hourAngle, float minuteAngle, uint8_t view){
    float key[3], rim[3];
    clockLight(hourAngle, KEY_ELEVATION, key);
    clockLight(minuteAngle, RIM_ELEVATION, rim);
    uint8_t mouth = mouthOpenness(currentTime);

    for(int16_t by = 0; by < FACE_BOX_H; by++){
        for(int16_t bx = 0; bx < FACE_BOX_W; bx++){
            if(!isHead(view, bx, by, mouth)){
                continue; // background stays as it is
            }
            int16_t x = FACE_BOX_X + bx, y = FACE_BOX_Y + by;
            int i = y * 200 + x;
            const int8_t *normal = facePixel(view, bx, by, mouth);
            float nx = (int8_t)pgm_read_byte(&normal[0]) / 127.0f;
            float ny = (int8_t)pgm_read_byte(&normal[1]) / 127.0f;
            float z = sqrtf(max(0.0001f, 1.0f - nx * nx - ny * ny));
            float diffuse = max(0.0f, nx * key[0] + ny * key[1] + z * key[2]);
            float edge = max(0.0f, nx * rim[0] + ny * rim[1] + z * rim[2]);
            float rimLight = 1.0f;
            for(uint8_t p = 0; p < RIM_POWER; p++){
                rimLight *= edge;
            }
            float brightness = look.ambient + look.key * diffuse + look.rim * rimLight;
            // outline the head so its lit side doesn't vanish into the white background
            bool outline = !isHead(view, bx - 1, by, mouth) || !isHead(view, bx + 1, by, mouth)
                || !isHead(view, bx, by - 1, mouth) || !isHead(view, bx, by + 1, mouth);
            if(look.fog && (x + y) % 2){
                outline = false; // in fog the outline is dotted, as if the head is fading into it
            }
            // the face is shaded the same day and night; only its outline follows the ink colour
            uint16_t color = brightness <= pgm_read_byte(&BlueNoise200[i]) ? GxEPD_BLACK : GxEPD_WHITE;
            display.drawPixel(x, y, outline ? look.ink : color); // background-coloured dots too, to cover the numbers
        }
    }
}

// a number behind the head: dotted like the face and lit from the same side, with a thin outline.
// `degrees` adds a degree sign (the fonts don't have one): a small, evenly grey dotted disc.
static void drawNumber(Adafruit_GFX &display, const String &text, int16_t centerX, int16_t centerY, float hourAngle,
                       const GFXfont *font = &FreeSansBold18pt7b, bool degrees = false){
    const int16_t CW = 80, CH = 60;
    GFXcanvas1 glyphs(CW, CH);
    glyphs.setFont(font);
    int16_t x1, y1;
    uint16_t w, h;
    glyphs.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    uint16_t ring = degrees ? h / 5 : 0; // the degree sign's radius
    glyphs.setTextColor(1);
    glyphs.setCursor((CW - w - 3 * ring) / 2 - x1, (CH - h) / 2 - y1);
    glyphs.print(text);
    int16_t degreeX = glyphs.getCursorX() + ring + 1, degreeY = (CH - h) / 2 + ring;

    // brighter toward the hour light, fading to dots on the far side
    float lx = sinf(hourAngle), ly = -cosf(hourAngle); // screen y points down
    int16_t ox = centerX - CW / 2, oy = centerY - CH / 2;
    for(int16_t y = 0; y < CH; y++){
        for(int16_t x = 0; x < CW; x++){
            if(!glyphs.getPixel(x, y)){
                continue;
            }
            int16_t sx = ox + x, sy = oy + y;
            if(sx < 0 || sx >= 200 || sy < 0 || sy >= 200){
                continue;
            }
            bool edge = x == 0 || y == 0 || x == CW - 1 || y == CH - 1 || !glyphs.getPixel(x - 1, y) || !glyphs.getPixel(x + 1, y)
                     || !glyphs.getPixel(x, y - 1) || !glyphs.getPixel(x, y + 1);
            float along = ((x - CW / 2.0f) * lx + (y - CH / 2.0f) * ly) / (max(w, h) / 2.0f);
            float brightness = 128 + 110 * along;
            bool shadow = brightness <= pgm_read_byte(&BlueNoise200[sy * 200 + sx]);
            if(edge || shadow != look.night){ // ink on the shadow side by day, on the lit side at night
                display.drawPixel(sx, sy, look.ink);
            }
        }
    }
    if(degrees){
        for(int16_t dy = -ring; dy <= ring; dy++){
            for(int16_t dx = -ring; dx <= ring; dx++){
                int16_t sx = ox + degreeX + dx, sy = oy + degreeY + dy;
                if(dx * dx + dy * dy <= ring * ring && sx >= 0 && sx < 200 && sy >= 0 && sy < 200
                   && pgm_read_byte(&BlueNoise200[sy * 200 + sx]) < DEGREE_GREY){
                    display.drawPixel(sx, sy, look.ink);
                }
            }
        }
    }
}

void MyFaceWatchy::drawNumbers(float hourAngle){
    uint8_t hour = currentTime.Hour % 12 == 0 ? 12 : currentTime.Hour % 12;
    String minutes = String(currentTime.Minute);
    if(currentTime.Minute < 10){
        minutes = "0" + minutes;
    }
    drawNumber(display, String(hour), HOUR_NUMBER_X, NUMBER_Y, hourAngle);
    drawNumber(display, minutes, MINUTE_NUMBER_X, NUMBER_Y, hourAngle);
}

// a small lightning bolt in the bottom-right corner while the USB cable is plugged in
static void drawChargingBolt(Adafruit_GFX &g){
    if(!USB_PLUGGED_IN){
        return;
    }
    const int16_t x = CHARGING_X, y = CHARGING_Y;
    g.fillTriangle(x + 3, y, x - 5, y + 11, x + 1, y + 11, look.ink);
    g.fillTriangle(x - 1, y + 8, x + 5, y + 8, x - 3, y + 19, look.ink);
}

// the outside temperature, smaller, in the bottom-left corner, once the weather service has answered
void MyFaceWatchy::drawTemperature(float hourAngle){
    if(weather.known){
        drawNumber(display, String(weather.temperature), TEMPERATURE_X, TEMPERATURE_Y, hourAngle, &FreeSansBold12pt7b, true);
    }
}

void MyFaceWatchy::drawDial(float hourAngle, float minuteAngle){
    // the ticks are the battery gauge: full ticks from 12 o'clock round to the charge left, then just a dot
    uint8_t charged = (look.battery * 60 + 50) / 100;
    for(uint8_t m = 0; m < 60; m++){
        float a = m * 6 * DEG;
        int16_t inner = m < charged ? (m % 5 == 0 ? HOUR_TICK_INNER : MINUTE_TICK_INNER) : DIAL_OUTER - 1;
        display.drawLine(DIAL_CENTER + sinf(a) * DIAL_OUTER, DIAL_CENTER - cosf(a) * DIAL_OUTER,
                         DIAL_CENTER + sinf(a) * inner, DIAL_CENTER - cosf(a) * inner, look.ink);
    }
    // the hands only show as markers on the rim; the light on the face does the rest
    display.fillCircle(DIAL_CENTER + sinf(hourAngle) * 92, DIAL_CENTER - cosf(hourAngle) * 92, 5, look.ink);
    for(int8_t w = -1; w <= 1; w++){
        float a = minuteAngle + w * 0.012f;
        display.drawLine(DIAL_CENTER + sinf(a) * DIAL_OUTER, DIAL_CENTER - cosf(a) * DIAL_OUTER,
                         DIAL_CENTER + sinf(a) * 86, DIAL_CENTER - cosf(a) * 86, look.ink);
    }
}

// hollow ring on the rim, so it can't be mistaken for the solid hour dot
// the second hand: a fuzzy dot on the dial, solid in the middle and thinning out to dots at the edge
static void drawSecondMarker(Adafruit_GFX &g, uint8_t second){
    float a = second * 6 * DEG;
    int16_t cx = DIAL_CENTER + sinf(a) * 92, cy = DIAL_CENTER - cosf(a) * 92;
    const int16_t r = SECOND_MARKER_RADIUS;
    for(int16_t dy = -r; dy <= r; dy++){
        for(int16_t dx = -r; dx <= r; dx++){
            int16_t x = cx + dx, y = cy + dy;
            float d = sqrtf(dx * dx + dy * dy) / r;
            if(d > 1 || x < 0 || x >= 200 || y < 0 || y >= 200){
                continue;
            }
            float density = constrain((1 - d) * 2.2f, 0.0f, 1.0f); // solid out to about half the radius
            bool dot = pgm_read_byte(&BlueNoise200[y * 200 + x]) < density * 255;
            g.drawPixel(x, y, dot ? look.ink : look.paper); // paper too, to clear the ticks underneath
        }
    }
}

// how far the watch is tilted left/right (roll) and toward/away (pitch), in degrees
static bool readTilt(float &roll, float &pitch){
    Accel a;
    if(!sensor.getAccel(a)){
        return false;
    }
    // the library remaps the sensor's axes with every sign flipped, so lying face up reads z = -1g
    roll = atan2f(-a.x, -a.z) / DEG;
    pitch = atan2f(-a.y, -a.z) / DEG;
    return true;
}

// which way gravity points in the watch's frame, as a unit vector (the library's axes, so a watch lying
// face up gives (0, 0, -1))
static RTC_DATA_ATTR float restDirection[3] = {0, 0, -1}; // how the watch was lying when it last settled
static RTC_DATA_ATTR float lastFacingUp = 0, lastTurn = 0; // the last raise check, for the diagnostics

static bool readDirection(float out[3]){
    Accel a;
    if(!sensor.getAccel(a)){
        return false;
    }
    float n = sqrtf((float)a.x * a.x + (float)a.y * a.y + (float)a.z * a.z);
    if(n < 100){
        return false; // free fall or a bad reading
    }
    out[0] = a.x / n;
    out[1] = a.y / n;
    out[2] = a.z / n;
    return true;
}

// a few readings averaged, for a steady one-off measurement
static void readTiltAveraged(float &roll, float &pitch){
    roll = pitch = 0;
    uint8_t n = 0;
    for(uint8_t i = 0; i < 4; i++){
        float r, p;
        if(readTilt(r, p)){ roll += r; pitch += p; n++; }
        delay(5);
    }
    if(n){ roll /= n; pitch /= n; }
}

// kept through deep sleep: the pose that counts as straight on (the last one the watch rested in) and
// the head angle on screen
static RTC_DATA_ATTR float neutralRoll = 0, neutralPitch = 0;
static RTC_DATA_ATTR int8_t shownYaw = FACE_YAW_STEPS / 2, shownNod = FACE_PITCH_STEPS / 2;


// The accelerometer is noisy, so readings are smoothed, and the head only moves to the next baked
// angle once the tilt is clearly past the halfway point between them, so it doesn't flicker between two.
struct TiltTracker {
    float roll = 0, pitch = 0; // smoothed, relative to the neutral pose
    int yaw = shownYaw, nod = shownNod;

    // however the watch is held now counts as straight on
    void calibrate(){
        readTiltAveraged(neutralRoll, neutralPitch);
        roll = pitch = 0;
        yaw = FACE_YAW_STEPS / 2;
        nod = FACE_PITCH_STEPS / 2;
    }

    // start from the current tilt, measured carefully, rather than smoothing up to it from zero
    void resume(){
        readTiltAveraged(roll, pitch);
        roll -= neutralRoll;
        pitch -= neutralPitch;
        settleAll();
    }

    void update(){
        float r, p;
        if(!readTilt(r, p)){
            return;
        }
        roll += TILT_SMOOTHING * ((r - neutralRoll) - roll);
        pitch += TILT_SMOOTHING * ((p - neutralPitch) - pitch);
        settleAll();
    }

    void settleAll(){
        yaw = settle(yaw, TILT_YAW_SIGN * TILT_GAIN * roll / FACE_YAW_STEP_DEG + FACE_YAW_STEPS / 2, FACE_YAW_STEPS);
        nod = settle(nod, TILT_PITCH_SIGN * TILT_GAIN * pitch / FACE_PITCH_STEP_DEG + FACE_PITCH_STEPS / 2, FACE_PITCH_STEPS);
    }

    // step index `current` toward continuous position `pos`, only once it's past the hysteresis margin
    static int settle(int current, float pos, int steps){
        while(pos > current + 0.5f + TILT_HYSTERESIS && current < steps - 1) current++;
        while(pos < current - 0.5f - TILT_HYSTERESIS && current > 0) current--;
        return current;
    }

    // the current (smoothed) pose becomes straight on, without moving the head yet
    void recenter(){
        neutralRoll += roll;
        neutralPitch += pitch;
        roll = pitch = 0;
    }

    uint8_t view() const { return nod * FACE_YAW_STEPS + yaw; }

    // remember the angle on screen for the next wake-up
    void save() const { shownYaw = yaw; shownNod = nod; }
};

// a dot in the top-right corner while the watch is awake following the tilt; gone once it sleeps
static void drawAwakeMarker(Adafruit_GFX &g){
    g.fillCircle(186, 14, 5, look.ink);
}

// ---------- waking on motion ----------
// The accelerometer's any-motion interrupt is wired to a pin that can wake the chip, so moving the watch
// wakes it to turn the head, without staying awake. The library's own deep sleep only listens to the
// buttons, so on the watch face the face puts the chip to sleep itself (sleepNow) with that pin added.

static RTC_DATA_ATTR bool motionConfigured = false; // cleared on reset, when the library resets the sensor
static RTC_DATA_ATTR uint8_t motionWakes = 0;       // looks at the watch since the last minute tick
static RTC_DATA_ATTR uint8_t ignoredWakes = 0;      // and motion wakes that weren't a look

#ifndef EMULATOR
static uint16_t bmaRead(uint8_t address, uint8_t reg, uint8_t *data, uint16_t len){
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.endTransmission();
    Wire.requestFrom(address, (uint8_t)len);
    uint16_t i = 0;
    while(Wire.available()){
        data[i++] = Wire.read();
    }
    return 0;
}

static uint16_t bmaWrite(uint8_t address, uint8_t reg, uint8_t *data, uint16_t len){
    Wire.beginTransmission(address);
    Wire.write(reg);
    Wire.write(data, len);
    return Wire.endTransmission() != 0;
}

static void bmaDelay(uint32_t ms){
    delay(ms);
}
#endif

// interrupt on a firm movement, active low so it can share the buttons' "any pin low" wake-up
// what happened with motion wake-up, kept through sleep and printed by Start Upload over USB
static RTC_DATA_ATTR uint16_t motionSetupErrors = 0; // OR of the sensor driver's results, 0 = all fine
static RTC_DATA_ATTR uint32_t sleeps = 0, sleepsArmed = 0, motionWakesTotal = 0;
static RTC_DATA_ATTR uint8_t sleepSkipped = 0; // why the last sleep didn't listen for motion: 1 setup, 2 not on face, 4 limit

#ifndef EMULATOR
// the library keeps its sensor handle private, so talk to the chip through a handle of our own
static bool openMotionDev(struct bma4_dev &dev){
    dev = {};
    dev.dev_addr = BMA4_I2C_ADDR_PRIMARY;
    dev.interface = BMA4_I2C_INTERFACE;
    dev.bus_read = bmaRead;
    dev.bus_write = bmaWrite;
    dev.delay = bmaDelay;
    dev.read_write_len = 8;
    return bma423_init(&dev) == BMA4_OK;
}
#endif

static void configureMotionInterrupt(){
#ifndef EMULATOR
    struct bma4_dev dev;
    if(!openMotionDev(dev)){
        motionSetupErrors = 0xFFFF;
        return;
    }
    uint16_t r = 0;
    // a firm movement wakes the watch, then raisedToLook() decides whether it was a look at the time.
    // (The sensor's own wrist-tilt gesture never fired on the V3, whose sensor sits differently from the
    // older boards the library's axis setup was written for.)
    r |= bma423_map_interrupt(BMA4_INTR1_MAP, BMA423_STEP_CNTR_INT | BMA423_TILT_INT | BMA423_WAKEUP_INT, BMA4_DISABLE, &dev);
    struct bma423_anymotion_config motion = {MOTION_DURATION, MOTION_THRESHOLD, 0};
    r |= bma423_set_any_motion_config(&motion, &dev);
    r |= bma423_anymotion_enable_axis(BMA423_ALL_AXIS_EN, &dev);
    r |= bma423_feature_enable(BMA423_ANY_MOTION, BMA4_ENABLE, &dev);
    r |= bma423_map_interrupt(BMA4_INTR1_MAP, BMA423_ANY_NO_MOTION_INT, BMA4_ENABLE, &dev);

    struct bma4_int_pin_config pin = {};
    pin.edge_ctrl = BMA4_LEVEL_TRIGGER;
    pin.lvl = BMA4_ACTIVE_LOW;
    pin.od = BMA4_PUSH_PULL;
    pin.output_en = BMA4_OUTPUT_ENABLE;
    pin.input_en = BMA4_INPUT_DISABLE;
    r |= bma4_set_int_pin_config(&pin, BMA4_INTR1_MAP, &dev);
    // latched: the pin stays low until the interrupt status is read, so a sleeping chip can't miss it
    r |= bma4_set_interrupt_mode(BMA4_LATCH_MODE, &dev);
    motionSetupErrors = r;
#endif
    motionConfigured = true;
}

// like the library's deepSleep(), but on the watch face it also wakes on motion
void MyFaceWatchy::sleepNow(uint32_t wakeWithin){
    sleeps++;
    // a motion latched while awake wakes us straight back up, once, which then clears it
    sleepSkipped = (motionConfigured ? 0 : 1) | (guiState == WATCHFACE_STATE ? 0 : 2)
                 | (motionWakes < MOTION_WAKES_PER_MINUTE && ignoredWakes < IGNORED_WAKES_PER_MINUTE ? 0 : 4);
    uint64_t wakePins = BTN_PIN_MASK;
    if(!sleepSkipped){
        wakePins |= ACC_INT_MASK;
        sleepsArmed++;
    }

    display.hibernate();
    RTC.clearAlarm();
#ifndef EMULATOR
    esp_sleep_enable_ext0_wakeup((gpio_num_t)USB_DET_PIN, USB_PLUGGED_IN ? LOW : HIGH); // USB plugged in/out
    rtc_gpio_set_direction((gpio_num_t)USB_DET_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en((gpio_num_t)USB_DET_PIN);
    rtc_gpio_set_direction((gpio_num_t)UP_BTN_PIN, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en((gpio_num_t)UP_BTN_PIN);
    rtc_clk_32k_enable(true);
    struct tm timeinfo;
    getLocalTime(&timeinfo);
    uint32_t seconds = 60 - timeinfo.tm_sec; // the next minute tick
    if(wakeWithin && wakeWithin < seconds){
        seconds = wakeWithin; // or sooner, e.g. to put a joke away on time
    }
    esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
#endif
    esp_sleep_enable_ext1_wakeup(wakePins, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

static RTC_DATA_ATTR uint32_t jokeShownAt = 0; // local time the joke went up, 0 if none is showing

// The library's init() ends in its own deep sleep, which only the buttons can wake, so the minute tick
// on the watch face is handled here instead, ending in sleepNow(). Everything else goes to init().
void MyFaceWatchy::start(){
    if(esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER && guiState == WATCHFACE_STATE){
#ifndef EMULATOR
        Wire.begin(WATCHY_V3_SDA, WATCHY_V3_SCL);
#endif
        RTC.init();
        display.epd2.initWatchy();
        RTC.read(currentTime);
        if(jokeShowing()){
            // leave the joke up until its time is over
        }else if(jokeShownAt){
            jokeShownAt = 0;
            showWatchFace(false); // a full refresh, so no ghost of the joke's text stays behind
        }else{
            showWatchFace(true);
        }
        if(settings.vibrateOClock && currentTime.Minute == 0){
            vibMotor(75, 4);
        }
        sleepNow(jokeSecondsLeft());
    }
    init();
}

// ---------- dad jokes: Up on the watch face ----------

bool MyFaceWatchy::jokeShowing(){
    return jokeShownAt && localNow(currentTime) - jokeShownAt < JOKE_SECONDS;
}

// when to wake to put the joke away (a second late, so it's surely over), 0 if none is up
uint32_t MyFaceWatchy::jokeSecondsLeft(){
    return jokeShowing() ? JOKE_SECONDS - (localNow(currentTime) - jokeShownAt) + 1 : 0;
}

// the jokes come as UTF-8 with curly quotes and the like; the fonts only have plain ASCII
static String toAscii(const String &text){
    String out;
    const char *s = text.c_str();
    while(*s){
        uint8_t c = *s;
        if(c < 0x80){
            out += (char)(c == '\r' || c == '\n' || c == '\t' ? ' ' : c);
            s++;
            continue;
        }
        // a multi-byte character: decode it, then map the common punctuation and drop the rest
        int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        uint32_t code = c & (0xFF >> (len + 1));
        for(int i = 1; i < len && s[i]; i++){
            code = (code << 6) | (s[i] & 0x3F);
        }
        s += len;
        if(code == 0x2018 || code == 0x2019) out += '\'';
        else if(code == 0x201C || code == 0x201D) out += '"';
        else if(code == 0x2013 || code == 0x2014) out += '-';
        else if(code == 0x2026) out += "...";
        else if(code == 0x00A0) out += ' ';
    }
    return out;
}

// split text into lines no wider than maxWidth in the given font; returns how many it took
static uint8_t wrapText(Adafruit_GFX &g, const String &text, const GFXfont *font, int16_t maxWidth,
                        String lines[], uint8_t maxLines){
    g.setFont(font);
    uint8_t count = 0;
    String line, word;
    String rest = text + " ";
    for(unsigned int i = 0; i < rest.length(); i++){
        char c = rest.charAt(i);
        if(c != ' '){
            word += c;
            continue;
        }
        if(!word.length()){
            continue;
        }
        String candidate = line.length() ? line + " " + word : word;
        int16_t x1, y1;
        uint16_t w, h;
        g.getTextBounds(candidate, 0, 0, &x1, &y1, &w, &h);
        if(w > maxWidth && line.length()){
            if(count == maxLines) return maxLines + 1; // doesn't fit
            lines[count++] = line;
            line = word;
        }else{
            line = candidate;
        }
        word = "";
    }
    if(line.length()){
        if(count == maxLines) return maxLines + 1;
        lines[count++] = line;
    }
    return count;
}

// a few words in the middle of the screen, in the face's colours
void MyFaceWatchy::showMessage(const char *text){
    prepareLook(currentTime);
    display.setFullWindow();
    display.fillScreen(look.paper);
    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(look.ink);
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((200 - w) / 2 - x1, 100 - h / 2 - y1);
    display.print(text);
    display.display(true);
}

// ask icanhazdadjoke.com for a joke; false if there's no Wi-Fi or no answer
bool MyFaceWatchy::fetchJoke(String &joke){
    if(!connectWiFi()){
        return false;
    }
    WiFiClientSecure client;
    client.setInsecure(); // it's HTTPS; a joke isn't worth carrying the site's certificate around for
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.begin(client, JOKE_URL);
    http.addHeader("Accept", "text/plain");
    http.setUserAgent(JOKE_USER_AGENT); // the site asks API users to say who they are
    bool ok = http.GET() == 200;
    if(ok){
        joke = toAscii(http.getString());
        joke.trim();
    }
    http.end();
    WiFi.mode(WIFI_OFF);
    btStop();
    return ok && joke.length();
}

// fetch a dad joke and show it, word-wrapped, in the largest font it fits in
void MyFaceWatchy::showJoke(){
    RTC.read(currentTime);
    showMessage("Thinking of one...");
    String joke;
    if(!fetchJoke(joke)){
        showMessage("No joke: no Wi-Fi");
        jokeShownAt = localNow(currentTime);
        return;
    }
    static const GFXfont *const FONTS[] = {&FreeSansBold12pt7b, &FreeSansBold9pt7b, &FreeSans9pt7b};
    static const int16_t LINE_HEIGHT[] = {28, 21, 20};
    const uint8_t MAX_LINES = 9;
    String lines[MAX_LINES];
    uint8_t font = 0, count = 0;
    for(; font < 3; font++){
        count = wrapText(display, joke, FONTS[font], JOKE_WIDTH, lines, MAX_LINES);
        if(count <= MAX_LINES && count * LINE_HEIGHT[font] <= JOKE_HEIGHT){
            break;
        }
    }
    if(font == 3){ // too long even in the smallest: show what fits
        font = 2;
        count = min(count, (uint8_t)(JOKE_HEIGHT / LINE_HEIGHT[font]));
    }

    prepareLook(currentTime);
    display.setFullWindow();
    display.fillScreen(look.paper);
    display.setFont(FONTS[font]);
    display.setTextColor(look.ink);
    int16_t top = 100 - count * LINE_HEIGHT[font] / 2;
    for(uint8_t i = 0; i < count; i++){
        int16_t x1, y1;
        uint16_t w, h;
        display.getTextBounds(lines[i], 0, 0, &x1, &y1, &w, &h);
        display.setCursor((200 - w) / 2 - x1, top + LINE_HEIGHT[font] * i + LINE_HEIGHT[font] * 3 / 4);
        display.print(lines[i]);
    }
    display.display(false); // a full refresh, so the face doesn't ghost through the text
    jokeShownAt = localNow(currentTime);
}

// one line of everything motion-related, for reading over USB
static void printMotionDiagnostics(){
#ifndef EMULATOR
    Accel a = {};
    sensor.getAccel(a);
    float roll, pitch;
    readTilt(roll, pitch);
    uint8_t io = 0, latch = 0, map1 = 0, pwrConf = 0, pwrCtrl = 0, feat[4] = {};
    uint16_t status = 0;
    int pinLevel = gpio_get_level((gpio_num_t)ACC_INT_1_PIN); // before reading the status clears the latch
    struct bma4_dev dev;
    bool ok = openMotionDev(dev);
    if(ok){
        bma4_read_regs(0x53, &io, 1, &dev);      // INT1_IO_CTRL
        bma4_read_regs(0x55, &latch, 1, &dev);   // INT_LATCH
        bma4_read_regs(0x56, &map1, 1, &dev);    // INT1_MAP (feature interrupts)
        bma4_read_regs(0x7C, &pwrConf, 1, &dev); // PWR_CONF
        bma4_read_regs(0x7D, &pwrCtrl, 1, &dev); // PWR_CTRL
        bma4_read_regs(BMA4_FEATURE_CONFIG_ADDR, feat, 4, &dev); // any-motion config
        bma423_read_int_status(&status, &dev);
    }
    Serial.printf("accel x=%d y=%d z=%d roll=%.0f pitch=%.0f neutral=%.0f/%.0f view=%d/%d | "
                  "pin=%d dev=%d setupErr=%04x io=%02x latch=%02x map1=%02x pwrConf=%02x pwrCtrl=%02x "
                  "anymo=%02x %02x %02x %02x int=%04x | sleeps=%u armed=%u skipped=%u looks=%u ignored=%u lastUp=%.2f lastTurn=%.0f | "
                  "battery=%.3fV usb=%d chrg=%d\n",
                  a.x, a.y, a.z, roll, pitch, neutralRoll, neutralPitch, shownYaw, shownNod,
                  pinLevel, ok, motionSetupErrors, io, latch, map1, pwrConf, pwrCtrl,
                  feat[0], feat[1], feat[2], feat[3], status, sleeps, sleepsArmed, sleepSkipped, motionWakesTotal, ignoredWakes,
                  lastFacingUp, lastTurn,
                  analogReadMilliVolts(BATT_ADC_PIN) / 1000.0f * ADC_VOLTAGE_DIVIDER, digitalRead(USB_DET_PIN),
                  digitalRead(CHRG_STATUS_PIN));
#endif
}

static RTC_DATA_ATTR bool needsTimeSync = false;

void MyFaceWatchy::requestTimeSync(){
    needsTimeSync = true;
}

void MyFaceWatchy::drawScene(uint8_t view, float extraHours){
    prepareLook(currentTime);
    float hourAngle = ((currentTime.Hour % 12) + currentTime.Minute / 60.0f + extraHours) * 30 * DEG;
    float minuteAngle = currentTime.Minute * 6 * DEG;
    display.fillScreen(look.paper);
    drawFog(display, currentTime.Minute * 60 + currentTime.Second);
    drawPrecipitation(display, currentTime.Minute * 60 + currentTime.Second);
    drawNumbers(hourAngle);
    drawFace(hourAngle, minuteAngle, view);
    drawDial(hourAngle, minuteAngle);
    drawTemperature(hourAngle);
    drawChargingBolt(display);
}

void MyFaceWatchy::drawWatchFace(){
    if(needsTimeSync){
        needsTimeSync = false;
        if(connectWiFi()){
            syncNTP();
            RTC.read(currentTime);
        }
        WiFi.mode(WIFI_OFF);
        btStop();
    }
    if(!motionConfigured){
        configureMotionInterrupt();
    }
#ifndef EMULATOR
    sensor.getINT(); // clear any motion latched since the last wake
#endif
    motionWakes = 0;
    ignoredWakes = 0;
    readDirection(restDirection); // the watch is resting however it is now
    updateConditions();

    // at rest the head faces you, whatever pose the watch is resting in
    TiltTracker tilt;
    tilt.calibrate();
    tilt.save();
    drawScene(tilt.view());
}

// stay awake, ticking the second hand round and turning the head as the watch tilts, redrawing
// whenever either changes (the screen can manage about two updates a second). It opens with the
// key light swinging once round the face, an hour per frame, back to the real time.
bool MyFaceWatchy::playLiveSeconds(){
    TiltTracker tilt;
    tilt.calibrate(); // however the watch is held now counts as straight on, from now on
    uint8_t lastSecond = 255, lastView = 255, orbitFrame = 0;
    unsigned long until = millis() + LIVE_SECONDS * 1000UL;
    pinMode(BACK_BTN_PIN, INPUT);
    pinMode(MENU_BTN_PIN, INPUT);
    bool released = false; // the Back press that started this has to end before Back can stop it
    while(millis() < until){
        bool back = digitalRead(BACK_BTN_PIN) == LOW, menu = digitalRead(MENU_BTN_PIN) == LOW;
        if(!back && !menu){
            released = true;
        }else if(released){
            tilt.recenter();
            tilt.yaw = FACE_YAW_STEPS / 2;
            tilt.nod = FACE_PITCH_STEPS / 2;
            tilt.save();
            while(digitalRead(BACK_BTN_PIN) == LOW || digitalRead(MENU_BTN_PIN) == LOW){
                delay(10); // let go first, so the menu doesn't take this press as a selection
            }
            return !menu;
        }
        RTC.read(currentTime);
        tilt.update();
        uint8_t view = tilt.view();
        bool orbiting = orbitFrame < ORBIT_FRAMES;
        if(!orbiting && currentTime.Second == lastSecond && view == lastView){
            delay(20);
            continue;
        }
        lastSecond = currentTime.Second;
        lastView = view;
        drawScene(view, orbiting ? 12.0f * ++orbitFrame / ORBIT_FRAMES : 0);
        drawSecondMarker(display, currentTime.Second);
        drawAwakeMarker(display);
        display.display(true);
    }
    easeToCenter(tilt);
    return true;
}

// After a motion wake: was that the watch being raised to look at it? It has to come round to face up
// (toward you) within RAISE_WINDOW_MS, having turned well away from how it was resting, and hold there.
// Walking (arm hanging, screen sideways) and typing (already face up, barely turning) don't pass.
bool MyFaceWatchy::raisedToLook(){
    float d[3] = {restDirection[0], restDirection[1], restDirection[2]};
    unsigned long start = millis(), heldSince = 0;
    bool raised = false;
    while(millis() - start < RAISE_WINDOW_MS){
        if(readDirection(d)){
            lastFacingUp = -d[2]; // the library reports a face-up watch as z = -1g
            float dot = d[0] * restDirection[0] + d[1] * restDirection[1] + d[2] * restDirection[2];
            lastTurn = acosf(constrain(dot, -1.0f, 1.0f)) / DEG;
            if(lastFacingUp >= RAISE_FACING_UP && lastTurn >= RAISE_MIN_TURN_DEG){
                if(!heldSince) heldSince = millis();
                if(millis() - heldSince >= RAISE_HOLD_MS){
                    raised = true;
                    break;
                }
            }else{
                heldSince = 0;
            }
        }
        delay(20);
    }
    if(!raised){
        memcpy(restDirection, d, sizeof(d)); // not a look: this is just where it rests now
    }
    return raised;
}

// raised to look: follow the tilt until the watch settles, then go back to sleep
void MyFaceWatchy::followMotion(){
    motionWakes++;
    motionWakesTotal++;
    RTC.read(currentTime);
    TiltTracker tilt;
    tilt.resume();
    uint8_t shown = tilt.view();
    drawScene(shown);
    drawAwakeMarker(display);
    display.display(true);
    unsigned long start = millis(), lastChange = start;
    while(millis() - lastChange < MOTION_SETTLE_MS && millis() - start < MOTION_FOLLOW_MS){
        tilt.update();
        if(tilt.view() != shown){
            shown = tilt.view();
            drawScene(shown);
            drawAwakeMarker(display);
            display.display(true);
            lastChange = millis();
        }else{
            delay(20);
        }
    }
    easeToCenter(tilt);
    drawScene(tilt.view()); // and once more without the dot, as it goes back to sleep
    display.display(true);
    readDirection(restDirection); // lowering the arm from here and raising it again counts as a new look
}

// once the watch is still, it becomes straight on and the head turns back to face you a few steps
// at a time (the last step is left to the caller, which draws it without the awake dot)
void MyFaceWatchy::easeToCenter(TiltTracker &tilt){
    tilt.recenter();
    const int midYaw = FACE_YAW_STEPS / 2, midNod = FACE_PITCH_STEPS / 2;
    while(abs(tilt.yaw - midYaw) > EASE_STEP || abs(tilt.nod - midNod) > EASE_STEP){
        tilt.yaw += constrain(midYaw - tilt.yaw, -EASE_STEP, EASE_STEP);
        tilt.nod += constrain(midNod - tilt.nod, -EASE_STEP, EASE_STEP);
        drawScene(tilt.view());
        drawAwakeMarker(display);
        display.display(true);
    }
    tilt.yaw = midYaw;
    tilt.nod = midNod;
    tilt.save();
}

// ---------- buttons and menu ----------
// The library's menu is a fixed list, so the face runs its own copy of it with an extra item.

extern RTC_DATA_ATTR bool alreadyInMenu; // Watchy.cpp: the menu closes on the second minute tick

static const char *const MENU_ITEMS[] = {"About Watchy", "Vibrate Motor", "Show Accelerometer",
                                         "Set Time", "Setup WiFi", "Sync NTP", "Weather", "Start Upload"};
#define MENU_COUNT ((int)(sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0])))
#define MENU_WEATHER 6
#define MENU_START_UPLOAD 7 // last, so Up from the top of the menu lands on it
#define MENU_ROW 23         // the library's 25px rows don't fit eight items

void MyFaceWatchy::drawMenu(bool partialRefresh){
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    for(int i = 0; i < MENU_COUNT; i++){
        int16_t yPos = 19 + MENU_ROW * i, x1, y1;
        uint16_t w, h;
        display.setCursor(0, yPos);
        if(i == menuIndex){
            display.getTextBounds(MENU_ITEMS[i], 0, yPos, &x1, &y1, &w, &h);
            display.fillRect(x1 - 1, y1 - 5, 200, h + 10, GxEPD_WHITE);
            display.setTextColor(GxEPD_BLACK);
        }else{
            display.setTextColor(GxEPD_WHITE);
        }
        display.println(MENU_ITEMS[i]);
    }
    display.display(partialRefresh);
    guiState = MAIN_MENU_STATE;
    alreadyInMenu = false;
}

void MyFaceWatchy::openMenuItem(){
    switch(menuIndex){
        case 0: showAbout(); break;
        case 1: showBuzz(); break;
        case 2: showAccelerometer(); break;
        case 3: setTime(); break;
        case 4: setupWifi(); break;
        case 5: showSyncNTP(); break;
        case MENU_WEATHER: showWeather(); break;
        case MENU_START_UPLOAD: waitForUpload(); break;
    }
}

// the weather report in words and numbers; Menu fetches a fresh one, Back returns to the menu
void MyFaceWatchy::showWeather(){
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(0, 14);
    if(!weather.known){
        display.println("No weather yet");
        display.println();
        display.println("Needs Wi-Fi and");
        display.println("an API key.");
        display.println();
        display.println("Menu: try now");
    }else{
        const char *temp = weather.metric ? "C" : "F", *speed = weather.metric ? "m/s" : "mph";
        static const char *const COMPASS[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        char line[40];
        display.println(weather.place);
        // the summary can be long: wrap it at 16 characters a line, two lines at most
        String summary = weather.summary;
        for(uint8_t row = 0; row < 2 && summary.length(); row++){
            int cut = summary.length() <= 16 ? summary.length() : summary.lastIndexOf(' ', 16);
            if(cut <= 0) cut = min((int)summary.length(), 16);
            display.println(summary.substring(0, cut));
            summary = summary.substring(cut);
            summary.trim();
        }
        snprintf(line, sizeof(line), "%d%s feels %d%s", weather.temperature, temp, weather.feelsLike, temp);
        display.println(line);
        snprintf(line, sizeof(line), "Humidity %d%%", weather.humidity);
        display.println(line);
        snprintf(line, sizeof(line), "Clouds %d%%", weather.clouds);
        display.println(line);
        snprintf(line, sizeof(line), "Wind %.0f%s %s", weather.wind, speed, COMPASS[((weather.windFrom + 22) % 360) / 45]);
        display.println(line);
        if(weather.rain > 0 || weather.snow > 0){
            snprintf(line, sizeof(line), "%s %.1fmm/h", weather.snow > 0 ? "Snow" : "Rain", weather.snow > 0 ? weather.snow : weather.rain);
            display.println(line);
        }
        if(weather.metric){
            snprintf(line, sizeof(line), "Visib. %.1fkm", weather.visibility / 1000.0f);
        }else{
            snprintf(line, sizeof(line), "Visib. %.1fmi", weather.visibility / 1609.0f);
        }
        display.println(line);
        snprintf(line, sizeof(line), "Sun %d:%02d-%d:%02d", weather.sunrise / 60, weather.sunrise % 60,
                 weather.sunset / 60, weather.sunset % 60);
        display.println(line);
        tmElements_t at;
        breakTime(weather.fetchedAt, at);
        snprintf(line, sizeof(line), "Updated %d:%02d", at.Hour, at.Minute);
        display.println(line);
    }
    display.display(false);
    guiState = APP_STATE;
}

// one button press in the menu or an app; false once it's back on the watch face
bool MyFaceWatchy::menuPress(uint64_t button){
    if(button & MENU_BTN_MASK){
        if(guiState == MAIN_MENU_STATE){
            openMenuItem();
        }else if(menuIndex == MENU_WEATHER){
            // fetch a fresh report right now
            display.setFont(&FreeMonoBold9pt7b);
            display.fillRect(0, 186, 200, 14, GxEPD_WHITE);
            display.setTextColor(GxEPD_BLACK);
            display.setCursor(2, 197);
            display.print("Updating...");
            display.display(true);
            RTC.read(currentTime);
            fetchWeather();
            showWeather();
        }
    }else if(button & BACK_BTN_MASK){
        if(guiState == MAIN_MENU_STATE){
            RTC.read(currentTime);
            showWatchFace(false);
        }else{
            drawMenu(false); // back out of an app
        }
    }else if(button & (UP_BTN_MASK | DOWN_BTN_MASK)){
        if(guiState == MAIN_MENU_STATE){
            menuIndex = (menuIndex + ((button & UP_BTN_MASK) ? MENU_COUNT - 1 : 1)) % MENU_COUNT;
            drawMenu(true);
        }
    }
    return guiState != WATCHFACE_STATE;
}

void MyFaceWatchy::handleButtonPress(){
    uint64_t wakeupBit = esp_sleep_get_ext1_wakeup_status();
    if(guiState == WATCHFACE_STATE){
        RTC.read(currentTime);
        bool joke = jokeShowing();
        uint32_t jokeSince = jokeShownAt;
        jokeShownAt = 0; // any button puts the joke away
        if(wakeupBit & MENU_BTN_MASK){
            menuIndex = constrain(menuIndex, 0, MENU_COUNT - 1);
            drawMenu(false);
        }else if(wakeupBit & UP_BTN_MASK){
            showJoke();
            sleepNow(jokeSecondsLeft());
        }else if(joke && (wakeupBit & ACC_INT_MASK)){
            jokeShownAt = jokeSince; // keep it up: raising the watch to read it shouldn't wipe it
            sleepNow(jokeSecondsLeft());
        }else if(joke){
            RTC.read(currentTime);
            showWatchFace(false); // Back or Down: straight back to the face, fully redrawn so no text ghosts
            sleepNow();
        }else if(wakeupBit & BACK_BTN_MASK){
            // a minute of seconds with the head following the tilt, opening with the light swinging round;
            // Back again ends it early, Menu ends it and opens the menu
            if(playLiveSeconds()){
                RTC.read(currentTime);
                showWatchFace(true);
                sleepNow();
            }
            menuIndex = constrain(menuIndex, 0, MENU_COUNT - 1);
            drawMenu(false); // then on to the menu's button loop below
        }else if(wakeupBit & ACC_INT_MASK){
#ifndef EMULATOR
            sensor.getINT(); // clear the latched motion
#endif
            if(raisedToLook()){
                followMotion();
            }else{
                ignoredWakes++; // back to sleep without touching the screen
            }
#ifndef EMULATOR
            sensor.getINT(); // movement during all that shouldn't wake us straight back up
#endif
            sleepNow();
        }else{
            sleepNow(); // Down is free for now
        }
    }else if(!menuPress(wakeupBit)){
        sleepNow();
    }

#ifndef EMULATOR
    // keep handling presses while awake, like the library's fast menu, until 5s without one
    pinMode(MENU_BTN_PIN, INPUT);
    pinMode(BACK_BTN_PIN, INPUT);
    pinMode(UP_BTN_PIN, INPUT);
    pinMode(DOWN_BTN_PIN, INPUT);
    unsigned long lastPress = millis();
    while(millis() - lastPress < 5000){
        uint64_t button = digitalRead(MENU_BTN_PIN) == LOW ? MENU_BTN_MASK
                        : digitalRead(BACK_BTN_PIN) == LOW ? BACK_BTN_MASK
                        : digitalRead(UP_BTN_PIN) == LOW ? UP_BTN_MASK
                        : digitalRead(DOWN_BTN_PIN) == LOW ? DOWN_BTN_MASK : 0;
        if(!button){
            continue;
        }
        lastPress = millis();
        if(!menuPress(button)){
            break;
        }
    }
#endif
    sleepNow();
}

#define UPLOAD_WAIT_MS 60000

void MyFaceWatchy::waitForUpload(){
    display.setFullWindow();
    display.fillScreen(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(4, 90);
    display.print("READY FOR UPLOAD");
    display.setCursor(4, 120);
    display.print("for 60 seconds");
    display.display(false);

    // the USB port only exists while the chip is awake; the uploader resets us when it starts.
    // Meanwhile report what the motion sensor sees, for anyone listening on the port.
#ifndef EMULATOR
    Serial.begin(115200);
    pinMode(USB_DET_PIN, INPUT);
    pinMode(CHRG_STATUS_PIN, INPUT);
#endif
    for(unsigned long start = millis(); millis() - start < UPLOAD_WAIT_MS;){
        printMotionDiagnostics();
        delay(250);
    }

    RTC.read(currentTime);
    showWatchFace(false);
}
