#include "board.h"
#include "http_bouffalo.h"
#include "audio_codec_bouffalo.h"
#include "device_identity.h"
#include "system_info.h"
#include "ota_port_config.h"
#include "wifi_init.h"
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#include "wifi_config.example.h"
#endif
#include <cstdio>
#include <esp_log.h>

namespace { constexpr char kBoard[] = "bouffalo-bl616cl-es8388"; }
Board::Board() : uuid_(GenerateUuid()) {}
std::string Board::GenerateUuid() {
    char mac[18] = {}, uuid[37] = {};
    return ReadIdentity(mac, uuid) ? uuid : "";
}
Led* Board::GetLed() { return nullptr; }
Display* Board::GetDisplay() { return nullptr; }
Camera* Board::GetCamera() { return nullptr; }
bool Board::GetTemperature(float&) { return false; }
bool Board::GetBatteryLevel(int&, bool&, bool&) { return false; }
std::string Board::GetSystemInfoJson() {
    return "{\"version\":2,\"language\":\"zh-CN\",\"mac_address\":\"" + SystemInfo::GetMacAddress() +
        "\",\"uuid\":\"" + GetUuid() + "\",\"chip_model_name\":\"bl616cl\",\"application\":{\"name\":\"xiaozhi-bouffalo\",\"version\":\"" +
        BouffaloApplicationVersion() + "\"},\"board\":" + GetBoardJson() + "}";
}
class BouffaloBoard final : public Board {
public:
    std::string GetBoardType() override { return kBoard; }
    AudioCodec* GetAudioCodec() override { return &audio_; }
    NetworkInterface* GetNetwork() override { return &network_; }
    void StartNetwork() override { StartWifiInitialization(XIAOZHI_WIFI_SSID, XIAOZHI_WIFI_PASSWORD); }
    const char* GetNetworkStateIcon() override { return ""; }
    void SetPowerSaveLevel(PowerSaveLevel) override { ESP_LOGW("Board", "Power-save adaptation is not enabled yet"); }
    std::string GetBoardJson() override { return std::string("{\"type\":\"") + kBoard + "\",\"name\":\"" + kBoard + "\",\"manufacturer\":\"Bouffalo Lab\"}"; }
    std::string GetDeviceStatusJson() override { return "{}"; }
private:
    BouffaloNetwork network_;
    BouffaloAudioCodec audio_;
};
DECLARE_BOARD(BouffaloBoard)
std::string SystemInfo::GetMacAddress() { char mac[18] = {}, uuid[37] = {}; return ReadIdentity(mac, uuid) ? mac : ""; }
std::string SystemInfo::GetChipModelName() { return "bl616cl"; }
std::string SystemInfo::GetUserAgent() { return std::string(kBoard) + "/" + BouffaloApplicationVersion(); }
