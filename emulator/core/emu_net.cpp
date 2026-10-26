// HTTP for the emulator: GET requests go out through curl, and answers are cached on disk for a few
// minutes so a face that asks on every redraw doesn't use up a free API plan.
#include <HTTPClient.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <functional>
#include <string>
#include "emu.h"

#define HTTP_CACHE_SECONDS 600

static std::string cachePath(const std::string &url) {
    if (emu.httpCacheDir.empty()) return "";
    char name[32];
    snprintf(name, sizeof(name), "/%016zx.txt", std::hash<std::string>{}(url));
    return emu.httpCacheDir + name;
}

// a cache file is "<status>\n<body>"
static bool readCache(const std::string &path, int &status, std::string &body) {
    struct stat st;
    if (path.empty() || stat(path.c_str(), &st) != 0 || time(nullptr) - st.st_mtime > HTTP_CACHE_SECONDS) return false;
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return false;
    if (fscanf(f, "%d\n", &status) != 1) { fclose(f); return false; }
    char buf[4096];
    size_t n;
    body.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
    fclose(f);
    return true;
}

static void writeCache(const std::string &path, int status, const std::string &body) {
    if (path.empty()) return;
    mkdir(emu.httpCacheDir.c_str(), 0755);
    if (FILE *f = fopen(path.c_str(), "w")) {
        fprintf(f, "%d\n", status);
        fwrite(body.data(), 1, body.size(), f);
        fclose(f);
    }
}

int HTTPClient::GET() {
    if (!emu.wifiAvailable) return -1;
    std::string url = url_.c_str();
    std::string headers;
    for (auto &h : headers_) {
        if (h.find('\'') != std::string::npos) return -1;
        headers += " -H '" + h + "'";
    }
    if (url.find('\'') != std::string::npos) return -1; // these go into a shell command in single quotes

    int status = 0;
    std::string body;
    // jokes should differ each time; only cache answers from APIs with keys (the weather)
    std::string cache = url.find("appid=") != std::string::npos ? cachePath(url) : "";
    if (readCache(cache, status, body)) {
        emuLog("HTTP " + std::to_string(status) + " (cached)");
    } else {
        // curl prints the body, then the status code on a line of its own
        std::string cmd = "curl -s -m " + std::to_string(timeoutMs_ / 1000 + 5) + " -w '\\n%{http_code}'" + headers + " '" + url + "'";
        FILE *p = popen(cmd.c_str(), "r");
        if (!p) return -1;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), p)) > 0) body.append(buf, n);
        pclose(p);
        size_t nl = body.rfind('\n');
        status = nl == std::string::npos ? 0 : atoi(body.c_str() + nl + 1);
        body = nl == std::string::npos ? "" : body.substr(0, nl);
        if (status <= 0) return -1;
        writeCache(cache, status, body);
        emuLog("HTTP " + std::to_string(status) + " (live)");
    }
    emu.ms += 400; // roughly what a request takes over the watch's Wi-Fi
    body_ = body.c_str();
    return status;
}
