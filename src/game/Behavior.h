#pragma once

// Standing orders for an agent's body, applied by the plugin in real time and saved in the co-save:
//   combat: style preset (vanilla combat styles: keep distance, mage, melee, ...), restraint (never
//           fight guards/civilians/allies), medic (heal the player when they are low)
//   follow: "close" (when the player stops, stand by them: no wandering, no benches; the normal
//           follower AI walks the body while the player travels) or "relaxed"
//           (the follower AI's normal behaviour, including sandboxing); remount (get back on your
//           horse when the player rides and you are not, e.g. after a fight)
// It also emits `mount` events (the player or a body getting on/off) and `health_low` events for the player and agents (once per dip below 35%).
namespace Game::Behavior
{
	json SetCombat(const json& p);       // main thread
	json SetFollowStyle(const json& p);  // main thread
	json Describe(const std::string& a_agent);

	bool Restrains(const std::string& a_agent, RE::Actor* a_target);  // true: restraint forbids attacking a_target
	// The body's horse: the last one it rode, else a nearby mount it owns. Main thread.
	RE::Actor* FindHorse(const std::string& a_agent, RE::Actor* a_actor);
	void       OnMountOrder(const std::string& a_agent, RE::Actor* a_horse);  // mount / activate on a horse
	void       OnDismountOrder(const std::string& a_agent);                   // pauses remount until the player's next ride
	// An action that moves the body on purpose (move_to, sit, walking over to activate or mount).
	// Close-follow's offset is released at once and stays off until EndAction (or a_max passes).
	void BeginAction(const std::string& a_agent, RE::Actor* a_actor, std::uint32_t a_id, std::chrono::seconds a_max = std::chrono::seconds{ 120 });
	void EndAction(const std::string& a_agent, std::uint32_t a_id);  // any thread
	void OnWaitChanged(const std::string& a_agent, bool a_waiting);   // wait_here / follow

	void Tick(float a_delta);  // frame callback
	void OnGameLoaded();       // re-apply presets after load (base forms revert)

	// co-save plumbing (called from Agents' serialization callbacks)
	void Save(SKSE::SerializationInterface* a_intfc);
	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version);
	void Revert();
	inline constexpr std::uint32_t kRecord = 'SETS';
	inline constexpr std::uint32_t kVersion = 1;
}
