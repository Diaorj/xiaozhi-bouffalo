"""Regression for the SDK LCD direction-value return contract (success=1)."""

from pathlib import Path
import subprocess
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdlib.h>
#include "bflb_gpio.h"
#include "lcd.h"
#include "screen_test.h"

static int mode, draws, direction_calls;
static unsigned long pixels;
static unsigned long long pins;
static struct bflb_device_s device;
void XiaozhiLcdDbiGpioInit(void);

struct bflb_device_s *bflb_device_get_by_name(const char *name) {
    (void)name;
    return &device;
}
void bflb_gpio_init(struct bflb_device_s *dev, uint8_t pin, uint32_t flags) {
    (void)dev;
    (void)flags;
    assert(pin < 37);
    pins |= 1ULL << pin;
}
int lcd_init(void) {
    if (mode == 1) return -1;
    XiaozhiLcdDbiGpioInit();
    return 0;
}
int lcd_set_dir(uint8_t direction, uint8_t mirror) {
    ++direction_calls;
    assert(direction == 1 && mirror == 0);
    return mode == 2 ? -1 : direction;
}
int lcd_draw_area(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, lcd_color_t color) {
    (void)color;
    assert(x1 <= x2 && x2 < 480 && y1 <= y2 && y2 < 320);
    const unsigned long count = (x2 - x1 + 1UL) * (y2 - y1 + 1UL);
    assert(count % 2 == 0);
    pixels += count;
    ++draws;
    return 0;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    mode = atoi(argv[1]);
    const int result = RunScreenBringupTest();
    if (mode == 0) {
        assert(result == 0 && direction_calls == 1);
        assert(draws > 30 && pixels >= 480UL * 320UL);
        /* Preserve the audio bus and DVP clock; GPIO36 carries display DC. */
        assert((pins & ((1ULL << 1) | (1ULL << 3) | (1ULL << 4) |
                        (1ULL << 5) | (1ULL << 6) | (1ULL << 7) |
                        (1ULL << 11) | (1ULL << 20))) == 0);
        assert(pins & (1ULL << 36));
    } else {
        assert(result < 0 && draws == 0);
        assert(direction_calls == (mode == 2));
    }
    return 0;
}
'''


class ScreenBringupTests(unittest.TestCase):
    def test_success_direction_one_draws_and_errors_stop(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            (temp / 'bflb_gpio.h').write_text('''#pragma once
#include <stdint.h>
#include "lcd_conf_user.h"
struct bflb_device_s { int unused; };
struct bflb_device_s *bflb_device_get_by_name(const char *name);
void bflb_gpio_init(struct bflb_device_s *, uint8_t, uint32_t);
#define GPIO_FUNC_DBI_B 1
#define GPIO_ALTERNATE 2
#define GPIO_PULLUP 4
#define GPIO_SMT_EN 8
#define GPIO_DRV_2 16
''')
            (temp / 'lcd.h').write_text('''#pragma once
#include <stdint.h>
typedef uint16_t lcd_color_t;
int lcd_init(void);
int lcd_set_dir(uint8_t, uint8_t);
int lcd_draw_area(uint16_t, uint16_t, uint16_t, uint16_t, lcd_color_t);
''')
            (temp / 'harness.c').write_text(HARNESS)
            binary = temp / 'screen_test'
            subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-DBL616CL', '-I' + str(temp), '-I' + str(PORT),
                            str(PORT / 'screen_test.c'), str(temp / 'harness.c'),
                            '-o', str(binary)], check=True)
            for mode in range(3):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(binary), str(mode)],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual('SCREEN_PATTERN_SENT' in result.stdout, mode == 0)


if __name__ == '__main__':
    unittest.main()
