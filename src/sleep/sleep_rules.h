#pragma once
// Sleep screen rules with no device code, so they run in host tests (test/sleep).
// Spec: MicroSlate-workspace/notes/v3.0-icon-and-sleep-screen.md, revision 2026-10-29.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sleepscr {

// Largest picture file accepted (a 1-bit 800 x 480 BMP is 48,062 bytes).
constexpr size_t kMaxPictureBytes = 64 * 1024;

// What a phone sent: a 1-bit, uncompressed BMP whose size fits one of our screens (800 x 480 or 480 x 800) and
// whose file length matches its header exactly. `head` is the start of the file (at least 62 bytes).
// On success sets w and h.
bool pictureLooksValid(const uint8_t* head, size_t headLen, size_t fileLen, int& w, int& h);

// The name for the next picture: one more than the highest "sleep-NNN.bmp" already there ("sleep-001.bmp" first).
std::string nextPictureName(const std::vector<std::string>& existing);

// "Picture 3" for "sleep-003.bmp"; any other name without its extension.
std::string pictureLabel(const std::string& name);

// What sleep shows: the stock screen, a random picture, or one chosen picture.
enum class Choice : uint8_t { Stock = 0, Random = 1, Picture = 2 };

// After pictures were added or removed, a choice that no longer fits falls back: the chosen picture gone -> Random
// with two or more left, the one picture left with one, Stock with none; Random needs two or more (one left -> that
// picture, none -> Stock). Picture with chosenThere == false means "the first picture" to the caller.
Choice settleChoice(Choice c, bool chosenThere, int count);

// The picture for this sleep: random among n, never the one shown last time (when there is a choice).
int pickRandom(int n, int last, uint32_t random);

}  // namespace sleepscr
