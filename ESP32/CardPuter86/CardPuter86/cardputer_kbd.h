#ifndef _CARDputer_KBD_H
#define _CARDputer_KBD_H

#include "gbConfig.h"
#include <Arduino.h>

// OSD-compatible key constants (maps to original keys.h values)
// These are used by osd.cpp ShowTinyMenu
// For Cardputer, we use the M5Cardputer.Keyboard built-in reader

void cardputer_kbd_init(void);
void cardputer_kbd_scan(void);
boolean checkAndCleanKey(uint8_t scancode);
boolean checkKey(uint8_t scancode);

// Raw key input for emulator keyboard handler
// Fills keymap[256] with pressed state (0=pressed, 1=not pressed)
void cardputer_kbd_fill_keymap(void);
void cardputer_kbd_serial_inject(void);
void cardputer_kbd_serial_apply(void);
// Debug console: byte 0x01 on the serial line arms command mode, the next byte
// is the command; returns 0 when nothing is pending. Debug builds only.
int cardputer_kbd_serial_take_command(void);
// Raw-line input (debug builds only): after cardputer_kbd_serial_begin_line()
// the bytes that follow are NOT turned into PS/2 keys; they are collected until
// CR and handed back by cardputer_kbd_serial_take_line() (NULL when no complete
// line is pending). Used to give the firmware verbatim strings such as
// "ssid,password" without depending on the scan-code injection path.
void cardputer_kbd_serial_begin_line(void);
const char *cardputer_kbd_serial_take_line(void);

#endif
