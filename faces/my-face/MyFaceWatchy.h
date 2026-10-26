#ifndef MY_FACE_WATCHY_H
#define MY_FACE_WATCHY_H

#include <Watchy.h>
#include "face_normals.h"
#include "BlueNoise200.h"

struct TiltTracker; // MyFaceWatchy.cpp

class MyFaceWatchy : public Watchy{
    using Watchy::Watchy;
    public:
        void drawWatchFace();
        void handleButtonPress();
        void requestTimeSync();
        void start();
    private:
        void sleepNow(uint32_t wakeWithin = 0); // seconds; 0 = at the next minute tick
        void updateConditions();
        bool fetchWeather();
        bool askWeatherService();
#ifdef EMULATOR
        bool readWeatherPanel();
#endif
        void showWeather();
        void drawTemperature(float hourAngle);
        bool jokeShowing();
        uint32_t jokeSecondsLeft();
        bool fetchJoke(String &joke);
        void showJoke();
        void showMessage(const char *text);
        void drawScene(uint8_t view, float extraHours = 0);
        void drawFace(float hourAngle, float minuteAngle, uint8_t view);
        void drawDial(float hourAngle, float minuteAngle);
        void drawNumbers(float hourAngle);
        bool playLiveSeconds(); // false if Menu cut it short
        bool raisedToLook();
        void followMotion();
        void easeToCenter(TiltTracker &tilt);
        void waitForUpload();
        void drawMenu(bool partialRefresh);
        void openMenuItem();
        bool menuPress(uint64_t button);
};

#endif
