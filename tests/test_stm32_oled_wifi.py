"""Run the real firmware UI/parser code on a host C compiler.

Set STM32_FIRMWARE_ROOT for another checkout. Peripheral writes are stubbed;
these checks do not flash hardware or prove physical button/OLED behavior.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(os.environ.get('STM32_FIRMWARE_ROOT',
                          'D:/STM32CubeMx/hal_stm32/科教协同下位机-1'))


def function(source, name):
    start = re.search(r'^static [^\n]+\b' + name + r'\([^;]*?\)\s*\{', source, re.M)
    if start is None:
        raise AssertionError('Missing firmware function: ' + name)
    opening = source.index('{', start.start())
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start.start():end]


HAL_STUB = r'''
#ifndef HOST_HAL_H
#define HOST_HAL_H
#include <stdint.h>
typedef int I2C_HandleTypeDef;
typedef int HAL_StatusTypeDef;
#define HAL_OK 0
#define HAL_ERROR 1
static int HAL_I2C_Master_Transmit(I2C_HandleTypeDef *a, int b, uint8_t *c, int d, int e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; return HAL_OK; }
static int HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *a, int b, int c, int d)
{ (void)a; (void)b; (void)c; (void)d; return HAL_OK; }
#endif
'''

PREAMBLE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "OLED.h"
#include "OLED_Text.h"
#include "gimbal_button.h"
I2C_HandleTypeDef hi2c1;
#define GPIOC 1
#define GPIOB 2
#define GPIO_PIN_13 13
#define GPIO_PIN_14 14
#define GPIO_PIN_15 15
#define GPIO_PIN_3 3
#define GPIO_PIN_4 4
#define GPIO_PIN_RESET 0
#define GPIO_PIN_SET 1
#define GIMBAL_MANUAL_STEP_DEFAULT_TENTHS 5
#define GIMBAL_MANUAL_STEP_MIN_TENTHS 1
#define GIMBAL_MANUAL_STEP_MAX_TENTHS 100
static uint32_t now;
static uint32_t HAL_GetTick(void) { return now; }
static uint32_t __get_PRIMASK(void) { return 0; }
static void __disable_irq(void) {}
static void __set_PRIMASK(uint32_t mask) { (void)mask; }
static int key_down;
static int HAL_GPIO_ReadPin(int port, int pin)
{ (void)port; (void)pin; return key_down ? GPIO_PIN_RESET : GPIO_PIN_SET; }
static void HAL_GPIO_WritePin(int port, int pin, int state)
{ (void)port; (void)pin; (void)state; }
typedef struct { uint8_t enabled; } HostAxis;
static HostAxis axis0, axis1;
static uint32_t last_link_time, track_time, wifi_time, wifi_scroll_start;
static uint8_t link_received, track_valid, work_mode, oled_page, wifi_state, wifi_seen;
static char wifi_ssid[33];
static int16_t track_offset_x, track_offset_y;
static uint8_t tracking_enable, emergency_stop, system_fault;
static uint8_t calibration_save_failed, calibration_saved_notice;
static uint32_t calibration_saved_time;
static unsigned centers, enables, disables, commands;
static uint32_t beep;
static void CenterGimbal(void) { centers++; }
static void Buzzer_On(uint32_t duration) { beep = duration; }
static uint8_t Button_Pressed(int port, int pin, int index)
{ (void)port; (void)pin; (void)index; return 0; }
static uint8_t Motion_EnsureEnabled(void) { enables++; return 1; }
static void DisableBothMotors(void) { disables++; }
static uint8_t EnableBothMotorsHoldingCurrent(void) { enables++; return 1; }
static void Proto_QueueCommand(uint8_t code, uint16_t step)
{ (void)code; (void)step; commands++; }
static uint8_t Status_ShowFault(void)
{ if (!system_fault) return 0; OLED_SetLine(1, OLED_TXT_FAULT); return 1; }
static void Status_SetAngle(uint8_t line, const char *name, const HostAxis *axis)
{ (void)name; (void)axis; OLED_SetLine(line, "ANGLE"); }
'''

CHECKS = r'''
static void expect_row(unsigned line, const char *text)
{
    unsigned i;
    for (i = 0; i < 16; i++)
        assert(oled_target[line - 1][i] == (i < strlen(text) ? (unsigned char)text[i] : ' '));
}
static void sample(uint32_t tick, int down)
{ now = tick; key_down = down; HandleButtons(); }
static void buttons(void)
{
    GimbalButton key = {0};
    assert(!GimbalButton_Update(&key, 0, 0));
    assert(!GimbalButton_Update(&key, 1, 10));
    assert(!GimbalButton_Update(&key, 0, 20)); /* short noise pulse */
    assert(!GimbalButton_Update(&key, 0, 60));
    assert(!GimbalButton_Update(&key, 1, 100));
    assert(!GimbalButton_Update(&key, 1, 130));
    assert(!GimbalButton_Update(&key, 0, 1099)); /* 999ms is short */
    assert(GimbalButton_Update(&key, 0, 1129) == GIMBAL_BUTTON_SHORT);
    assert(!GimbalButton_Update(&key, 1, 1200));
    assert(!GimbalButton_Update(&key, 1, 1230));
    assert(GimbalButton_Update(&key, 1, 2200) == GIMBAL_BUTTON_LONG);
    assert(!GimbalButton_Update(&key, 1, 5000));
    assert(!GimbalButton_Update(&key, 0, 5010));
    assert(!GimbalButton_Update(&key, 0, 5040)); /* release cannot turn page */
    memset(&key, 0, sizeof(key));
    assert(!GimbalButton_Update(&key, 1, 0)); /* held at startup */
    assert(!GimbalButton_Update(&key, 1, 3000));
    assert(!GimbalButton_Update(&key, 0, 3010));
    assert(!GimbalButton_Update(&key, 0, 3040));
    assert(!GimbalButton_Update(&key, 1, 4000));
    assert(!GimbalButton_Update(&key, 1, 4030));
    assert(!GimbalButton_Update(&key, 0, 5000));
    assert(GimbalButton_Update(&key, 0, 5030) == GIMBAL_BUTTON_LONG);
    memset(&key, 0, sizeof(key));
    assert(!GimbalButton_Update(&key, 0, UINT32_MAX - 100));
    assert(!GimbalButton_Update(&key, 1, UINT32_MAX - 50));
    assert(!GimbalButton_Update(&key, 1, UINT32_MAX - 20));
    assert(GimbalButton_Update(&key, 1, 949) == GIMBAL_BUTTON_LONG);

    sample(0, 0); sample(100, 1); sample(130, 1); sample(250, 0); sample(280, 0);
    assert(oled_page == 1 && work_mode == 0 && !centers && !enables && !disables);
    assert(beep == 30);
    sample(400, 1); sample(430, 1); sample(1400, 1);
    assert(oled_page == 1 && work_mode == 1 && centers == 1 && !enables);
    sample(5000, 1); sample(5010, 0); sample(5040, 0);
    assert(oled_page == 1 && work_mode == 1 && centers == 1);
    sample(5200, 1); sample(5230, 1); sample(5350, 0); sample(5380, 0);
    assert(oled_page == 0 && work_mode == 1 && centers == 1);
}
static void scrolling(void)
{
    unsigned offset;
    const char *name = "ABCDEFGHIJKLMNOPQRSTUVWXYZ123456";
    OLED_SetLine(1, "KEEP"); OLED_SetLine(3, "KEEP");
    OLED_SetScrollingLine(2, "Short Network", 999);
    expect_row(2, "Short Network");
    for (offset = 0; offset < 35; offset++) {
        unsigned i;
        OLED_SetScrollingLine(2, name, offset);
        for (i = 0; i < 16; i++) {
            unsigned pos = (offset + i) % 35;
            assert(oled_target[1][i] == (pos < 32 ? (unsigned char)name[pos] : ' '));
        }
    }
    OLED_SetScrollingLine(2, name, 35);
    expect_row(2, "ABCDEFGHIJKLMNOP");
    OLED_SetScrollingLine(2, "", 999); expect_row(2, "");
    expect_row(1, "KEEP"); expect_row(3, "KEEP");
    /* At a Chinese boundary, only a complete two-slot glyph may be drawn. */
    for (offset = 0; offset < 22; offset++) {
        unsigned i;
        OLED_SetScrollingLine(2, "123456789012345" OLED_TXT_WIFI_CONNECTED "XYZ", offset);
        for (i = 0; i < 16; i++) {
            uint16_t glyph = oled_target[1][i];
            if (glyph & OLED_RIGHT_HALF) assert(i > 0 && oled_target[1][i - 1] == (glyph & ~OLED_RIGHT_HALF));
            else if (glyph >= OLED_CHINESE_BASE) assert(i < 15 && oled_target[1][i + 1] == (glyph | OLED_RIGHT_HALF));
        }
    }
}
static void protocol_and_pages(void)
{
    uint8_t payload[33]; unsigned i;
    payload[0] = WIFI_STATE_CONNECTED;
    memcpy(payload + 1, "ABCDEFGHIJKLMNOPQRSTUVWXYZ123456", 32);
    now = 100;
    /* Feed real framing/parser in fragments, including corrupted CRC. */
    { uint8_t wire[38] = {0xA5, 0x5A, 34, PROTO_TYPE_WIFI};
      memcpy(wire + 4, payload, 33); wire[37] = crc8_calc(wire + 2, 35);
      for (i = 0; i < 37; i++) Proto_ParseByte(wire[i]);
      assert(!wifi_seen); Proto_ParseByte(wire[37] ^ 1); assert(!wifi_seen);
      for (i = 0; i < 38; i++) Proto_ParseByte(wire[i]); }
    assert(wifi_seen && wifi_time == 100 && wifi_state == WIFI_STATE_CONNECTED);
    assert(strlen(wifi_ssid) == 32 && !link_received && !last_link_time && !commands);
    oled_page = 1; Status_Update();
    expect_row(2, "ABCDEFGHIJKLMNOP");
    now = 1599; Status_Update(); expect_row(2, "ABCDEFGHIJKLMNOP");
    now = 1600; Status_Update(); expect_row(2, "BCDEFGHIJKLMNOPQ");
    now = 2100; Status_Update(); expect_row(2, "CDEFGHIJKLMNOPQR");
    now = 7101; Status_Update(); expect_row(2, "--");
    now = 7200; Proto_Dispatch(PROTO_TYPE_WIFI, payload, 33); Status_Update();
    expect_row(2, "ABCDEFGHIJKLMNOP"); /* fresh name resets scrolling */
    payload[0] = WIFI_STATE_DISCONNECTED; Proto_Dispatch(PROTO_TYPE_WIFI, payload, 1);
    Status_Update(); expect_row(2, "--"); assert(!wifi_ssid[0]);
    payload[0] = 3; Proto_Dispatch(PROTO_TYPE_WIFI, payload, 1);
    assert(wifi_state == WIFI_STATE_DISCONNECTED);
    payload[0] = WIFI_STATE_CONNECTED; Proto_Dispatch(PROTO_TYPE_WIFI, payload, 1);
    assert(wifi_state == WIFI_STATE_DISCONNECTED);
    Proto_Dispatch(PROTO_TYPE_HEARTBEAT, payload, 0);
    assert(link_received && last_link_time == now);
    payload[1] = 0; payload[2] = 10; payload[3] = 'Z';
    Proto_Dispatch(PROTO_TYPE_WIFI, payload, 4); assert(!strcmp(wifi_ssid, "??Z"));
    Status_Update(); expect_row(2, "??Z");
    oled_page = 0; Status_Update(); expect_row(2, "ANGLE");
    oled_page = 1; emergency_stop = 1; Status_Update();
    assert(oled_target[0][0] == ' ' && oled_target[0][4] >= OLED_CHINESE_BASE);
    emergency_stop = 0; system_fault = 1; Status_Update();
    assert(oled_target[0][0] >= OLED_CHINESE_BASE);
    system_fault = 0; now = UINT32_MAX - 20; Proto_Dispatch(PROTO_TYPE_WIFI, payload, 4);
    now = 40; Status_Update(); expect_row(2, "??Z");
}
int main(void)
{ buttons(); scrolling(); protocol_and_pages(); puts("firmware button, marquee, UART and page checks passed"); return 0; }
'''


@unittest.skipUnless((ROOT / 'Core/Src/main_gimbal.c').exists() and shutil.which('gcc'),
                     'Requires the STM32 checkout and host gcc')
class FirmwareUiTests(unittest.TestCase):
    def test_real_firmware_ui_and_protocol(self):
        main = (ROOT / 'Core/Src/main_gimbal.c').read_text(encoding='utf-8-sig')
        defines = '\n'.join(line for line in main.splitlines()
                            if re.match(r'#define (PROTO_|WIFI_|KEY_|LED_|LINK_|TRACK_DATA_|CMD_)', line))
        names = ('crc8_calc', 'Proto_Dispatch', 'Proto_ParseByte', 'HandleButtons',
                 'Status_ShowWifi', 'Status_Update')
        table = main[main.index('static const uint8_t crc8_table['):]
        table = table[:table.index('};') + 2]
        with tempfile.TemporaryDirectory(prefix='stm32-oled-wifi-') as directory:
            root = Path(directory)
            (root / 'stm32f1xx_hal.h').write_text(HAL_STUB)
            (root / 'main.h').write_text('')
            # Include the actual renderer in the same TU to inspect its target buffer.
            source = PREAMBLE + '\n' + defines + '\n' + \
                '#include "' + (ROOT / 'Core/Src/OLED.c').as_posix() + '"\n' + table + '\n' + \
                '\n'.join(function(main, name) for name in names) + '\n' + CHECKS
            (root / 'test.c').write_text(source, encoding='utf-8')
            command = ['gcc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                       '-Wno-missing-braces',  # Existing ASCII font uses flat initializers.
                       '-I', str(root), '-I', str(ROOT / 'Core/Inc'),
                       str(root / 'test.c'), '-o', str(root / 'test.exe')]
            build = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace')
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(root / 'test.exe')], capture_output=True, text=True, encoding='utf-8', errors='replace')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
