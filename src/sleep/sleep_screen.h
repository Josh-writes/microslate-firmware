#pragma once
// The sleep screen. One choice, made on one screen (Settings > Display > Sleep screen): Stock (the brand logo, text
// until Josh picks one), Random (a different one of my pictures each sleep) or one picture. Pictures are sent from a
// phone (Transfer's picture mode) and live in /sleep/ on the card as finished 1-bit BMPs the size of a screen.
// Spec: MicroSlate-workspace/notes/v3.0-icon-and-sleep-screen.md, revision 2026-10-29.

#include <cstdint>
#include <string>

#include "sleep_rules.h"

class GfxRenderer;

namespace sleepscr {

void init();                       // after the card is mounted: the saved choice

void refresh();                    // read /sleep/ again (a choice that no longer fits falls back, see settleChoice)
int count();
const char* name(int i);           // "sleep-003.bmp"
std::string label(int i);          // "Picture 3"
bool remove(int i);

Choice choice();
bool isChosen(int i);              // picture i is the sleep screen
std::string choiceLabel();         // "Stock", "Random", "Picture 3"
void chooseStock();
void chooseRandom();               // needs at least two pictures
void choosePicture(int i);

// Draws picture i over the whole screen (fit, centred, white around it). No refresh. False = could not read it.
bool drawPicture(GfxRenderer& r, int i);
// Draws what this sleep shows (the chosen picture, a random one, or the stock screen). No refresh.
void drawForSleep(GfxRenderer& r);
void drawStock(GfxRenderer& r);

// Picture upload (Transfer): the received file at kUploadTmp is checked and moved into /sleep/ under the next free
// name. The first picture ever added becomes the sleep screen. False with a message for the phone when refused.
constexpr const char* kUploadTmp = "/sleep/.upload.tmp";
bool acceptUpload(std::string& savedName, const char*& error);

}  // namespace sleepscr
