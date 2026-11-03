# Bouffalo / BL616CL 移植

保留小智原工程结构，用 Bouffalo SDK 构建 BL616CL 固件。官方 OTA、绑定、语音输入输出、流式上传、VAD 和播放后恢复监听已在板上验证。连续对话基线为 `543864d`。已补上联网失败重试与运行中掉线恢复，用户板测暂未发现异常。

## 构建与烧录

SDK：`db9219af`（2.3.38，含 `ae8ff431`）。工具链：同事提供的 GCC 14.3.0、POSIX gthread、Picolibc 版本。
工程目录：`bouffalo_sdk/examples/solution/xiaozhi-bouffalo`。

```sh
# 首次使用才复制；编辑本地文件填写 2.4 GHz 热点，该文件被 Git 忽略。
cp -n ports/bouffalo/wifi_config.example.h ports/bouffalo/wifi_config.h
export PATH="$HOME/riscv64-zephyr-elf-gthread/bin:$PATH"
make CHIP=bl616cl BOARD=bl616cldk \
  CROSS_COMPILE="$HOME/riscv64-zephyr-elf-gthread/bin/riscv64-zephyr-elf-" \
  XIAOZHI_PTHREAD_KEY_ASSERT_STUBS=ON
```

关闭占用 COM7 的终端、进入下载模式后，用相同参数执行 `make flash ... COMX=COM7`，退出下载模式再 RESET。固件为 `build/build_out/xiaozhi_bouffalo_bl616cl.bin`，标记 `network-recovery-20261103-v7`。
当前 VS Code 插件对 BL616CL 仍检查 unknown-elf 前缀，暂用上述终端命令。

`XIAOZHI_PTHREAD_KEY_ASSERT_STUBS` 默认 OFF；当前 release SDK 缺四个 pthread key API 时显式开启。它只提供声明和断言入口，调用即中止，不实现线程私有存储。SDK 有完整实现时保持 OFF。

## 使用

上电联网 → 原 OTA 检查与 WebSocket 认证 → 等待说话 → 边录边编码上传 → 自动判断说话结束 → 播放回答 → 恢复监听。

看到 `AUTO_LISTENING` 后说话。连续安静约 800 ms 结束本轮，单轮最长 10 秒（含前缓冲）。安静时不发送 listen/start 或空录音；80 ms 短噪声会被过滤，至少约 160 ms 语音才触发。每次重新打开麦克风后先进行 300 ms 安静校准。

```text
AUTO_LISTENING: speak now; stop after 800 ms silence, max 10 s
VOICE_START: validated voice; preroll retained; streaming upload
VOICE_CAPTURE_DONE: samples=... packets=... reason=silence
VOICE_UPLOADED: packets=... bytes=...
RECOGNIZED: ...
AUDIO_REPLY_DONE: round=1 ...
AUTO_LISTEN_RESUME: round=1 completed; same server session
AUTO_LISTENING: ...
```

默认 `XIAOZHI_AUTO_CHAT=ON`，保留同一条服务端连接连续问答。设为 OFF 可只运行一轮 VAD 问答；`XIAOZHI_VOICE_INPUT_TEST=OFF` 恢复短唤醒词问候测试。
等待期间每 20 秒发送 WebSocket Ping，30 秒未收到 Pong 则终止并释放连接。失败的录音或回答会丢弃，恢复后重新监听，不自动重放问题。

连接热点、DHCP 或服务端失败后，按 3、6、12、24、30 秒间隔重试，之后每 30 秒尝试一次。Wi-Fi 连接成功后复位其重试间隔。运行中断网先停止录音/播放，释放旧 WebSocket/TLS，再重新获取 IP、执行联网检查和 OTA，连接成功后回到 `AUTO_LISTENING`。只发生互联网或服务端失败时保留当前 Wi-Fi；同一网络任务负责恢复，不重复启动驱动或管理器。

SDK 明确报告参数错误时停止重试并打印 `WIFI_RETRY_STOPPED`，需要修改本地配置；空 SSID 仍只初始化，不连接。RF、驱动等初始化失败仍保留原失败提示，不反复初始化硬件。服务端重试复用成功取得的 OTA 配置，超过 60 秒才重新刷新；失败的 OTA 请求按退避间隔重试。

测试恢复：反复 RESET 后观察是否进入监听；关闭热点，等待出现 `WIFI_OFFLINE` / `NETWORK_RETRY`，重新开启后应依次出现 `WIFI_READY`、`ORIGINAL_WS_OK`、`AUTO_LISTENING`。

## 架构与内存

- 原 `Ota`、`Protocol`、`WebsocketProtocol` 处理版本检查、认证和协议。语音使用原 listen/start(manual)、SendAudio、listen/stop；保留同一 session，不在每个回答后重新认证。
- 原 `AudioService` 保留编码/发送/解码/播放队列，平台 AudioEncoder / AudioDecoder 调用 SDK Opus。公共 AudioService 有平台分支改动，尚未整体接入原 Application、AudioEngine。
- `VoiceInputSession` 负责 VAD、前缓冲和采集生产者；网络任务从有界 PSRAM PCM 环取帧，再持续排空原发送队列。接收回调只入队或置标志，停止操作由网络任务执行。
- 录音与播放半双工，复用内部 6 KiB 静态 DMA 任务栈。播放期间停止录音；DMA 排空、静音、任务结束后才恢复监听，另留 300 ms 扬声器尾音衰减时间。
- 编码工作任务的 32 KiB 栈改为内部 **WRAM**，启动时申请一次并保留复用；它是 BSP 已注册的另一块内部堆。申请前检查至少还留 16 KiB，不足时明确失败，不回退占用 OCRAM。MEM 日志增加 wram_free / wram_largest。
- 解码 16 KiB、输出 8 KiB 任务栈仍在 OCRAM，按轮创建/停止；解码器状态保留在 PSRAM，启动下一轮播放时复位。编码和播放分阶段，不同时执行。
- PSRAM 中的 PCM FIFO 固定 1 秒（32000 字节），前缓冲固定 13120 字节；不保存整段录音。编码队列最多 2 帧、发送队列最多 4 包，积压超限明确失败，不静默丢音频。
- 原能量 VAD 从旧 Demo 移入项目，逐 10 ms 块处理并去除直流偏置。前缓冲保留约 250 ms 静音加语音起始验证段，避免漏掉开头；持续等待时计数保持有界。

WRAM 编码栈已在连续对话日志中确认申请成功；复用目的在于避免长连接保留时，每轮重新寻找连续 32 KiB OCRAM。长期运行仍需观察内存余量与栈水位。

SDK 和第三方原文件未改。WebSocket 兼容副本仍由 prepare_websocket.py 生成；本次心跳使用原 WebSocket::Ping/OnPong。Bouffalo 协议活动时间和错误标志使用原子变量，ESP 分支保持原路径，未进行 ESP-IDF 整机编译。

## 音频硬件与格式

ES8388：MCLK=1、BCLK=4、LRCK=5、输入 GPIO6、输出 GPIO7、SDA=11、SCL=20。麦克风使用 LIN2/RIN2、+18 dB PGA，取 slot1 / 高 16 位。16 kHz 单声道有符号 16 位 PCM，实际采样率约 16025.6 Hz。
Opus 为 VOIP、16 kbps、复杂度 0、60 ms 一帧；末尾不足一帧补零。采样到达自然控制上传速度，没有每包额外 60 ms 延迟。
播放默认音量 15，不写 Flash；PCM 环 64000 字节在 PSRAM，8×160 帧立体声 DMA 缓冲在内部非缓存 SRAM。

`VOICE_TIMING` 中 audio_ms 是本轮有效录音长度，first_upload_ms 是检测到语音到首包发出的时间，upload_tail_ms 是录音结束后的上传收尾时间。`REPLY_FIRST_AUDIO` 记录发送 listen/stop 到收到首包回答的时间。

## 网络与存储边界

Wi-Fi 使用 DHCP、可选 PMF，由项目网络任务管理延时重试，暂不配网。MAC 和稳定 UUID 沿用官方工作台绑定；不打印 token。模型、识别和语音合成由官方服务端提供。
HTTPS/WSS 使用 SDK TLS，保留 CA、主机名和日期校验，当前信任 DigiCert Global Root G2。Settings 使用既有 PSM 分区的 LittleFS KV（4 MiB 配置：0x3E9000、32 KiB），正常 RESET 保留，整片擦除不保留。
HTTP 适配支持 OTA 所需的有界请求；固件升级、MQTT/UDP、唤醒词、AEC 和语音打断尚未接入。

## 验证

```sh
export XIAOZHI_HOST_OPUS_SOURCE="$HOME/workspace/bouffalo_sdk_internal/bouffalo_sdk/components/multimedia/opus"
python3 -m unittest discover -s ports/bouffalo/tests -v
python3 -m unittest discover -s scripts/tests -v
```

release SDK 为预编译 Opus 库，主机测试使用对应 SDK 源码。模拟覆盖同一连接 20 轮识别和播放、静音不提问、短噪声过滤、10 秒上限、心跳确认/超时及失败清理；实际 WRAM 工作任务另验证多次复用只分配一份栈。
联网测试补充 42 个初始化/恢复场景，覆盖热点拒绝后成功、DHCP 失败、持续离线退避、运行中掉线、仅服务端失败、配置错误和延迟回调；真实音频队列模拟覆盖等待/上传/播放时取消，以及两组各 5 次重新连接。主机测试和成功构建不等于板上验证；需要实测断网恢复、反复 RESET、背景噪声、长时间多轮和内存余量。SDK bflb_dbg.h 的有符号比较警告仍存在。
