#pragma once

// Resolves the target strings of the protocol ("self", "player", "crosshair", "0x0001A696", names).
// Main thread only.
namespace Game::Targets
{
	inline constexpr float kUnitsPerMeter = 70.f;

	// All loaded, enabled references within a_radius (game units) of a_origin, closest first.
	std::vector<RE::TESObjectREFR*> InRange(RE::TESObjectREFR* a_origin, float a_radius);

	// Throws not_found. a_self may be null (then "self" fails and names are searched around the player).
	RE::TESObjectREFR* Ref(std::string_view a_spec, RE::Actor* a_self, float a_searchRadius = 150.f * kUnitsPerMeter);
	RE::Actor*         Actor(std::string_view a_spec, RE::Actor* a_self, float a_searchRadius = 150.f * kUnitsPerMeter);

	struct InventoryItem
	{
		RE::TESBoundObject* object{ nullptr };
		std::int32_t        count{ 0 };
		bool                worn{ false };
	};
	// Finds an item in a_owner's inventory by base-form id or name (exact, then prefix, then substring).
	InventoryItem Item(RE::TESObjectREFR* a_owner, std::string_view a_spec);

	// A spell a_actor knows (race/base/added spells), by id or name.
	RE::SpellItem* Spell(RE::Actor* a_actor, std::string_view a_spec);
	std::vector<RE::SpellItem*> KnownSpells(RE::Actor* a_actor);

	// Ownership as it concerns a_actor (an agent's body). The engine's IsCrimeToActivate() answers
	// for the player only, so a follower's own horse or bedroll counts as someone else's there.
	//   kFree:  unowned, or the player's (or nothing the player would be punished for)
	//   kMine:  owned by a_actor's NPC record or by a faction a_actor belongs to
	//   kOther: someone else's; using or taking it would be a crime
	enum class Ownership
	{
		kFree,
		kMine,
		kOther
	};
	Ownership   OwnershipFor(RE::TESObjectREFR* a_ref, RE::Actor* a_actor);
	std::string OwnerName(RE::TESObjectREFR* a_ref);  // "" when unowned

	// a_actor's mount if it is riding one, else null.
	RE::Actor* MountOf(RE::Actor* a_actor);
	// Something a_actor could ride: a living mount nobody else is riding.
	bool Rideable(RE::Actor* a_mount, RE::Actor* a_rider);
}
