// Minimal Arduino core for running Watchy faces on a desktop.
#ifndef EMU_ARDUINO_H
#define EMU_ARDUINO_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <cmath>
#include <algorithm>
#include <string>

#include "pgmspace.h"

typedef uint8_t byte;
typedef bool boolean;

class __FlashStringHelper;
#define F(s) (s)
#define radians(deg) ((deg) * DEG_TO_RAD)
#define degrees(rad) ((rad) * RAD_TO_DEG)
#define BIT64(n) (1ULL << (n))
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))

using std::min;
using std::max;
using std::abs; // like the ESP32 core, so abs() works on floating point too

#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105

unsigned long millis();
#define INPUT 0x01
#define LOW 0
#define HIGH 1
inline void pinMode(int, int) {}
inline int digitalRead(int) { return HIGH; } // buttons read as released
unsigned long micros();
void delay(unsigned long ms);
long random(long max);
long random(long min, long max);
void randomSeed(unsigned long seed);

class String {
public:
    String() {}
    String(const char *s) : s_(s ? s : "") {}
    String(const std::string &s) : s_(s) {}
    String(char c) : s_(1, c) {}
    String(int v) : s_(std::to_string(v)) {}
    String(unsigned int v) : s_(std::to_string(v)) {}
    String(long v) : s_(std::to_string(v)) {}
    String(unsigned long v) : s_(std::to_string(v)) {}
    String(float v, int digits = 2);
    String(double v, int digits = 2);

    const char *c_str() const { return s_.c_str(); }
    unsigned int length() const { return s_.length(); }
    char charAt(unsigned int i) const { return i < s_.size() ? s_[i] : 0; }
    char operator[](unsigned int i) const { return charAt(i); }
    int toInt() const { return atoi(s_.c_str()); }
    float toFloat() const { return atof(s_.c_str()); }
    void toCharArray(char *buf, unsigned int size) const {
        if (!size) return;
        strncpy(buf, s_.c_str(), size - 1);
        buf[size - 1] = 0;
    }
    String substring(unsigned int from) const { return from < s_.size() ? String(s_.substr(from)) : String(); }
    String substring(unsigned int from, unsigned int to) const { return from < s_.size() ? String(s_.substr(from, to - from)) : String(); }
    int indexOf(char c) const { auto p = s_.find(c); return p == std::string::npos ? -1 : (int)p; }
    int lastIndexOf(char c, unsigned int from) const { auto p = s_.rfind(c, from); return p == std::string::npos ? -1 : (int)p; }
    void trim() {
        size_t b = s_.find_first_not_of(" \t\r\n"), e = s_.find_last_not_of(" \t\r\n");
        s_ = b == std::string::npos ? "" : s_.substr(b, e - b + 1);
    }
    void replace(const String &from, const String &to) {
        if (from.s_.empty()) return;
        for (size_t p = 0; (p = s_.find(from.s_, p)) != std::string::npos; p += to.s_.size()) s_.replace(p, from.s_.size(), to.s_);
    }
    void toUpperCase() { for (auto &c : s_) c = toupper(c); }
    void toLowerCase() { for (auto &c : s_) c = tolower(c); }
    bool equals(const String &o) const { return s_ == o.s_; }

    String &operator+=(const String &o) { s_ += o.s_; return *this; }
    String &operator+=(const char *o) { s_ += o; return *this; }
    String &operator+=(char c) { s_ += c; return *this; }
    friend String operator+(const String &a, const String &b) { return String(a.s_ + b.s_); }
    friend String operator+(const String &a, const char *b) { return String(a.s_ + b); }
    friend String operator+(const char *a, const String &b) { return String(a + b.s_); }
    bool operator==(const String &o) const { return s_ == o.s_; }
    bool operator==(const char *o) const { return s_ == o; }
    bool operator!=(const String &o) const { return s_ != o.s_; }

private:
    std::string s_;
};

#include "Print.h"

#endif
