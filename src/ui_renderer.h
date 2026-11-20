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
void drawLayoutEditor(GfxRenderer& renderer, HalGPIO& gpio);
void drawSyncScreen(GfxRenderer& renderer, HalGPIO& gpio);
void drawSleepPictures(GfxRenderer& renderer, HalGPIO& gpio);
void drawMyWords(GfxRenderer& renderer, HalGPIO& gpio);
void drawTransferScreen(GfxRenderer& renderer, HalGPIO& gpio);
void drawFwUpdateScreen(GfxRenderer& renderer, HalGPIO& gpio);
void drawReaderScreen(GfxRenderer& renderer, HalGPIO& gpio);

// The main menu, top to bottom: Notes, Transfer, Sync, the installed reader (dual boot), Settings. The screen and the
// input handler both read the order from here. Returns the number of items.
enum class MainItem : unsigned char { Notes, Reader, Transfer, Sync, Settings };
int mainMenuItems(MainItem (&out)[5]);

// Editor text with tabs: a tab is a fixed indent (kEditorTabSpaces space widths), not a tab stop, so a line's wrap
// still depends only on where it starts. Wrapping (main.cpp), drawing, the cursor and taps all use this width.
constexpr int kEditorTabSpaces = 4;
int editorTabWidth(GfxRenderer& renderer, int fontId);
int editorTextAdvance(GfxRenderer& renderer, int fontId, const char* text, int len);

// Centered notice while the notes list waits for the OneDrive check.
void drawCloudNotice(GfxRenderer& renderer, HalGPIO& gpio, const char* text, const char* sub = nullptr);
