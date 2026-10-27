#include "settings.h"
#include "settings_storage.h"
#include <cJSON.h>
#include <mutex>
#include <cstring>
#include <esp_log.h>
extern "C" {
#include "bflb_mtd.h"
#include "lfs_kv.h"
}
namespace {
std::recursive_mutex lock;
bool ready = false, healthy = true;
std::string Key(const std::string& ns) { return "xz." + ns; }
void Failed() { healthy = false; ESP_LOGE("Settings", "Persistent settings failed; credentials are not logged"); }
}
bool InitializeSettingsStorage() {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (ready) return true;
    bflb_mtd_init();
    ready = lfs_kv_init() == LFS_KV_OK;
    if (!ready) Failed();
    return ready;
}
bool SettingsStorageHealthy() { return ready && healthy; }
Settings::Settings(const std::string& ns, bool writable) : ns_(ns), read_write_(writable) {
    lock.lock();  // Serialize each namespace read/modify/commit transaction.
    if (!ready || ns.empty() || ns.size() > 24 || ns.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) {
        Failed(); return;
    }
    size_t size = 0;
    lfs_kv_get_blob(Key(ns).c_str(), nullptr, 0, &size);
    if (size > 4096) { Failed(); return; }
    if (size) {
        std::string data(size, '\0');
        if (lfs_kv_get_blob(Key(ns).c_str(), data.data(), size, nullptr) != size) { Failed(); return; }
        document_ = cJSON_ParseWithLength(data.data(), size);
        if (!cJSON_IsObject(document_)) { cJSON_Delete(document_); document_ = nullptr; Failed(); return; }
    } else document_ = cJSON_CreateObject();
    if (!document_) Failed();
}
Settings::~Settings() {
    if (document_ && read_write_ && dirty_ && healthy) {
        char* text = cJSON_PrintUnformatted(document_);
        if (!text || std::strlen(text) >= 4096 ||
            lfs_kv_set_blob(Key(ns_).c_str(), text, std::strlen(text) + 1) != LFS_KV_OK) Failed();
        cJSON_free(text);
    }
    cJSON_Delete(document_);
    lock.unlock();
}
std::string Settings::GetString(const std::string& key, const std::string& fallback) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(document_, key.c_str());
    return cJSON_IsString(value) ? std::string(value->valuestring) : fallback;
}
int32_t Settings::GetInt(const std::string& key, int32_t fallback) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(document_, key.c_str());
    return cJSON_IsNumber(value) ? value->valueint : fallback;
}
bool Settings::GetBool(const std::string& key, bool fallback) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(document_, key.c_str());
    return cJSON_IsBool(value) ? cJSON_IsTrue(value) : fallback;
}
void Settings::EraseKey(const std::string& key) {
    if (document_ && read_write_) { cJSON_DeleteItemFromObjectCaseSensitive(document_, key.c_str()); dirty_ = true; }
}
void Settings::SetString(const std::string& key, const std::string& value) {
    if (!document_ || !read_write_) return;
    EraseKey(key);
    if (!cJSON_AddStringToObject(document_, key.c_str(), value.c_str())) Failed();
}
void Settings::SetInt(const std::string& key, int32_t value) {
    if (!document_ || !read_write_) return;
    EraseKey(key);
    if (!cJSON_AddNumberToObject(document_, key.c_str(), value)) Failed();
}
void Settings::SetBool(const std::string& key, bool value) {
    if (!document_ || !read_write_) return;
    EraseKey(key);
    if (!cJSON_AddBoolToObject(document_, key.c_str(), value)) Failed();
}
void Settings::EraseAll() {
    if (!read_write_ || !document_) return;
    cJSON_Delete(document_); document_ = cJSON_CreateObject(); dirty_ = true;
    if (!document_) Failed();
}
