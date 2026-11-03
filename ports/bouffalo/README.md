# Bouffalo / BL616CL 移植

当前保留小智原工程结构，用 Bouffalo SDK 构建 BL616CL 固件。已在板上验证官方 OTA 检查、工作台绑定、WebSocket 握手与 AudioService 语音播放。单轮麦克风输入已在板上完成官方识别与回答播放；本次继续改成流式上传，待板上确认延迟与内存。

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

联网 → DNS / SNTP / HTTPS 检查 → 原 `Ota::CheckVersion()` → 连接官方 WebSocket → 提示录音 5 秒，边录边编码上传 → 识别与语音回答 → 播完并主动关闭会话。

看到 `RECORDING: speak now, 5 seconds` 后说一个简短问题，例如“你好，你是谁？”。这是一次上电测试；结束后 RESET 再试，不是连续对话或唤醒模式。Wi-Fi、连接或识别失败会打印阶段，心跳继续。

预期日志：

```text
ORIGINAL_WS_OK: authenticated; server hello received; ... connect_ms=...
RECORDING: speak now, 5 seconds; streaming upload
VOICE_FIRST_UPLOAD: from_recording_start_ms=...
VOICE_CAPTURE_DONE: samples=80000 peak=... mean_abs=... packets=84
VOICE_TIMING: capture_ms=... first_upload_ms=... upload_tail_ms=... total_ms=...
VOICE_UPLOADED: packets=84 bytes=...; waiting for recognition and reply
REPLY_FIRST_AUDIO: after_request_ms=...
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
- `voice_input.cc` 只做单轮验证的采集控制，不替代完整 `Application`。录音在共用 DMA 工作任务中运行，编码走原 Opus 工作任务，网络任务持续取发送队列；接收回调只入队或置标志。
- SDK 和第三方原文件未改。`prepare_websocket.py` 在构建时生成原 esp-ml307 v3.7.6 的兼容副本。

## 音频与内存

ES8388：MCLK=1、BCLK=4、LRCK=5、输入 GPIO6、输出 GPIO7、SDA=11、SCL=20。麦克风为 LIN2/RIN2、+18 dB PGA，取 DMA 字的 slot1 / 高 16 位。名义 16 kHz、单声道、有符号 16 位 PCM；实际硬件采样率约 16025.6 Hz。

录音和播放半双工，共用内部非缓存 DMA 环（8×160 立体声帧）。录音时 DAC 静音，停止 RX 后才允许播放，默认音量 15、不写 Flash。

录音和上传同时运行；播放任务等输入结束后再创建，控制片内 RAM 峰值：

| 阶段 | 主要内存与任务 |
| --- | --- |
| 连接准备 | 提前创建编码任务，避免 TLS 分配后找不到连续栈空间；Opus 状态在 PSRAM |
| 采集 / 编码 / 上传 | 共用原先的 6 KiB 静态 DMA 工作栈；编码任务栈 32 KiB；网络任务持续发送，不保存整段 PCM 或压缩录音 |
| 播放 | 先停止并等待采集/编码任务，释放编码栈和状态；再启动原解码/输出任务，复用同一 DMA 工作任务；PCM 环 64000 字节在 PSRAM |

编码栈由 48 KiB 改为 32 KiB：上一版板上第一帧已测用量约 22 KiB，本版记录第一帧及全部帧结束时的栈水位。需继续用实际对话验证余量；任务栈保持在内部 RAM。

80000 样本编码成 84 帧，末尾补零至 80640 样本，每帧 60 ms；SDK Opus VOIP、16 kbps、复杂度 0。发送由采样到达自然控制速度，移除每包额外等待的 60 ms。编码队列最多 2 帧、发送队列最多 4 包；网络阻塞导致积压时明确取消本轮，避免无界申请或静默丢掉语音。

DMA 工作任务现为 `audio_dma_worker.c/.h`（原 speaker_worker），录音和播放顺序复用，栈仍为 1536 words / 6144 字节。输入结束先 join 采集再停止编码，确保不会在生产者仍访问队列或硬件时释放资源。

`VOICE_TIMING` 用于实测：`first_upload_ms` 应小于录音总时长；`upload_tail_ms` 是录音结束后等待编码/发送收尾的时间，不应再有固定 5 秒上传等待。`REPLY_FIRST_AUDIO` 记录发送 listen/stop 到收到首个回答音频包的时间；实际值受网络及服务端影响。

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
python3 -m unittest -v test_voice_input test_reply_audio test_audio_dma_worker test_tls_read_diagnostics test_websocket
```

语音集成测试模拟麦克风和网络，编译实际 SDK Opus 源码，检查第一包在录音结束前上传、84 帧能解码、识别消息与回答播放流程，以及输入/任务/上传失败、发送积压和中途断线的取消与清理。旧问候的 18 个生命周期场景和 WebSocket/TLS、固定任务栈回归同时通过。
主机测试不等于真实麦克风、官方 ASR 或扬声器验证；本次流式上传仍需板上确认。SDK 的 bflb_dbg.h 有符号比较警告仍存在。
