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

#endif
