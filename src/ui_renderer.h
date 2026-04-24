#pragma once

class GfxRenderer;
class HalGPIO;

void rendererSetup(GfxRenderer& renderer);
void drawMainMenu(GfxRenderer& renderer, HalGPIO& gpio);
void drawFileBrowser(GfxRenderer& renderer, HalGPIO& gpio);
void drawTextEditor(GfxRenderer& renderer, HalGPIO& gpio);
void drawRenameScreen(GfxRenderer& renderer, HalGPIO& gpio);
void drawSettingsMenu(GfxRenderer& renderer, HalGPIO& gpio);
void drawBluetoothSettings(GfxRenderer& renderer, HalGPIO& gpio);
void drawPairedKeyboardsMenu(GfxRenderer& renderer, HalGPIO& gpio);
void drawSyncScreen(GfxRenderer& renderer, HalGPIO& gpio);
