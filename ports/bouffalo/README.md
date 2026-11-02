# Bouffalo / BL616CL 移植

当前保留小智原工程结构，用 Bouffalo SDK 构建 BL616CL 固件。已在板上验证官方 OTA 检查、工作台绑定、WebSocket 握手与 AudioService 语音播放。新加入的单轮麦克风输入已通过构建和主机模拟，待板上验证。

## 构建与烧录

当前 SDK：`db9219af`（2.3.38，含父提交 `ae8ff431`）。工具链为同事提供的 GCC 14.3.0、POSIX gthread、Picolibc 版本。
工程目录：`bouffalo_sdk/examples/solution/xiaozhi-bouffalo`。

```sh
# 首次使用才复制，并在本地文件填写 2.4 GHz 热点；该文件被 Git 忽略。
cp -n ports/bouffalo/wifi_config.example.h ports/bouffalo/wifi_config.h
export PATH="$HOME/riscv64-zephyr-elf-gthread/bin:$PATH"
make CHIP=bl616cl BOARD=bl616cldk \
  CROSS_COMPILE="$HOME/riscv64-zephyr-elf-gthread/bin/riscv64-zephyr-elf-" \
  XIAOZHI_PTHREAD_KEY_ASSERT_STUBS=ON
```

固件在 `build/build_out/xiaozhi_bouffalo_bl616cl.bin`。关闭占用 COM7 的终端、让板子进入下载模式后，用相同参数执行 `make flash ... COMX=COM7`，退出下载模式再 RESET。
当前 VS Code 插件对 BL616CL 仍检查 unknown-elf 前缀，暂用上述终端命令选择新工具链。

`XIAOZHI_PTHREAD_KEY_ASSERT_STUBS` 默认 OFF；当前 release SDK 缺少工具链所引用的四个 pthread key API 时显式开启。它只补声明和断言入口，调用就中止，不实现线程私有存储。SDK 有完整实现时保持 OFF。

## 当前执行流程

联网 → DNS / SNTP / HTTPS 检查 → 原 `Ota::CheckVersion()` → 提示录音 5 秒 → Opus 编码 → 连接官方 WebSocket → 上传 → 识别与语音回答 → 播完并主动关闭会话。

看到 `RECORDING: speak now, 5 seconds` 后说一个简短问题，例如“你好，你是谁？”。这是一次上电测试；结束后 RESET 再试，不是连续对话或唤醒模式。Wi-Fi、连接或识别失败会打印阶段，心跳继续。

预期日志：

```text
RECORDING: speak now, 5 seconds
VOICE_CAPTURE_DONE: samples=80000 peak=... mean_abs=...
VOICE_ENCODE_DONE: packets=84; PCM and encoder released
ORIGINAL_WS_OK: authenticated; server hello received
VOICE_UPLOADED: waiting for recognition and reply
RECOGNIZED: ...
SPEAKER_PLAYING: PCM 16000 Hz, volume=15
WS_SESSION_RELEASED: socket and receiver released
AUDIO_REPLY_DONE: ...
```

默认 `XIAOZHI_VOICE_INPUT_TEST=ON`。构建加 `XIAOZHI_VOICE_INPUT_TEST=OFF` 可恢复上一步短唤醒词问候测试；OFF 时不打开麦克风。官方 listen/detect 只能发送唤醒词，语音提问使用原 `SendStartListening(manual)`、`SendAudio()`、`SendStopListening()`。

## 代码分工

- 原 `Ota`、`Protocol`、`WebsocketProtocol` 负责版本检查、认证、消息序列化及协议解析。
- 原 `AudioCodec` 接口接到 `audio_codec_bouffalo.cc`，I2C/I2S/DMA 由项目内 `audio_sdk_bridge.c` 调用 SDK。
- 原 `AudioService` 保留编码队列、发送队列、解码队列和播放队列。新增平台 `AudioEncoder` / `AudioDecoder` 接口注入 SDK Opus 实现；本次仍改动了公共 AudioService，尚不是纯板级新增。
- `XIAOZHI_AUDIO_PLATFORM_BACKEND` 隔离 ESP 专用音频库和引擎；替代原先名为 PLAYBACK_ONLY 的开关。ESP 默认保留原后端，未进行 ESP-IDF 整机编译。
- `voice_input.cc` 只做单轮验证的阶段调度，不替代完整 `Application`。录音与网络通信在现有网络任务中执行；接收回调只入队或置标志。
- SDK 和第三方原文件未改。`prepare_websocket.py` 在构建时生成原 esp-ml307 v3.7.6 的兼容副本。

## 音频与内存

ES8388：MCLK=1、BCLK=4、LRCK=5、输入 GPIO6、输出 GPIO7、SDA=11、SCL=20。麦克风为 LIN2/RIN2、+18 dB PGA，取 DMA 字的 slot1 / 高 16 位。名义 16 kHz、单声道、有符号 16 位 PCM；实际硬件采样率约 16025.6 Hz。

录音和播放半双工，共用内部非缓存 DMA 环（8×160 立体声帧）。录音时 DAC 静音，停止 RX 后才允许播放，默认音量 15、不写 Flash。

为控制片内 RAM 峰值，阶段不重叠：

| 阶段 | 主要内存与任务 |
| --- | --- |
| 采集 | 5 秒 PCM 160000 字节，放 PSRAM；用现有网络任务读取 DMA |
| 编码 | Opus 状态和最多 84×512 字节压缩缓冲放 PSRAM；只启动原 Opus 工作任务，临时栈 48 KiB 在内部 RAM |
| 连接与播放 | 编码任务、编码状态和录音 PCM 已释放；WSS 接收任务 + 原解码/输出任务 + 固定扬声器任务；PCM 环 64000 字节在 PSRAM |

48 KiB 是编码任务的预留栈，并非已测得的实际消耗。`voice_encoder_first_frame` 的 MEM 日志记录板上栈水位，供后续调整。编码在建立 TLS 前完成，避免它和播放任务栈同时占用内部 RAM。

录音不足一帧的末尾补零：80000 样本编码成 84 帧、每帧 60 ms（共 80640 样本）。使用 SDK Opus VOIP、16 kbps、复杂度 0；上传按 60 ms 间隔发送。录音溢出/超时则丢弃本轮，编码或发送队列满会明确失败，不静默丢语音。

播放队列上限 128 包、累计 64 KiB，每包最多 4096 字节。TTS stop 后等待队列和 DMA 排空，再停止任务。单轮结束由控制任务发送 close 帧，最多等 1 秒后强制释放 socket/TLS。此前多次 RESET 播放已在板上验证；“服务端残留会话”仍不是已证实的唯一根因。

## 网络与存储边界

- Wi-Fi 用 DHCP、可选 PMF，只提交一次连接；暂不自动重连或配网。
- 官方 MAC/稳定 UUID 沿用工作台绑定；不打印 token。模型和语音服务由官方服务端提供。
- HTTPS/WSS 使用 SDK TLS，校验 CA、主机名和日期，当前信任 DigiCert Global Root G2；不跳过校验。
- `Settings` 使用 SDK LittleFS KV，保存在既有 PSM 分区（4 MiB 配置下 0x3E9000、32 KiB）；正常 RESET 保留，整片擦除不保留。
- HTTP 适配目前支持 OTA 所需的有界 GET/JSON POST；不支持任意流式请求。固件升级、MQTT/UDP、完整 Application、AudioEngine、唤醒词、AEC、语音打断和连续监听均未接入。

## 验证

```sh
# release SDK 只有 Opus 静态库，主机测试用内部 SDK 的对应源码。
export XIAOZHI_HOST_OPUS_SOURCE="$HOME/workspace/bouffalo_sdk_internal/bouffalo_sdk/components/multimedia/opus"
cd ports/bouffalo/tests
python3 -m unittest -v test_voice_input test_reply_audio test_speaker_worker test_tls_read_diagnostics test_websocket
```

语音集成测试模拟麦克风和网络，编译实际 SDK Opus 源码，检查 84 帧上传能解码、识别消息与回答播放流程，以及输入/分配/任务/上传失败清理。旧问候的 18 个生命周期场景和 WebSocket/TLS、固定任务栈回归同时通过。
主机测试不等于真实麦克风、官方 ASR 或扬声器验证；这次语音输入仍需板上确认。SDK 的 bflb_dbg.h 有符号比较警告仍存在。
