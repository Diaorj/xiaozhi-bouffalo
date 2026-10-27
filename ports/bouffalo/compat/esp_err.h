#pragma once
// Port-side error values used by the original application interfaces.
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_TIMEOUT = 0x107;
