#ifndef SETTINGS_H
#define SETTINGS_H

#include <string>
#include <cstdint>
#ifndef XIAOZHI_PLATFORM_BOUFFALO
#include <nvs_flash.h>
#else
struct cJSON;
#endif

class Settings {
public:
    Settings(const std::string& ns, bool read_write = false);
    ~Settings();
#ifdef XIAOZHI_PLATFORM_BOUFFALO
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;
#endif

    std::string GetString(const std::string& key, const std::string& default_value = "");
    void SetString(const std::string& key, const std::string& value);
    int32_t GetInt(const std::string& key, int32_t default_value = 0);
    void SetInt(const std::string& key, int32_t value);
    bool GetBool(const std::string& key, bool default_value = false);
    void SetBool(const std::string& key, bool value);
    void EraseKey(const std::string& key);
    void EraseAll();

private:
    std::string ns_;
#ifndef XIAOZHI_PLATFORM_BOUFFALO
    nvs_handle_t nvs_handle_ = 0;
#else
    cJSON* document_ = nullptr;
#endif
    bool read_write_ = false;
    bool dirty_ = false;
};

#endif
