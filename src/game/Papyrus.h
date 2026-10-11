#pragma once

// Papyrus-facing API (script "Animunculi", data/Scripts/Source/Animunculi.psc) and helpers for
// calling into Papyrus from native code.
namespace Game::Papyrus
{
	bool RegisterNatives(RE::BSScript::IVirtualMachine* a_vm);

	// SKSE mod event (RegisterForModEvent). Main thread.
	void SendModEvent(const char* a_name, const std::string& a_str, float a_num, RE::TESForm* a_sender);

	// Calls a Papyrus member function on a form (e.g. Actor.PathToReference). a_onDone runs with the
	// return value when the call finishes, possibly frames later (latent functions). Main thread.
	using Callback = std::function<void(const RE::BSScript::Variable&)>;
	bool CallMethod(RE::TESForm* a_self, const char* a_script, const char* a_function,
		RE::BSScript::IFunctionArguments* a_args, Callback a_onDone = {});

	// Mod-registered actions ("perform").
	struct ModAction
	{
		std::string name, description, params;
	};
	std::vector<ModAction> Actions();
	// Sends "Animunculi_Action_<name>" and returns an action id; the mod answers with
	// Animunculi.CompleteAction(id, ...), which emits action_finished. Main thread.
	std::uint32_t Perform(const std::string& a_agent, RE::Actor* a_actor, const std::string& a_name, const json& a_args);
}
