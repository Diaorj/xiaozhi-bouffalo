# Bouffalo / BL616CL 移植

当前运行 C++ 启动检查、原状态机演示、Wi-Fi / DNS / HTTPS 验证，并直接调用原 `main/ota.cc` 中的 `Ota::CheckVersion()`。随后调用原 WebsocketProtocol 完成官方 WebSocket 会话握手，并自动发送一次标准短唤醒词并播放官方返回的 Opus。已接入原 AudioService 的播放链路，尚未启动完整 Application、麦克风或语音处理引擎。

## 构建

仓库位于 SDK 的 `examples/solution/xiaozhi-bouffalo`。
已验证 SDK 为 b3874865（包含 877ddce7），GCC 10.2.0。

```sh
cp ports/bouffalo/wifi_config.example.h ports/bouffalo/wifi_config.h
# 编辑本地 wifi_config.h，填写 2.4 GHz 热点。
make CHIP=bl616cl BOARD=bl616cldk CROSS_COMPILE=riscv64-unknown-elf-
```

wifi_config.h 被 Git 忽略，无本地配置时只初始化、不连接。ESP-IDF 原构建保留。

## 当前执行流程

C++ 构造检查 → 状态机演示 → RF/lwIP/Wi-Fi 初始化 → DHCP → DNS → SNTP 校时 → example.com HTTPS 验证 → 原 Ota::CheckVersion() → 原 WebsocketProtocol::Start()/OpenAudioChannel() → 短唤醒词问候 → Opus 解码与播放。
Wi-Fi 使用 DHCP、可选 PMF，仅提交一次连接，关联和 DHCP 各等最多 30 秒。事件回调只发通知；阻塞通信在网络任务执行。失败后打印位置，复位重试，心跳继续。

新旧板上身份保持一致：真实 MAC；URL 命名空间 UUIDv5，名称为 `xiaozhi-bouffalo/bl616cl/<MAC>`；板卡 `bouffalo-bl616cl-es8388`，固件版本 0.0.1。

## 原 OTA 类的适配

* `ota_bootstrap.cc` 只初始化存储、调用原 CheckVersion、报告结果；已删除临时 ota_check.cc/.h。
* `board_bouffalo.cc` 提供原 Board 工厂和 SystemInfo 的设备信息，恰好一个 DECLARE_BOARD。SDK BSP 的 board.h 通过生成的 sdk_board.h 明确引用，避免同名头冲突。
* `http_bouffalo.cc` 实现原 Http/NetworkInterface 接口，底下调用 SDK HTTPS 客户端。当前支持官方 OTA 检查和 activate 地址的有界同步 GET/JSON POST；响应上限 8192 字节。只信任 DigiCert Global Root G2，保留主机名和证书日期检查。HTTP 尚不支持任意站点、流式 Write 或响应头访问。CreateSsl/CreateWebSocket 已实现；TCP 明文、UDP/MQTT 工厂仍返回 nullptr。
* `settings_bouffalo.cc` 实现原 Settings 接口，通过 SDK LittleFS KV 保存到现有 PSM 分区（当前 4MB 分区表为 0x3E9000、32KB）。每个命名空间保存为 xz.<namespace>，关闭写事务时提交；原 SDK KV 空间的其他键不主动清除。SDK 会初始化未格式化的 PSM 文件系统；正常复位保留配置，整片擦除不会保留。
* `main/platform/expected.h` 在博流使用 tl::expected，ESP32 保留 std::expected。third_party 中保留固定版本原网络接口头和许可证，CMake 生成 C++17 兼容头与 WebSocket 适配源码，不修改第三方原文件或 SDK。
* 原 OTA 的 ESP32 下载/分区烧写路径在博流明确返回不支持。原响应解析、配置保存、版本比较和激活请求函数可编译；当前启动仅调用 CheckVersion，不自动激活轮询或升级。
* 服务端时间经 C 接口设置为 UTC，避免时区偏移影响后续 TLS。

配置读写失败会令此次启动检查失败。凭据不由应用日志打印。配置保存是按命名空间提交，尚无跨命名空间原子事务。
网络与 SNTP 检查沿用上一步：校时的两个 UDP 服务各等 5 秒；HTTP 等待 10 秒不覆盖 DNS/TCP/TLS 全过程；当前有效时间范围 2024 年至 2036 年 NTP 纪元切换前。SNTP 用于开发阶段的时钟初始化。

## 原 WebSocket 协议接入

* 编译原 `main/protocols/websocket_protocol.cc` 和 `protocol.cc`，认证头、hello 构造与解析仍由原类完成。`websocket_bootstrap.cc` 创建原对象、注册回调并调用公开方法；接收回调只入队和置标志，网络任务等待回复结束并收尾。
* URL、token、version 读取 OTA 保存的 Settings；MAC、UUID 沿用当前绑定。如果本次 OTA 仍要求激活，则不使用缓存启动会话。
* `BouffaloNetwork::CreateWebSocket` 创建原 esp-ml307 WebSocket；`CreateSsl` 创建博流适配。TLS 使用 SDK https_wrapper，显式 DigiCert Global Root G2、SNI 与日期检查，不回退到明文或跳过验证；其他证书链需补充信任根后验证。
* `tcp_bouffalo.cc` 在后台任务接收 TLS 数据，互斥串行访问 SSL 上下文；非阻塞读取、分段完整发送，退出时等待回调结束并释放连接。发送等待有 5 秒 socket 超时；DNS、TCP、TLS 建连仍沿用 SDK 的等待行为。
* 第三方源码固定为 v3.7.6。`prepare_websocket.py` 生成博流专用编译副本：使用 SDK 随机数、校验 Sec-WebSocket-Accept、限制消息至 16 KiB、处理失败和关闭。不改变 ESP-IDF 的依赖选择。
* hello 保留原 Opus 格式与 60 ms 帧参数；当前已解码播放，不上传录音，也不宣告尚未移植的 MCP、AEC、字库推送能力。
* 这一步只启动一次；支持回复服务端 Ping，不主动保活或自动重连。服务端空闲关闭后可复位重试，关闭日志本身不代表初始握手失败。

## 验证

```text
ORIGINAL_OTA_BEGIN: calling Ota::CheckVersion()
ORIGINAL_OTA_OK: activation=0 mqtt=1 websocket=1 server_time=1
SETTINGS_SAVED: SDK LittleFS KV; firmware upgrade disabled
```

第一次保存成功后按 RESET；下一次网络请求前应看到 SETTINGS_LOADED: mqtt=1 websocket=1（以服务端实际返回为准）。1/0 仅表示字段存在，不代表已建立对话连接。

```sh
python3 -m unittest discover -s ports/bouffalo/tests -v
python3 -m unittest discover -s scripts/tests -v
```

原 Ota 调用、绑定及复位后的配置重载已在板上验证。本次新增连接应继续看到：

```text
WSS_TLS_OK
WebSocket handshake done, ...
Server hello received; session established
ORIGINAL_WS_OK: authenticated; server hello received; rate=24000 frame=60 ms
AUDIO_SERVICE_READY: original decode/playback queues and tasks; playback only
WAKE_GREETING_SENT: standard short wake phrase; volume=15; AudioService
OPUS_FIRST_PACKET: decoded=960 samples at 16000 Hz
SPEAKER_PLAYING: PCM 16000 Hz, volume=15
AUDIO_REPLY_DONE: ... speaker drained and muted
```

采样率以服务端实际协商结果为准。只有 ORIGINAL_WS_OK 才表示本次应用层握手成功。
已完成 BL616CL 构建、端口 14 项主机测试（含真实 SDK Opus 编解码、PCM/DMA 打包及 14 个 WebSocket 场景）及原项目 111 项回归（1 项跳过）。WSS 官方连接和原 AudioService 播放均已在 BL616CL 板上确认；2026-10-27 日志显示 107 个音频包播放完成，未进行 ESP-IDF 整机编译。

## 官方语音播放（当前步骤）

每次上电仅发送一次标准短唤醒词“你好小智”，使用原 Protocol::SendWakeWordDetected 的 listen/detect/text 消息序列化。官方服务端明确拒绝把长问题当作 detect 文本；这一步验证唤醒问候播放，不代表已支持任意文字聊天。
音频回调调用原 AudioService::PushPacketToDecodeQueue，原 OpusCodecTask 解码并将 PCM 放进原播放队列，原 AudioOutputTask 交给 AudioCodec 输出。接收任务不等待扬声器。TTS stop 后先等原队列及在途工作完成，再通过 AudioCodec::FinishOutput 等 DMA 播完才报告 AUDIO_REPLY_DONE。临时 reply_audio.cc/.h 已删除。

板卡 GetAudioCodec() 返回原 AudioCodec 的博流实现；原 AudioCodec 基类参与编译，ESP I2S 类型仅在 ESP 构建包含。博流 CMake 编译原 audio_service.cc，并启用 XIAOZHI_AUDIO_PLAYBACK_ONLY 裁掉当前未移植的输入、编码、ESP 音频引擎和电源定时器；ESP 默认构建仍保留完整路径。AudioDecoder 是公共解码接口，opus_decoder_bouffalo.cc 提供 SDK Opus 实现；公共 AudioService 不引用具体博流板类。
SDK CONFIG_OPUS 提供真实固定点解码器；24 kHz 服务端码流由 Opus 直接解码成 16 kHz PCM。ES8388/I2S 沿用实测 16 kHz 时钟（实际约 16025.6 Hz），不是额外的简单抽样丢点。

PCM 使用 64 KB PSRAM 环形缓冲，DMA 使用内部非缓存 SRAM 的 8×160 帧立体声块；引脚为 MCLK=1、BCLK=4、LRCK=5、DSDIN=7、SDA=11、SCL=20。DACR 和差分功放输出配置沿用旧 Demo，默认 PCM 音量 15，不写 Flash。
压缩包队列上限 128 包/64 KB、每包不超过 4096 字节。队列溢出、解码错误、播放错误、未收到 TTS stop 的断线都明确失败并静音；不丢包后继续假装成功。此步骤仍是上电问候测试，无麦克风、唤醒、打断、自动重连或重复提问入口。AudioService::Stop 唤醒并等待解码/播放两条任务退出后再关闭输出，任务创建失败也会回收已创建的任务；不在网络回调中停止硬件。

新增主机集成测试编译 SDK 的实际 Opus 源码，验证协议 v1/v2/v3 收包、24→16 kHz 解码、原 AudioCodec 输出，以及空包、断线、硬件打开/写入失败。PCM 声道打包、音量和环形回卷另经 ASan/UBSan 检查。
上一步的独立播放器已在板上确认出声，用户反馈声音清楚；默认音量随后由 20 调为 15。2026-10-27 已确认原 AudioService 正常播放，默认音量为 15。

### 2026-10-23 官方联调修正

同设备身份实测：长 detect 文本返回 alert/ERROR：`Detect is only for wake words, do not send long texts.`，音频包数 0；改为“你好小智”后返回 STT、TTS start、68 个音频包及 TTS stop。
原 Application 处理 alert 的 status/message/emotion，当前无屏幕启动入口改为串口显示并取消本轮等待；错误日志补充具体原因和收包数。原板端日志漏掉了 alert，只显示 15 秒后泛化失败。新增测试按实际返回覆盖此情形，并检查唤醒文本，避免模拟服务端错误地接受任意长问题。
修复后的官方返回、板上解码和扬声器播放已确认成功。

### 2026-10-26 接入原 AudioService

沿用原 AudioService 的解码队列、两帧 PCM 播放队列、OpusCodecTask 与 AudioOutputTask；博流提供 AudioDecoder 和 AudioCodec 的后端。std::mutex 沿用 SDK；条件变量通过 main/platform/condition_variable.h 显式初始化 SDK pthread_cond_t，避开当前预编译 libstdc++ 的布局不兼容。ESP 默认仍使用 std::condition_variable。播放任务栈为 2048 words，解码任务栈为 4096 words。
主机集成测试扩充为 17 个场景：除成功路径外，覆盖任务创建失败、真实 Opus 解码错误、DMA 排空失败、空回复、超时、TTS stop 后断线继续排空、播放中停止、重复启停以及包数/字节数上限。当前仍是播放阶段，未宣告麦克风或完整 AudioEngine 支持。

### 2026-10-26 条件变量构造兼容修正

板上在 AUDIO_SERVICE_READY 后 terminate 并输出 Coredump。实际 SDK 头文件下 sizeof(pthread_cond_t) 和 sizeof(std::condition_variable) 均为 12；当前预编译 libstdc++ 构造函数却清零 64 字节，还留下 tasksLenth=0。SDK pthread_cond_wait 因此返回 ENOMEM，C++ wait 调用 terminate。
项目平台层改为直接 pthread_cond_init/wait/broadcast/destroy，继续使用原队列、任务和 std::mutex，ESP 分支仍为标准条件变量。13 项主机测试通过；新增测试分别验证原 SDK 源码的零初始化失败/显式初始化成功，以及多任务等待、广播与错误诊断。最终 ELF 不再链接不兼容的 std::condition_variable 构造函数；后续板上验证已通过此阶段。

### 2026-10-27 片内内存布局与定位

板上已通过第一包 Opus 解码和 Codec/I2S 配置，随后进入 FreeRTOS malloc 失败钩子。代码把范围收敛到创建 speaker_dma 任务的栈/TCB 申请；具体失败大小须由新诊断确认。SDK heap_3 的任务分配固定使用 OCRAM，当前两个 AudioService 任务比旧单任务增加8 KB内部栈。
Opus 解码器改用 get_size + PSRAM/CACHE 对齐分配 + opus_decoder_init，匹配 kfree，保持解码器4096 words、输出2048 words、DMA1536 words的栈尺寸。音频环形缓冲和 Opus 状态在PSRAM，硬件DMA缓冲在内部无缓存区。
memory_diagnostics.c 包装外部 pvPortMalloc/kmalloc 引用并提供强应用失败钩子：记录请求大小、任务、调用返回地址、堆总余量/最大连续块和当前任务栈水位；固定32个记录槽，不占任务TLS槽，不分配诊断内存，仍在打印后中止。阶段检查点与 BUILD_TAG 用于确认固件版本。返回地址减1后结合相同ELF定位。
14项端口主机测试通过（包括18个音频场景、显式解码器状态释放与PSRAM分配失败、四个诊断场景），构建和原111项回归通过（1项跳过）。用户已确认板上播放正常。该次日志显示 Opus 状态为17800字节、扬声器任务申请6144字节栈并成功创建，最终 AUDIO_REPLY_DONE 收到107包。任务创建后 OCRAM 剩余9564字节、最大连续块3648字节；这些是该次短问候的观测值，长期稳定性和麦克风尚未验证。
