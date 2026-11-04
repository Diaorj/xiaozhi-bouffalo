#include "screen_test.h"

#include <stdint.h>
#include <stdio.h>

#include "bflb_gpio.h"
#include "lcd.h"

#if !defined(BL616CL)
#error "This U10 pin mapping is for the BL616CL audio/video board only"
#endif

/* BL616CL 0821 schematic, U2 -> U10; also matches the SDK ave_v10 overlay.
 * CS=GPIO0, WR=GPIO2, DC=BOOT/GPIO36, D0..7=GPIO12..19.
 * Do not use the generic bl616cldk LCD GPIO helper: GPIO1 is audio MCLK
 * and GPIO3 is routed to DVP_MCLK3. This module is write-only. */
void XiaozhiLcdDbiGpioInit(void)
{
    struct bflb_device_s *gpio = bflb_device_get_by_name("gpio");
    static const uint8_t pins[] = { 0, 2, 36, 12, 13, 14, 15, 16, 17, 18, 19 };

    for (unsigned int i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        bflb_gpio_init(gpio, pins[i], GPIO_FUNC_DBI_B | GPIO_ALTERNATE | GPIO_PULLUP |
                                        GPIO_SMT_EN | GPIO_DRV_2);
    }
}

int RunScreenBringupTest(void)
{
    /* The SDK LCD initializer expects all three device handles to exist. */
    if (bflb_device_get_by_name("gpio") == NULL ||
        bflb_device_get_by_name("dbi") == NULL ||
        bflb_device_get_by_name(LCD_DBI_DMA_NAME) == NULL) {
        printf("[E][SCREEN] SCREEN_DEVICE_MISSING: gpio/dbi/dma0_ch3\r\n");
        return -1;
    }

    printf("[I][SCREEN] ST7796_BEGIN: U10, DBI 8080 x8, RGB565; preserve audio GPIO1\r\n");
    if (lcd_init() != 0) {
        printf("[E][SCREEN] SCREEN_INIT_FAILED\r\n");
        return -1;
    }
    /* SDK lcd_set_dir() returns the selected direction (0..3), not a
     * zero-only success code. Landscape direction 1 is a successful result. */
    const int direction = lcd_set_dir(1, 0);
    if (direction < 0) {
        printf("[E][SCREEN] SCREEN_DIRECTION_FAILED: result=%d\r\n", direction);
        return -1;
    }
    printf("[I][SCREEN] SCREEN_CONFIG_DONE: direction=%d; drawing pattern\r\n", direction);

    /* Landscape 480 x 320. Each rectangle has an even pixel count: the SDK
     * RGB565 fill writes two pixels per 32-bit FIFO word. No full-frame RAM. */
    static const lcd_color_t colors[] = {
        0xF800, 0x07E0, 0x001F, 0x07FF, 0xF81F, 0xFFE0, 0xFFFF, 0x0000
    };
    for (uint16_t i = 0; i < 8; ++i) {
        lcd_draw_area(i * 60, 0, i * 60 + 59, 159, colors[i]);
        const uint16_t level = i * 31 / 7;
        const lcd_color_t gray = (level << 11) | ((level * 2) << 5) | level;
        lcd_draw_area(i * 60, 160, i * 60 + 59, 239, gray);
    }
    for (uint16_t row = 0; row < 2; ++row) {
        for (uint16_t col = 0; col < 12; ++col) {
            const lcd_color_t color = ((row + col) & 1) ? 0xFFFF : 0x0000;
            lcd_draw_area(col * 40, 240 + row * 40, col * 40 + 39, 279 + row * 40, color);
        }
    }
    /* A thin border reveals clipping; colored corners reveal rotation. */
    lcd_draw_area(0, 0, 479, 1, 0xFFFF);
    lcd_draw_area(0, 318, 479, 319, 0xFFFF);
    lcd_draw_area(0, 0, 1, 319, 0xFFFF);
    lcd_draw_area(478, 0, 479, 319, 0xFFFF);
    lcd_draw_area(2, 2, 17, 17, 0xF800);
    lcd_draw_area(462, 2, 477, 17, 0x07E0);
    lcd_draw_area(2, 302, 17, 317, 0x001F);
    lcd_draw_area(462, 302, 477, 317, 0xFFE0);

    printf("[I][SCREEN] SCREEN_PATTERN_SENT: 480x320; RGB bars, gray ramp, checkerboard\r\n");
    printf("[I][SCREEN] Visually verify colors/orientation; continuing voice startup\r\n");
    return 0;
}
