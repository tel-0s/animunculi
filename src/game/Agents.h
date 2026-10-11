#pragma once

// Which actor each agent id inhabits. Persisted in the SKSE co-save.
namespace Game::Agents
{
	inline constexpr std::string_view kDefault = "default";

	std::string AgentOf(const json& a_params);  // params["agent"], defaulting to "default"

	// Main thread. nullptr when unbound or the form no longer resolves.
	RE::Actor* Get(std::string_view a_agent);
	// Main thread. Throws not_bound / not_loaded (when a_needLoaded and the body has no 3D).
	RE::Actor* Require(const json& a_params, bool a_needLoaded = true);

	void Bind(const std::string& a_agent, RE::Actor* a_actor);
	bool Unbind(const std::string& a_agent);

	// Agent id for an actor, or empty. Thread-safe.
	std::string AgentFor(RE::FormID a_actor);
	bool        IsAgent(const RE::TESObjectREFR* a_ref);

	std::vector<std::pair<std::string, RE::FormID>> Snapshot();
	json                                             Describe();  // main thread

	void InstallSerialization();
}
