#pragma once

// What an embodied agent can learn by looking closer: books and notes, the map (where am I, what
// places are around, where the quest markers point), named places, and the dialogue topics the
// player currently has open. All main thread.
namespace Game::Knowledge
{
	json Read(const json& p);             // text of a book/note (inventory, within reach, or the one the player has open)
	json Map(const json& p);              // here + nearby discovered places + quest targets (+ exits when indoors)
	json Locate(const json& p);           // find a place by name, anywhere (incl. other worldspaces)
	json DialogueOptions(const json& p);  // topics in the player's open dialogue menu

	// Outdoors: the nearest map marker ("near Bleakcoast Cave (400 m NE)"); null indoors.
	json NearestPlace(RE::TESObjectREFR* a_ref);
}
