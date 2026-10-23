# Bouffalo / BL616CL 移植

当前运行 C++ 启动检查、原小智状态机演示、Wi-Fi 联网、DNS/HTTPS 连通性检查及一次官方 OTA/激活信息检查。尚未启动小智 Application、云端协议或音频功能。

## 构建

将本仓库放在 Bouffalo SDK 的 `examples/solution/xiaozhi-bouffalo` 下。
SDK 需要 C++ 运行时及 FreeRTOS POSIX 支持；当前验证版本为 `b3874865`（包含 `877ddce7`），工具链为 GCC 10.2.0。

```sh
cp ports/bouffalo/wifi_config.example.h ports/bouffalo/wifi_config.h
# 编辑 wifi_config.h，填写 2.4 GHz 热点名称和密码。
make CHIP=bl616cl BOARD=bl616cldk CROSS_COMPILE=riscv64-unknown-elf-
```

本地 `wifi_config.h` 已被 Git 忽略。没有本地配置时使用空白模板，只初始化，不连接热点。

## 启动流程

构造检查 → 状态机演示 → RF/lwIP/Wi-Fi 初始化 → 连接热点 → DHCP → DNS → 校时 → HTTPS。
已删除启动时的双任务互斥锁检查，状态机保留原生 `std::mutex`。

连接参数使用 DHCP、PMF 可选（`pmf_cfg=1`），关闭 SDK 自动重连。关联和 DHCP 分别等待最多 30 秒，失败或断网后复位重试；不会由应用提交第二次连接请求。
SDK 事件回调只发送任务通知，连接与 HTTPS 在普通网络任务执行；网卡和 DNS 配置信息在 lwIP 核心线程读取。

```text
CXX_RUNTIME_OK: constructors=1
STATE_MACHINE_OK: current=idle (demo only)
WIFI_INIT_OK: driver and manager ready
WIFI_ASSOCIATED: waiting for DHCP
WIFI_READY: IPv4 acquired; DNS/Internet/server not tested
DNS_OK: example.com -> ...
TIME_READY: ...
HTTPS_OK: status=200; verified response received
```

`WIFI_READY` 仅表示已有 IPv4。随后解析 `example.com`，必要时通过 SNTP 设置系统时间，再用 SDK HTTP 客户端访问 `https://example.com/`。请求明确传入公开根证书，校验信任链、主机名和有效期，不降级跳过验证。只接受完整的 2xx 响应。

该根证书仅用于此测试站点，不是通用证书库。站点更换证书链时需要重新核对。校时依次尝试 ntp1.aliyun.com、time.cloudflare.com；每个 UDP 响应等待 5 秒。当前 SNTP 是开发阶段的时间初始化，不提供时间源认证；有效时间范围为 2024 年至 2036 年 NTP 纪元切换前。

DNS、校时或 HTTPS 失败后只打印失败位置，不循环重试；心跳继续运行。HTTP 传输等待设置为 10 秒，DNS/TCP/TLS 使用 SDK 自身的等待机制，整个检查不保证 10 秒内完成。HTTPS 成功只验证该公开站点，不代表小智服务端已接入。

`http_sdk_bridge.c` 隔离 SDK HTTP 头的 C++ 链接花括号问题；`wifi_sdk_bridge.c` 隔离 Wi-Fi 头的 C++ 类型转换问题，未修改 SDK 源码。

## 主机测试

```sh
python3 -m unittest discover -s ports/bouffalo/tests -v
```

覆盖状态机与互斥锁、28 种网络事件路径、HTTPS 成功与失败路径，以及 SNTP 响应校验。测试使用主机标准库或 SDK 模拟，不能替代板上验证。

板上验证：2026-10-22 已确认 WIFI_READY、DNS_OK、TIME_READY 和 HTTPS_OK（200），心跳持续运行。

## OTA / 激活信息检查（板上已验证）

基础网络检查成功后，网络任务调用 `RunOtaCheck()`，向原工程配置的 `https://api.tenclass.net/xiaozhi/ota/` 发送一次 JSON POST。请求沿用 Activation-Version、Device-Id、Client-Id、User-Agent 等字段；设备 MAC 从 SDK 获取，板卡标识为 `bouffalo-bl616cl-es8388`，当前移植版本为 `0.0.1`，不伪装 ESP32。

Client-Id 使用 URL 命名空间的 UUIDv5，由名称 `xiaozhi-bouffalo/bl616cl/<MAC>` 生成，复位后保持一致，不写 Flash。它是设备标识，不是安全凭据；后续更改算法会改变服务端所见身份。

`main/ota_check_response.*` 从原 Ota 类提取共用的 JSON 所有权及激活信息解析，两种构建都使用它。原 ESP32 的 MQTT/WebSocket 设置写入、时间及版本处理保持原流程。当前 GCC 10 工具链尚不能直接使用原 Ota 类依赖的 C++23 `std::expected`，也没有适配其 ESP32 分区/升级接口，所以本步在端口中编排一次检查，不编译完整 Ota 类。

HTTPS 明确使用 DigiCert Global Root G2，保留证书、主机名及日期验证。响应上限 8192 字节，超长、非完整响应、非 200 状态或非法 JSON 均失败，不将截断数据当作配置。连接配置只保留在 RAM，应用日志不打印 token、密码或 challenge。

```text
OTA_CHECK_BEGIN: https://api.tenclass.net/xiaozhi/ota/; ...
OTA_CHECK_OK: activation=1 mqtt=0 websocket=0
ACTIVATION_REQUIRED: code=...; device is not activated yet
OTA_CHECK_ONLY: no firmware download, flash update or conversation connection
```

三个数字只表示返回的字段是否存在，不表示已完成激活或已建立连接。已绑定的设备可能直接返回 MQTT 或 WebSocket 配置。激活码可能过期；本步不调用 `/activate`、不自动轮询、不下载或烧写任何固件，失败后复位重试。配置尚未持久化。

本次已通过 BL616CL 构建、6 项移植主机测试及原项目 111 项回归（1 项跳过）；未进行 ESP-IDF 整机编译，2026-10-23 板上已确认官方 OTA 请求成功，绑定后重启返回 activation=0、mqtt=1、websocket=1。
