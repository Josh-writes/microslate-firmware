#pragma once

#include "config.h"

void inputSetup();
void enqueueKeyEvent(uint8_t keyCode, uint8_t modifiers, bool pressed);
void processAllInput();
char hidToAscii(uint8_t hid, uint8_t modifiers);
