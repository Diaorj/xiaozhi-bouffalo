#ifndef XIAOZHI_LCD_CONF_USER_H
#define XIAOZHI_LCD_CONF_USER_H

/* U10 ST7796-DBI module: 8-bit 8080 bus, RGB565, native 320 x 480. */
#define LCD_DBI_ST7796
#define LCD_DBI_INTERFACE_TYPE 1
#define ST7796_DBI_PIXEL_FORMAT 1
#define ST7796_DBI_W 320
#define ST7796_DBI_H 480
#define ST7796_DBI_OFFSET_X 0
#define ST7796_DBI_OFFSET_Y 0
#define ST7796_DBI_COLOR_ORDER 0
#define ST7796_DBI_COLOR_REVERSAL 0
#define ST7796_DBI_DIR_MIRROR 0

#define LCD_DBI_WORK_MODE 3
#define LCD_DBI_GPIO_INIT_FUNC XiaozhiLcdDbiGpioInit
#define LCD_RGB_ORDER_MODE 0
/* Audio already owns DMA0 channels 0 and 1. */
#define LCD_DBI_DMA_NAME "dma0_ch3"
#define DBI_DBI_DATA_SIZE_MAX (ST7796_DBI_W * ST7796_DBI_H * 2)

/* U10 exposes neither reset nor backlight control. Use the SDK ST7796
 * software-reset sequence; the module has its own backlight circuitry.
 * GPIO12 / GPIO16 are data pins, not the generic example's reset/backlight. */
#define LCD_RESET_EN 0
#define LCD_BACKLIGHT_EN 0

#endif
