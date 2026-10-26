// Emulated ESP32 HTTPClient: real requests through the Mac's curl (see core/emu_net.cpp), so a face's
// weather or joke code can be tried against the live services. Only what the faces use: a GET with headers.
#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <string>
#include <vector>

class HTTPClient {
public:
    void setConnectTimeout(int32_t ms) { timeoutMs_ = ms; }
    bool begin(const String &url) { url_ = url; body_ = ""; headers_.clear(); return true; }
    bool begin(WiFiClientSecure &, const String &url) { return begin(url); }
    void addHeader(const String &name, const String &value) { headers_.push_back(std::string(name.c_str()) + ": " + value.c_str()); }
    void setUserAgent(const String &agent) { addHeader("User-Agent", agent); }
    int GET(); // the HTTP status, or a negative number if the request couldn't be made
    String getString() { return body_; }
    void end() {}

private:
    String url_, body_;
    std::vector<std::string> headers_;
    int32_t timeoutMs_ = 5000;
};

#endif
