#pragma once
// The UI settings: orientation, dark mode, writing mode, font size, word count, new-note type, spellcheck,
// autocorrect and keyboard layout. Kept in NVS ("ui_prefs") and copied to /microslate/ui_prefs.json on the SD
// card, so they come back after a flash that erased NVS.

namespace uiprefs {

void load();              // NVS into the settings; early in setup, so the first screen has the saved orientation
bool restoreFromSd();     // only when NVS has none: the SD copy into NVS and the settings. True if restored.
const char* layoutFile(); // the saved keyboard layout file ("" = built-in US)
int legacyLayout();       // the old built-in layout index (1 RU, 2 DE); 0 once a file name has been saved
void saveIfChanged();     // from the loop: writes NVS and the SD copy after a setting changed

}  // namespace uiprefs
