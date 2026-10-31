#pragma once
#include "app.h"
#include "binding_editor.h"
#include "rows.h"
#include "settings_registry.h"
#include <vector>
namespace uc::deck {
// The registry pages each tab of the Deck launcher shows (only pages the registry lists for the Deck launcher, in page order):
// Game = game; Graphics = graphics + effects; Controls = the gamepad page (gyro, sticks, touch aim) and the gamepad's buttons. The reticle is the in-game menu's only.
std::vector<settings::Page> tab_pages(Tab);
// Draws the pages of a tab as rows, with the captions of its sections (Gyro, Sticks, Touch aim, Effects, Bindings). The gamepad bindings are one row per action plus
// "restore defaults"; the options' bindings follow `pad_editor`. Returns true when `options` changed.
bool draw_tab(Options& options, Tab tab, RowCtx& rows, BindingEditor& pad_editor, PadFamily family);
}
