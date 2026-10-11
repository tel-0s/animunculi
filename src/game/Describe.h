#pragma once

// Turns game objects into compact, LLM-friendly JSON. Main thread only.
// Distances are meters; bearings are degrees clockwise from the viewer's facing.
namespace Game::Describe
{
	// "actor", "item", "container", "door", "furniture", "activator", "flora", or "" (not notable).
	std::string Kind(RE::TESObjectREFR* a_ref);
	std::string ItemKind(const RE::TESForm* a_form);
	std::string Name(RE::TESObjectREFR* a_ref);

	// Relative placement of a_ref as seen from a_viewer: dist_m, bearing, bearing_word, dz_m.
	void Placement(json& a_out, RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_viewer);

	json Brief(RE::TESObjectREFR* a_ref, RE::Actor* a_viewer);                    // list entry
	json ActorState(RE::Actor* a_actor, RE::Actor* a_viewer, bool a_detailed);   // self / player / inspect
	json Detailed(RE::TESObjectREFR* a_ref, RE::Actor* a_viewer);                 // inspect
	json Location(RE::TESObjectREFR* a_who);
	json Time();
	json Inventory(RE::TESObjectREFR* a_owner, const std::string& a_filter, std::size_t a_limit);
	json Quests();

	struct NearbyOptions
	{
		float                    radiusM{ 40.f };
		std::vector<std::string> kinds;  // empty = all notable kinds
		std::size_t              limit{ 25 };
		bool                     losOnly{ false };
	};
	json Nearby(RE::Actor* a_viewer, const NearbyOptions& a_opts);
}
