// Emulated WiFiClientSecure: HTTPS goes through the Mac's curl, so there's nothing to set up here.
#ifndef WIFI_CLIENT_SECURE_H
#define WIFI_CLIENT_SECURE_H

class WiFiClientSecure {
public:
    void setInsecure() {}
};

#endif
