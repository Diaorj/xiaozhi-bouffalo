# Bouffalo / BL616CL 移植

保留小智原工程结构，用 Bouffalo SDK 构建 BL616CL 固件。官方 OTA、绑定、语音输入输出、流式上传、VAD 和播放后恢复监听已在板上验证。连续对话与断网恢复基线为 `0b2f56e`，已推送并收到用户板测正常反馈。原 Application 的主循环、对象持有和监听许可已收到用户板测正常反馈。本轮新增 ST7796 上电测试图，待实物确认。

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

关闭占用 COM7 的终端、进入下载模式后，用相同参数执行 `make flash ... COMX=COM7`，退出下载模式再 RESET。固件为 `build/build_out/xiaozhi_bouffalo_bl616cl.bin`，标记 `screen-20261104-v10`。
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
APP_RESUME_LISTENING: playback and DMA finished
APP_LISTEN_GRANTED
AUTO_LISTENING: ...
```

默认 `XIAOZHI_AUTO_CHAT=ON`，保留同一条服务端连接连续问答。设为 OFF 可只运行一轮 VAD 问答；`XIAOZHI_VOICE_INPUT_TEST=OFF` 恢复短唤醒词问候测试。
等待期间每 20 秒发送 WebSocket Ping，30 秒未收到 Pong 则终止并释放连接。失败的录音或回答会丢弃，恢复后重新监听，不自动重放问题。

连接热点、DHCP 或服务端失败后，按 3、6、12、24、30 秒间隔重试，之后每 30 秒尝试一次。Wi-Fi 连接成功后复位其重试间隔。运行中断网先停止录音/播放，释放旧 WebSocket/TLS，再重新获取 IP、执行联网检查和 OTA，连接成功后回到 `AUTO_LISTENING`。只发生互联网或服务端失败时保留当前 Wi-Fi；同一网络任务负责恢复，不重复启动驱动或管理器。

SDK 明确报告参数错误时停止重试并打印 `WIFI_RETRY_STOPPED`，需要修改本地配置；空 SSID 仍只初始化，不连接。RF、驱动等初始化失败仍保留原失败提示，不反复初始化硬件。服务端重试复用成功取得的 OTA 配置，超过 60 秒才重新刷新；失败的 OTA 请求按退避间隔重试。

测试恢复：反复 RESET 后观察是否进入监听；关闭热点，等待出现 `WIFI_OFFLINE` / `NETWORK_RETRY`，重新开启后应依次出现 `WIFI_READY`、`ORIGINAL_WS_OK`、`AUTO_LISTENING`。

## U10 屏幕点亮测试

默认 `XIAOZHI_SCREEN_TEST=ON`。上电在任务启动前，调用 SDK ST7796 驱动，横屏显示 480×320 测试图：上部从左到右为红、绿、蓝、青、紫、黄、白、黑；中部黑到白灰阶；底部黑白棋盘格；外圈白边，四角依次红、绿、蓝、黄。图案保留，随后正常联网和语音对话。

引脚按《BL616CL 0821》原理图 U2/U10 与 SDK `bl616cl_ave_v10` 的 DBI 配置核对：

| 信号 | GPIO | U10 引脚 |
| --- | --- | --- |
| CS | 0 | 10 |
| WR | 2 | 9 |
| DC（BOOT） | 36 | 12 |
| D0～D7 | 12～19 | 13～20 |

项目 `lcd_conf_user.h` 选择 ST7796、8 位 8080、RGB565；`screen_test.c` 单独配置这些引脚，保留音频 MCLK GPIO1 和 I2S/I2C。SDK 初始化使用 DMA0 通道3，音频仍用0/1；本次测试同步填充 FIFO，没有整屏缓冲和新增任务。链接静态内部 RAM 增加32字节，PSRAM占用不变。

U10 没有独立复位或背光控制信号，使用 SDK 的 ST7796 软件复位，屏模块背光由其板上电路控制。不要套用通用例程的 GPIO12 复位、GPIO16 背光设置，它们在此板上都是数据脚。上电/RESET 时松开 BOOT 键。

修正首版点屏中对 `lcd_set_dir()` 返回值的误判：SDK 成功时返回方向值0～3，横屏返回1；先前把1当错误，导致提前退出、未绘图。现在仅负值报方向错误，并增加 `SCREEN_CONFIG_DONE: direction=1` 日志。

串口 `SCREEN_PATTERN_SENT` 仅表示指令已发出；接口为写入路径，不能据此确认屏型号或真实显示成功。板测检查颜色、方向、完整白框，并继续测试语音录放。若红蓝互换，调整 `ST7796_DBI_COLOR_ORDER`；若方向不合适，调整 `lcd_set_dir()`。没有显示时先核对插接方向与背光，再结合日志定位。

使用相同编译参数加 `XIAOZHI_SCREEN_TEST=OFF` 可跳过点屏；默认开启。当前是硬件显示验证，确认后再接原小智 Display 的状态、对话文字和表情。

## Application 接入

启动任务现在调用原 `Application::Initialize()` / `Run()`，删除了状态机演示和独立心跳循环。共享的 Run、Schedule、事件分发与原 DeviceStateMachine 真正运行；平台初始化和处理差异通过 `ApplicationRuntime` 接口提供。

Application 持有唯一的 AudioService 和 WebsocketProtocol。工作任务只借用对象并上报阶段；状态修改通过 Schedule 回到 Application 任务执行。DMA 排空、播放任务结束后，上报 PlaybackFinished，Application 才发下一轮录音许可。重连时清除旧许可，握手成功前不采集。

状态随真实阶段变化：starting → activating → idle → connecting → listening → idle（等待回答）→ speaking → idle → listening。原枚举没有独立的 thinking 状态，等待回答另打印 APP_WAITING_REPLY。

本次沿用原 4 KiB 心跳任务的栈作为 Application 主循环，网络工作任务仍为 12 KiB，没有再增加一份应用任务。`application_listen_grant` 的 MEM 日志用于板上检查主循环栈余量。

这是 Application 的平台接入阶段；原 ActivationTask / InitializeProtocol 的完整业务、显示、MCP、提示音和资源包尚未整体接入。DNS/OTA/连接收尾和半双工音频 I/O 仍由已验证的博流工作任务执行，后续继续迁移。原 ESP Application 分支保持原路径，未进行 ESP-IDF 整机编译。

## 架构与内存

- 原 `Ota`、`Protocol`、`WebsocketProtocol` 处理版本检查、认证和协议。语音使用原 listen/start(manual)、SendAudio、listen/stop；保留同一 session，不在每个回答后重新认证。
- 原 `AudioService` 保留编码/发送/解码/播放队列，平台 AudioEncoder / AudioDecoder 调用 SDK Opus。公共 AudioService 有平台分支改动，由 Application 持有；AudioEngine 仍未整体接入。
- `VoiceInputSession` 负责 VAD、前缓冲和采集生产者；网络工作任务从有界 PSRAM PCM 环取帧，再持续排空原发送队列；下一轮开始由 Application 许可。接收回调只入队或置标志，停止操作由网络任务执行。
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
联网测试补充 42 个初始化/恢复场景，覆盖热点拒绝后成功、DHCP 失败、持续离线退避、运行中掉线、仅服务端失败、配置错误和延迟回调；真实音频队列模拟覆盖等待/上传/播放时取消，以及两组各 5 次重新连接。本轮 21 项移植回归全部通过；Application 测试补充跨线程事件、真实状态顺序、监听许可及实际运行接口联合 Opus 三轮问答、掉线恢复、录音/播放暂停后再恢复。原项目脚本 111 项检查通过（1 项因本地依赖缺失跳过）。主机测试和成功构建不等于板上验证；需要实测新的 Application 栈水位、连续问答、断网恢复和内存余量。SDK bflb_dbg.h 的有符号比较警告仍存在。
