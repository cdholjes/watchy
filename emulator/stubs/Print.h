#ifndef EMU_PRINT_H
#define EMU_PRINT_H

#include <stddef.h>
#include <stdint.h>

class String;
class Print;

// something that can print itself (Arduino's Printable)
class Printable {
public:
    virtual ~Printable() {}
    virtual size_t printTo(Print &p) const = 0;
};

class Print {
public:
    virtual ~Print() {}
    virtual size_t write(uint8_t c) = 0;
    virtual size_t write(const uint8_t *buf, size_t size);
    size_t write(const char *str);
    size_t write(const char *buf, size_t size) { return write((const uint8_t *)buf, size); }

    size_t print(const char *s);
    size_t print(const String &s);
    size_t print(char c);
    size_t print(int v, int base = 10);
    size_t print(unsigned int v, int base = 10);
    size_t print(long v, int base = 10);
    size_t print(unsigned long v, int base = 10);
    size_t print(long long v, int base = 10);
    size_t print(unsigned long long v, int base = 10);
    size_t print(double v, int digits = 2);

    size_t println();
    template <typename T> size_t println(const T &v) { size_t n = print(v); return n + println(); }
    template <typename T> size_t println(const T &v, int fmt) { size_t n = print(v, fmt); return n + println(); }
};

#endif
