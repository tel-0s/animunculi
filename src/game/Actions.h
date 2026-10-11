#pragma once

// Things an embodied agent can do. All run on the main thread and return quickly; long-running
// actions return an action_id and later emit an `action_finished` event.
namespace Game::Actions
{
	json MoveTo(const json& p);
	json Follow(const json& p);
	json WaitHere(const json& p);
	json Activate(const json& p);  // walks over first when out of reach (approach=false to refuse instead)
	json Mount(const json& p);     // get on your horse (or a target mount); walks over if needed
	json Dismount(const json& p);
	json PickUp(const json& p);
	json Give(const json& p);
	json UseItem(const json& p);
	json Unequip(const json& p);
	json Cast(const json& p);
	json Attack(const json& p);
	json StopCombat(const json& p);
	json LookAt(const json& p);  // timed (duration_s, 15 s; 0 = until changed); paused while walking away or fighting
	json Emote(const json& p);
	json DrawWeapon(const json& p);
	json OpenInventory(const json& p);
	json Notify(const json& p);
	json Console(const json& p);

	// Frame callback: keeps look_at gazes timed and out of the way of walking and fighting.
	void Tick(float a_delta);
	void OnGameLoaded();  // forget gazes (their handles belong to the old session)

	// Walk to a_target (going after it again if it wanders, as horses do), then activate it.
	// Emits action_finished {action: a_action, ok, target, mounted?}. Main thread. Returns the id.
	std::uint32_t ApproachAndActivate(RE::Actor* a_actor, const std::string& a_agent, RE::TESObjectREFR* a_target, const char* a_action);
}
