// Arduino core functions backed by the emulator's virtual clock.
#include <Arduino.h>
#include <stdio.h>
#include <unistd.h>
#include "emu.h"

static uint64_t rngState = 0x9E3779B97F4A7C15ULL;

unsigned long millis() { return (unsigned long)emu.ms; }
unsigned long micros() { return (unsigned long)(emu.ms * 1000); }
void delay(unsigned long ms) {
    emu.ms += ms;
    if (emu.realtime) usleep(ms * 1000);
}

void randomSeed(unsigned long seed) { rngState = seed ? seed : 1; }

static uint32_t nextRandom() {
    // xorshift64*
    rngState ^= rngState >> 12;
    rngState ^= rngState << 25;
    rngState ^= rngState >> 27;
    return (uint32_t)((rngState * 0x2545F4914F6CDD1DULL) >> 32);
}

long random(long max) { return max <= 0 ? 0 : (long)(nextRandom() % (uint32_t)max); }
long random(long min, long max) { return max <= min ? min : min + random(max - min); }

String::String(float v, int digits) { char b[32]; snprintf(b, sizeof(b), "%.*f", digits, (double)v); s_ = b; }
String::String(double v, int digits) { char b[32]; snprintf(b, sizeof(b), "%.*f", digits, v); s_ = b; }

size_t Print::write(const uint8_t *buf, size_t size) {
    size_t n = 0;
    while (size--) n += write(*buf++);
    return n;
}
size_t Print::write(const char *str) { return str ? write((const uint8_t *)str, strlen(str)) : 0; }

static size_t printNumber(Print &p, unsigned long long v, int base, bool negative) {
    char buf[72];
    char *s = &buf[sizeof(buf) - 1];
    *s = 0;
    if (base < 2) base = 10;
    do {
        int d = v % base;
        *--s = d < 10 ? '0' + d : 'A' + d - 10;
        v /= base;
    } while (v);
    if (negative) *--s = '-';
    return p.write(s);
}

size_t Print::print(const char *s) { return write(s); }
size_t Print::print(const String &s) { return write(s.c_str()); }
size_t Print::print(char c) { return write((uint8_t)c); }
size_t Print::print(int v, int base) { return print((long long)v, base); }
size_t Print::print(unsigned int v, int base) { return print((unsigned long long)v, base); }
size_t Print::print(long v, int base) { return print((long long)v, base); }
size_t Print::print(unsigned long v, int base) { return print((unsigned long long)v, base); }
size_t Print::print(long long v, int base) {
    if (base == 10 && v < 0) return printNumber(*this, (unsigned long long)(-v), 10, true);
    return printNumber(*this, (unsigned long long)v, base, false);
}
size_t Print::print(unsigned long long v, int base) { return printNumber(*this, v, base, false); }
size_t Print::print(double v, int digits) { char b[48]; snprintf(b, sizeof(b), "%.*f", digits, v); return write(b); }
size_t Print::println() { return write("\r\n"); }
