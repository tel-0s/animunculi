#include "PCH.h"
#include "Papyrus.h"
#include "Agents.h"
#include "ChatUI.h"
#include "Events.h"
#include "Voice.h"

#include "core/Text.h"
#include "net/Rpc.h"
#include "net/Server.h"

namespace Game::Papyrus
{
	namespace
	{
		constexpr auto kScript = "Animunculi";

		class CallbackFunctor : public RE::BSScript::IStackCallbackFunctor
		{
		public:
			explicit CallbackFunctor(Callback a_cb) :
				_cb(std::move(a_cb)) {}
			void operator()(RE::BSScript::Variable a_result) override
			{
				if (_cb) {
					_cb(a_result);
				}
			}
			void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

		private:
			Callback _cb;
		};

		std::mutex                                g_actionsLock;
		std::map<std::string, ModAction>          g_actions;
		struct PendingAction
		{
			std::string agent, name;
		};
		std::map<std::uint32_t, PendingAction> g_pending;
		std::atomic<std::uint32_t>             g_nextAction{ 1 };

		std::string Agent(const RE::BSFixedString& s)
		{
			return s.empty() ? std::string(Agents::kDefault) : std::string(s.c_str());
		}

		json ParseLoose(const std::string& a_text)
		{
			if (a_text.empty()) {
				return nullptr;
			}
			auto j = json::parse(a_text, nullptr, false);
			return j.is_discarded() ? json(a_text) : j;
		}

		// ---------------------------------------------------------------- natives
		bool Bind(RE::StaticFunctionTag*, RE::Actor* a_actor, RE::BSFixedString a_agent)
		{
			if (!a_actor) {
				return false;
			}
			Agents::Bind(Agent(a_agent), a_actor);
			return true;
		}

		void Unbind(RE::StaticFunctionTag*, RE::BSFixedString a_agent)
		{
			Agents::Unbind(Agent(a_agent));
		}

		RE::Actor* GetBoundActor(RE::StaticFunctionTag*, RE::BSFixedString a_agent)
		{
			return Agents::Get(Agent(a_agent));
		}

		RE::BSFixedString GetAgentId(RE::StaticFunctionTag*, RE::Actor* a_actor)
		{
			return a_actor ? RE::BSFixedString(Agents::AgentFor(a_actor->GetFormID())) : RE::BSFixedString("");
		}

		bool IsConnected(RE::StaticFunctionTag*)
		{
			return Net::ClientCount() > 0;
		}

		bool IsSpeaking(RE::StaticFunctionTag*, RE::BSFixedString a_agent)
		{
			return Voice::IsSpeaking(Agent(a_agent));
		}

		void PlayerSay(RE::StaticFunctionTag*, RE::BSFixedString a_text, RE::BSFixedString a_agent)
		{
			Events::PlayerChat(a_text.c_str(), "papyrus", a_agent.empty() ? "" : a_agent.c_str());
		}

		void EmitEvent(RE::StaticFunctionTag*, RE::BSFixedString a_kind, RE::BSFixedString a_data)
		{
			Events::Log().Push("custom", { { "kind", a_kind.c_str() }, { "data", ParseLoose(a_data.c_str()) } });
		}

		void RegisterAction(RE::StaticFunctionTag*, RE::BSFixedString a_name, RE::BSFixedString a_description, RE::BSFixedString a_params)
		{
			if (a_name.empty()) {
				return;
			}
			std::lock_guard lock{ g_actionsLock };
			g_actions[a_name.c_str()] = { a_name.c_str(), a_description.c_str(), a_params.c_str() };
			logger::info("mod action registered: {}", a_name.c_str());
		}

		void UnregisterAction(RE::StaticFunctionTag*, RE::BSFixedString a_name)
		{
			std::lock_guard lock{ g_actionsLock };
			g_actions.erase(a_name.c_str());
		}

		void CompleteAction(RE::StaticFunctionTag*, std::int32_t a_id, bool a_success, RE::BSFixedString a_result)
		{
			PendingAction p;
			{
				std::lock_guard lock{ g_actionsLock };
				auto            it = g_pending.find(static_cast<std::uint32_t>(a_id));
				if (it == g_pending.end()) {
					return;
				}
				p = it->second;
				g_pending.erase(it);
			}
			json j{ { "action_id", a_id }, { "agent", p.agent }, { "action", p.name }, { "ok", a_success } };
			if (!a_result.empty()) {
				j["result"] = ParseLoose(a_result.c_str());
			}
			Events::Log().Push("action_finished", std::move(j));
		}

		RE::BSFixedString GetVersion(RE::StaticFunctionTag*)
		{
			return ANIMUNCULI_VERSION_STRING;
		}

		void OpenChat(RE::StaticFunctionTag*)
		{
			ChatUI::Open();
		}
	}

	bool RegisterNatives(RE::BSScript::IVirtualMachine* a_vm)
	{
		a_vm->RegisterFunction("Bind", kScript, Bind);
		a_vm->RegisterFunction("Unbind", kScript, Unbind);
		a_vm->RegisterFunction("GetBoundActor", kScript, GetBoundActor);
		a_vm->RegisterFunction("GetAgentId", kScript, GetAgentId);
		a_vm->RegisterFunction("IsConnected", kScript, IsConnected);
		a_vm->RegisterFunction("IsSpeaking", kScript, IsSpeaking);
		a_vm->RegisterFunction("PlayerSay", kScript, PlayerSay);
		a_vm->RegisterFunction("EmitEvent", kScript, EmitEvent);
		a_vm->RegisterFunction("RegisterAction", kScript, RegisterAction);
		a_vm->RegisterFunction("UnregisterAction", kScript, UnregisterAction);
		a_vm->RegisterFunction("CompleteAction", kScript, CompleteAction);
		a_vm->RegisterFunction("GetVersion", kScript, GetVersion);
		a_vm->RegisterFunction("OpenChat", kScript, OpenChat);
		logger::info("Papyrus natives registered ({})", kScript);
		return true;
	}

	void SendModEvent(const char* a_name, const std::string& a_str, float a_num, RE::TESForm* a_sender)
	{
		auto* src = SKSE::GetModCallbackEventSource();
		if (!src) {
			return;
		}
		SKSE::ModCallbackEvent ev{ a_name, a_str.c_str(), a_num, a_sender };
		src->SendEvent(&ev);
	}

	bool CallMethod(RE::TESForm* a_self, const char* a_script, const char* a_function,
		RE::BSScript::IFunctionArguments* a_args, Callback a_onDone)
	{
		auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!vm || !a_self) {
			delete a_args;
			return false;
		}
		// Find or create the script object bound to this form, typed by its actual form type
		// (Actor, ObjectReference, ...), so inherited functions resolve.
		RE::BSScript::Variable self;
		RE::BSScript::PackHandle(&self, a_self, static_cast<RE::VMTypeID>(a_self->GetFormType()));
		auto obj = self.GetObject();
		if (!obj) {
			logger::warn("papyrus: no script object for {:08X} ({}.{})", a_self->GetFormID(), a_script, a_function);
			delete a_args;
			return false;
		}
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb{ new CallbackFunctor(std::move(a_onDone)) };
		return vm->DispatchMethodCall(obj, a_function, a_args, cb);
	}

	std::vector<ModAction> Actions()
	{
		std::lock_guard        lock{ g_actionsLock };
		std::vector<ModAction> out;
		for (auto& [name, a] : g_actions) {
			out.push_back(a);
		}
		return out;
	}

	std::uint32_t Perform(const std::string& a_agent, RE::Actor* a_actor, const std::string& a_name, const json& a_args)
	{
		{
			std::lock_guard lock{ g_actionsLock };
			if (!g_actions.contains(a_name)) {
				throw Net::RpcError("not_found", "no mod action '" + a_name + "' (see actions)");
			}
		}
		const auto id = g_nextAction++;
		{
			std::lock_guard lock{ g_actionsLock };
			g_pending[id] = { a_agent, a_name };
			if (g_pending.size() > 256) {
				g_pending.erase(g_pending.begin());
			}
		}
		const auto eventName = "Animunculi_Action_" + a_name;
		SendModEvent(eventName.c_str(), a_args.dump(), static_cast<float>(id), a_actor);
		return id;
	}
}
