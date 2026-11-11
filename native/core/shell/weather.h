// The weather for the weather widget: Open-Meteo's forecast (no account), where the computer is:
// Windows' location service when it lets desktop apps use it (the place named by BigDataCloud's
// free reverse geocoding), else where the network connection is (ip-api.com, asked directly:
// through a proxy it would answer for the proxy's place). Fetched on a worker thread every half hour while a
// weather widget is on the desktop, and kept in %LOCALAPPDATA%\Atrium\weather.json, which the next
// start shows at once and the settings app's previews read.
#pragma once

#include <windows.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace atrium {

struct WeatherHour {
    std::wstring label; // "现在", "14时", or the time of a sunrise / sunset
    int code = 0;       // WMO weather code
    bool day = true;
    float temp = 0;     // °C
    int event = 0;      // 1 sunrise, 2 sunset (code and temp unused)
};

struct WeatherDay {
    std::wstring label; // "今天", "周二"
    int code = 0;
    float lo = 0, hi = 0;
};

struct Weather {
    bool ok = false;
    std::wstring city;
    float temp = 0, hi = 0, lo = 0;
    int code = 0;
    bool day = true;
    std::vector<WeatherHour> hours; // the next six (sunrise and sunset among them)
    std::vector<WeatherDay> days;   // today and the next days
    std::wstring error;             // why there is none (shown while !ok)
};

// "晴", "多云", "小雨"…
std::wstring WeatherCondition(int code);
std::wstring WeatherCachePath();

class WeatherSource {
public:
    // `message` is posted to `notify` with a new Latest().
    WeatherSource(HWND notify, UINT message);
    ~WeatherSource();
    WeatherSource(const WeatherSource&) = delete;
    WeatherSource& operator=(const WeatherSource&) = delete;

    void SetActive(bool on);                // fetch only while a weather widget shows
    void Refresh();                         // the labels are language-specific: fetch again after a language change
    Weather Latest();

private:
    void Run();
    bool Fetch();

    HWND notify_;
    UINT message_;
    std::mutex lock_;
    std::condition_variable wake_;
    bool active_ = false, quit_ = false, dirty_ = true;
    Weather latest_;
    std::thread thread_;
};

} // namespace atrium
