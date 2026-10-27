// Playback hardware extracted from the previously verified BL616CL demo.
// Only this C unit includes SDK codec/I2S/DMA headers; no Shell dependency.
#include "audio_sdk_bridge.h"
#include <stdbool.h>
#include <string.h>
#include "FreeRTOS.h"
#include "audio_codec_es8388.h"
#include "bflb_clock.h"
#include "bflb_dma.h"
#include "bflb_gpio.h"
#include "bflb_i2c.h"
#include "bflb_i2s.h"
#include "bl616cl_glb.h"
#include "event_groups.h"
#include "hardware/i2s_reg.h"
#include "memory_diagnostics.h"
#include "mm.h"
#include "pcm_ring.h"
#include "sdk_audio_codec.h"
#include "task.h"
#define DBG_TAG "SPEAKER"
#include "log.h"
#define CODEC_ADDR 0x10U
#define BLOCKS 8U
#define FRAMES 160U
#define CAPACITY 32000U
static ATTR_NOCACHE_NOINIT_RAM_SECTION uint32_t output[BLOCKS][FRAMES] __ALIGNED(32);
static ATTR_NOCACHE_NOINIT_RAM_SECTION struct bflb_dma_channel_lli_pool_s lli[BLOCKS] __ALIGNED(32);
static struct bflb_device_s *i2c, *i2s, *dma;
static audio_codec_dev_t codec;
static volatile uint32_t completed_blocks;
static int16_t* pcm_ring;
static uint32_t published, consumed, closed, cancelled, failed;
static unsigned volume;
static EventGroupHandle_t done;
static bool running;
static uint32_t load(const uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void store(uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
// ISR only increments a counter. It never logs or calls the RTOS.
static void dma_done(void* arg) {
    (void)arg;
    completed_blocks++;
}
static int codec_read(uint8_t reg, uint8_t* value) {
    struct bflb_i2c_msg_s messages[2] = {
        {.addr = CODEC_ADDR, .flags = I2C_M_NOSTOP, .buffer = &reg, .length = 1},
        {.addr = CODEC_ADDR, .flags = I2C_M_READ, .buffer = value, .length = 1}};
    return bflb_i2c_transfer(i2c, messages, 2);
}

/* Validate only documented control fields. Reserved bits may ignore writes or
 * read fixed values: preserve them rather than guessing their behavior.
 * ES8388 Rev 5.0 sections 6.1 / 6.2.7 / 6.3.3.
 */
static uint8_t codec_control_mask(uint8_t reg) {
    switch (reg) {
        case 0x00:
            return 0x7F; /* exclude self-clearing reset */
        case 0x01:
            return 0x3F; /* reference/bias power; bit 6 reserved */
        case 0x06:
            return 0xC3; /* PGA, mixer, ADC/DAC Vref low-power modes */
        case 0x03:
            return 0xFC; /* ADC and analog input power */
        case 0x04:
            return 0xFC; /* DAC and output power */
        case 0x0F:
        case 0x19:
            return 0xEC; /* ramp rate, soft ramp, linked gain, mute */
        case 0x0A:
            return 0xFC;
        case 0x0B:
            return 0x9C;
        case 0x0D:
            return 0x3F;
        case 0x0E:
            return 0xF0;
        case 0x17:
            return 0x7F; /* DAC serial format */
        case 0x18:
        case 0x26:
            return 0x3F;
        case 0x27:
        case 0x2A:
            return 0xF8;
        case 0x2B:
            return 0xFC;
        case 0x2E:
        case 0x2F:
        case 0x30:
        case 0x31:
            return 0x3F;
        default:
            return 0xFF;
    }
}

static int codec_write_checked(uint8_t reg, uint8_t value) {
    uint8_t mask = codec_control_mask(reg);
    uint8_t old_value = 0;
    if (codec_read(reg, &old_value) != 0) {
        LOG_E("CODEC_READ_FAILED: reg=0x%02x\r\n", reg);
        return -1;
    }
    uint8_t write_value = (old_value & (uint8_t)~mask) | (value & mask);
    uint8_t pair[2] = {reg, write_value};
    uint8_t readback = 0;
    struct bflb_i2c_msg_s message = {
        .addr = CODEC_ADDR, .flags = 0, .buffer = pair, .length = sizeof(pair)};
    if (bflb_i2c_transfer(i2c, &message, 1) != 0 || codec_read(reg, &readback) != 0 ||
        ((readback ^ value) & mask) != 0) {
        LOG_E("CODEC_CONFIG_FAILED: reg=0x%02x expected=0x%02x actual=0x%02x mask=0x%02x\r\n", reg,
              value, readback, mask);
        return -1;
    }
    return 0;
}

static int setup_hardware(void) {
    struct bflb_device_s* gpio = bflb_device_get_by_name("gpio");
    i2c = bflb_device_get_by_name("i2c0");
    i2s = bflb_device_get_by_name("i2s0");
    dma = bflb_device_get_by_name("dma0_ch0");
    if (!gpio || !i2c || !i2s || !dma) {
        LOG_E("AUDIO_DEVICE_MISSING\r\n");
        return -1;
    }

    uint32_t pins = GPIO_ALTERNATE | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_1;
    bflb_gpio_init(gpio, GPIO_PIN_11, GPIO_FUNC_I2C0 | pins);
    bflb_gpio_init(gpio, GPIO_PIN_20, GPIO_FUNC_I2C0 | pins);
    bflb_i2c_init(i2c, 100000);
    uint8_t probe = 0;
    if (codec_read(0x00, &probe) != 0) {
        LOG_E("CODEC_NOT_FOUND: I2C addr=0x10 SDA=11 SCL=20\r\n");
        return -1;
    }
    LOG_I("ES8388 I2C response OK at 0x10 (not a chip-ID check)\r\n");

    /* Same reference as the verified demo: 320 MHz / 26.
     * 16 kHz nominal, actual LRCK about 16025.6 Hz; do not change the Wi-Fi PLL.
     */
    GLB_Set_I2S_CLK(ENABLE, 25, GLB_I2S_DI_SEL_I2S_DI_INPUT, GLB_I2S_DO_SEL_I2S_DO_OUTPT);
    GLB_Set_Chip_Clock_Out1_Sel(GLB_CHIP_CLK_OUT_1_I2S_REF_CLK);
    bflb_gpio_init(gpio, GPIO_PIN_1, GPIO_FUNC_CLKOUT | pins);
    bflb_gpio_init(gpio, GPIO_PIN_4, GPIO_FUNC_I2S | pins);
    bflb_gpio_init(gpio, GPIO_PIN_5, GPIO_FUNC_I2S | pins);
    bflb_gpio_init(gpio, GPIO_PIN_7, GPIO_FUNC_I2S | pins);

    codec = (audio_codec_dev_t){.name = "ES8388",
                                .i2c = i2c,
                                .drv = &audio_codec_es8388_driver,
                                .type = AUDIO_CODEC_TYPE_ES8388,
                                .i2c_addr = CODEC_ADDR};
    audio_codec_cfg_t config = {.sample_rate_hz = 16000,
                                .channels = 2,
                                .i2s_fmt = AUDIO_CODEC_I2S_FMT_I2S,
                                .bits = AUDIO_CODEC_BITS_16,
                                .codec_master = false,
                                .enable_adc = false,
                                .enable_dac = true,
                                .work_mode = AUDIO_CODEC_WORK_MODE_PLAYBACK};
    if (audio_codec_init(&codec, &config) != 0 || audio_codec_set_mute(&codec, true) != 0) {
        LOG_E("CODEC_INIT_FAILED\r\n");
        return -1;
    }

    /* Correct the legacy playback defaults explicitly and verify readback.
     * This board feeds ROUT1/ROUT2 into a differential amplifier. Equal signals
     * would cancel: ROUT1=0 dB, ROUT2=-45 dB gives a single-ended playback signal.
     * Only the right outputs are powered; microphone and analog bypass stay off.
     */
    static const uint8_t registers[][2] = {
        {0x19, 0x24},                             /* soft ramp, mute */
        {0x08, 0x00},                             /* codec slave */
        {0x00, 0x35},                             /* same rate, DAC MCLK, references */
        {0x03, 0xFC},                             /* ADC/inputs off */
        {0x0F, 0x24},                             /* ADC mute */
        {0x17, 0x18},                             /* 16-bit standard I2S */
        {0x18, 0x06},                             /* MCLK/LRCK = 768 */
        {0x1A, 0x00}, {0x1B, 0x00},               /* digital attenuation 0 dB; PCM volume below */
        {0x26, 0x00}, {0x27, 0x00}, {0x2A, 0xB8}, /* DACR only, no analog loopback; SDK keeps
                                                     reserved 0x28/29 defaults */
        {0x2B, 0x80}, {0x2E, 0x00}, {0x2F, 0x1E}, {0x30, 0x00},
        {0x31, 0x00}, {0x04, 0x94}, /* DACL off, DACR on, ROUT1 + ROUT2 on */
        {0x02, 0xAA}                /* DAC state machine on, ADC off */
    };
    for (size_t n = 0; n < sizeof(registers) / sizeof(registers[0]); n++) {
        if (codec_write_checked(registers[n][0], registers[n][1]) != 0)
            return -1;
    }
    LOG_I("CODEC_CONFIG_OK: 16-bit I2S, slave, DAC only, muted\r\n");

    struct bflb_i2s_config_s i2s_config = {
        .bclk_freq_hz = 16000U * 16U * 2U,
        .role = I2S_ROLE_MASTER,
        .format_mode = I2S_MODE_LEFT_JUSTIFIED,
        .channel_mode = I2S_CHANNEL_MODE_NUM_2,
        .frame_width = I2S_SLOT_WIDTH_16,
        .data_width = I2S_SLOT_WIDTH_16,
        .fs_offset_cycle = 1, /* Philips standard I2S has one-bit delay */
        .tx_fifo_threshold = 4,
        .rx_fifo_threshold = 0};
    bflb_i2s_init(i2s, &i2s_config);
    uint32_t ref_clock = bflb_clk_get_peripheral_clock(BFLB_DEVICE_TYPE_I2S, i2s->idx);
    uint32_t divider = (ref_clock * 10U / i2s_config.bclk_freq_hz + 5U) / 10U;
    if (divider < 2U)
        divider = 2U;
    LOG_I("I2S ref=%lu Hz, calculated LRCK=%lu Hz, MCLK GPIO1\r\n", (unsigned long)ref_clock,
          (unsigned long)(ref_clock / divider / 32U));
    bflb_i2s_feature_control(i2s, I2S_CMD_CHANNEL_LR_MERGE, true);
    bflb_i2s_link_txdma(i2s, true);

    struct bflb_dma_channel_config_s dma_config = {.direction = DMA_MEMORY_TO_PERIPH,
                                                   .src_req = DMA_REQUEST_NONE,
                                                   .dst_req = DMA_REQUEST_I2S_TX,
                                                   .src_addr_inc = DMA_ADDR_INCREMENT_ENABLE,
                                                   .dst_addr_inc = DMA_ADDR_INCREMENT_DISABLE,
                                                   .src_burst_count = DMA_BURST_INCR1,
                                                   .dst_burst_count = DMA_BURST_INCR1,
                                                   .src_width = DMA_DATA_WIDTH_32BIT,
                                                   .dst_width = DMA_DATA_WIDTH_32BIT};
    bflb_dma_channel_init(dma, &dma_config);
    bflb_dma_channel_tcint_clear(dma);
    bflb_dma_channel_irq_attach(dma, dma_done, NULL);
    return 0;
}

static void cleanup(void) {
    if (codec.drv && i2c)
        audio_codec_set_mute(&codec, true);
    vTaskDelay(pdMS_TO_TICKS(30));
    if (dma) {
        bflb_dma_channel_stop(dma);
        bflb_dma_channel_tcint_mask(dma, true);
    }
    if (i2s) {
        bflb_i2s_feature_control(i2s, I2S_CMD_DISABLE_CONTROL, I2S_ARG_CMD_MASTER | I2S_ARG_CMD_TX);
        bflb_i2s_link_txdma(i2s, false);
        bflb_i2s_feature_control(i2s, I2S_CMD_CLEAR_TX_FIFO, 0);
    }
}
static unsigned pack(uint32_t* out, uint32_t offset) {
    return PcmRingPack(pcm_ring, CAPACITY, load(&published), offset, out, FRAMES, volume);
}
static int play(void) {
    memset(output, 0, sizeof(output));
    uint32_t read = 0, target = UINT32_MAX;
    for (unsigned i = 3; i < BLOCKS; i++) {
        read += pack(output[i], read);
        if (load(&closed) && read == load(&published) && target == UINT32_MAX)
            target = i + 1;
    }
    store(&consumed, read);
    struct bflb_dma_channel_lli_transfer_s transfers[BLOCKS];
    for (unsigned i = 0; i < BLOCKS; i++)
        transfers[i] =
            (struct bflb_dma_channel_lli_transfer_s){.src_addr = (uint32_t)(uintptr_t)output[i],
                                                     .dst_addr = DMA_ADDR_I2S_TDR,
                                                     .nbytes = sizeof(output[i])};
    bflb_dma_channel_tcint_clear(dma);
    if (bflb_dma_channel_lli_reload(dma, lli, BLOCKS, transfers, BLOCKS) != BLOCKS)
        return -1;
    bflb_dma_channel_lli_link_head(dma, lli, BLOCKS);
    completed_blocks = 0;
    uint32_t filled = 0;
    TickType_t progress = xTaskGetTickCount(), packet_progress = progress;
    uint32_t last_published = load(&published);
    bflb_dma_channel_start(dma);
    bflb_i2s_feature_control(i2s, I2S_CMD_ENABLE_CONTROL, I2S_ARG_CMD_MASTER | I2S_ARG_CMD_TX);
    vTaskDelay(pdMS_TO_TICKS(30));
    if (volume && audio_codec_set_mute(&codec, false) != 0)
        return -1;
    LOG_I("SPEAKER_PLAYING: PCM 16000 Hz, volume=%u\r\n", volume);
    while (completed_blocks < target) {
        if (load(&cancelled))
            return -1;
        uint32_t blocks = completed_blocks;
        if (blocks - filled >= BLOCKS) {
            LOG_E("DMA_REFILL_LATE\r\n");
            return -1;
        }
        while (filled < blocks) {
            read += pack(output[filled % BLOCKS], read);
            __sync_synchronize();
            store(&consumed, read);
            if (completed_blocks - filled >= BLOCKS)
                return -1;
            if (load(&closed) && read == load(&published) && target == UINT32_MAX)
                target = filled + BLOCKS + 1;
            filled++;
            progress = xTaskGetTickCount();
            blocks = completed_blocks;
        }
        if (getreg32(i2s->reg_base + I2S_FIFO_CONFIG_0_OFFSET) &
            (I2S_TX_FIFO_OVERFLOW | I2S_TX_FIFO_UNDERFLOW))
            return -1;
        if (load(&published) != last_published) {
            last_published = load(&published);
            packet_progress = xTaskGetTickCount();
        }
        if (xTaskGetTickCount() - progress > pdMS_TO_TICKS(500) ||
            (!load(&closed) && xTaskGetTickCount() - packet_progress > pdMS_TO_TICKS(15000)))
            return -1;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return 0;
}
static void output_task(void* arg) {
    (void)arg;
    TickType_t started = xTaskGetTickCount();
    while (!load(&closed) && !load(&cancelled) && load(&published) < 3200) {
        if (xTaskGetTickCount() - started > pdMS_TO_TICKS(15000)) {
            store(&failed, 1);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (load(&cancelled) || load(&failed) || play() != 0)
        store(&failed, 1);
    cleanup();
    MemoryDiagnosticsStage("speaker_task_done");
    xEventGroupSetBits(done, 1);
    vTaskDelete(NULL);
}
void* AudioSdkAllocateOpusState(size_t bytes) {
    void* state = kmalloc(bytes, MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_CACHE);
    if (state)
        LOG_I("OPUS_STATE_PSRAM: bytes=%lu ptr=%p\r\n", (unsigned long)bytes, state);
    return state;
}
void AudioSdkFreeOpusState(void* state) {
    if (state)
        kfree(state);
}

int AudioSdkOpen(unsigned requested_volume) {
    if (running || requested_volume > 100)
        return -1;
    MemoryDiagnosticsStage("speaker_open");
    if (!done)
        done = xEventGroupCreate();
    if (!done)
        return -1;
    pcm_ring = kmalloc(CAPACITY * sizeof(int16_t), MM_FLAG_HEAP_PSRAM_0);
    if (!pcm_ring) {
        LOG_E("AUDIO_PSRAM_ALLOC_FAILED\r\n");
        return -1;
    }
    store(&published, 0);
    store(&consumed, 0);
    store(&closed, 0);
    store(&cancelled, 0);
    store(&failed, 0);
    volume = requested_volume;
    if (setup_hardware() != 0) {
        cleanup();
        kfree(pcm_ring);
        pcm_ring = NULL;
        return -1;
    }
    xEventGroupClearBits(done, 1);
    MemoryDiagnosticsStage("before_speaker_task_create");
    LOG_I("SPEAKER_TASK_CREATE: stack_bytes=%lu\r\n", (unsigned long)(1536 * sizeof(StackType_t)));
    if (xTaskCreate(output_task, "speaker_dma", 1536, NULL, 8, NULL) != pdPASS) {
        cleanup();
        kfree(pcm_ring);
        pcm_ring = NULL;
        return -1;
    }
    running = true;
    MemoryDiagnosticsStage("speaker_task_created");
    return 0;
}
int AudioSdkWrite(const int16_t* pcm, size_t frames) {
    if (!running || !pcm || !frames || load(&closed))
        return -1;
    TickType_t progress = xTaskGetTickCount();
    size_t offset = 0;
    while (offset < frames) {
        if (load(&failed) || load(&cancelled))
            return -1;
        uint32_t write = load(&published), read = load(&consumed),
                 space = CAPACITY - (write - read);
        if (write - read > CAPACITY)
            return -1;
        if (!space) {
            if (xTaskGetTickCount() - progress > pdMS_TO_TICKS(2000))
                return -1;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        size_t count = frames - offset;
        if (count > space)
            count = space;
        size_t contiguous = CAPACITY - write % CAPACITY;
        if (count > contiguous)
            count = contiguous;
        memcpy(pcm_ring + write % CAPACITY, pcm + offset, count * sizeof(int16_t));
        store(&published, write + count);
        offset += count;
        progress = xTaskGetTickCount();
    }
    return (int)frames;
}
int AudioSdkFinish(void) {
    if (!running)
        return -1;
    store(&closed, 1);
    xEventGroupWaitBits(done, 1, pdFALSE, pdTRUE, portMAX_DELAY);
    int result = load(&failed) ? -1 : 0;
    kfree(pcm_ring);
    pcm_ring = NULL;
    running = false;
    return result;
}
void AudioSdkAbort(void) {
    if (running) {
        store(&cancelled, 1);
        (void)AudioSdkFinish();
    }
}
