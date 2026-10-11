#include "PCH.h"
#include "Agents.h"
#include "Behavior.h"
#include "Events.h"

#include "core/Text.h"
#include "net/Rpc.h"

namespace Game::Agents
{
	namespace
	{
		constexpr std::uint32_t kUID = 'ANMC';
		constexpr std::uint32_t kRecBindings = 'BIND';
		constexpr std::uint32_t kVersion = 1;

		std::mutex                          g_lock;
		std::map<std::string, RE::FormID>   g_bindings;  // agent id -> actor form id

		void OnSave(SKSE::SerializationInterface* a_intfc)
		{
			const auto snap = Snapshot();
			if (!a_intfc->OpenRecord(kRecBindings, kVersion)) {
				logger::error("co-save: could not open bindings record");
				return;
			}
			const auto count = static_cast<std::uint32_t>(snap.size());
			a_intfc->WriteRecordData(count);
			for (const auto& [agent, id] : snap) {
				const auto len = static_cast<std::uint32_t>(agent.size());
				a_intfc->WriteRecordData(len);
				a_intfc->WriteRecordData(agent.data(), len);
				a_intfc->WriteRecordData(id);
			}
			Behavior::Save(a_intfc);
		}

		void OnLoad(SKSE::SerializationInterface* a_intfc)
		{
			std::map<std::string, RE::FormID> loaded;
			std::uint32_t                     type, version, length;
			while (a_intfc->GetNextRecordInfo(type, version, length)) {
				if (type == Behavior::kRecord) {
					Behavior::Load(a_intfc, version);
					continue;
				}
				if (type != kRecBindings || version != kVersion) {
					continue;
				}
				std::uint32_t count = 0;
				a_intfc->ReadRecordData(count);
				for (std::uint32_t i = 0; i < count && i < 256; ++i) {
					std::uint32_t len = 0;
					a_intfc->ReadRecordData(len);
					if (len > 256) {
						return;  // corrupt
					}
					std::string agent(len, '\0');
					a_intfc->ReadRecordData(agent.data(), len);
					RE::FormID oldId = 0, newId = 0;
					a_intfc->ReadRecordData(oldId);
					if (a_intfc->ResolveFormID(oldId, newId)) {
						loaded[agent] = newId;
					} else {
						logger::warn("co-save: agent '{}' body {:08X} no longer exists", agent, oldId);
					}
				}
			}
			{
				std::lock_guard lock{ g_lock };
				g_bindings = std::move(loaded);
			}
			for (const auto& [agent, id] : Snapshot()) {
				logger::info("co-save: agent '{}' -> {:08X}", agent, id);
			}
		}

		void OnRevert(SKSE::SerializationInterface*)
		{
			{
				std::lock_guard lock{ g_lock };
				g_bindings.clear();
			}
			Behavior::Revert();
		}
	}

	std::string AgentOf(const json& a_params)
	{
		auto a = Net::OptString(a_params, "agent", std::string(kDefault));
		return a.empty() ? std::string(kDefault) : a;
	}

	RE::Actor* Get(std::string_view a_agent)
	{
		RE::FormID id = 0;
		{
			std::lock_guard lock{ g_lock };
			const auto      it = g_bindings.find(std::string(a_agent));
			if (it == g_bindings.end()) {
				return nullptr;
			}
			id = it->second;
		}
		return RE::TESForm::LookupByID<RE::Actor>(id);
	}

	RE::Actor* Require(const json& a_params, bool a_needLoaded)
	{
		const auto agent = AgentOf(a_params);
		auto*      actor = Get(agent);
		if (!actor) {
			throw Net::RpcError("not_bound", "agent '" + agent + "' has no body; call bind (or Animunculi.Bind from Papyrus)");
		}
		if (a_needLoaded && (!actor->Is3DLoaded() || !actor->GetParentCell())) {
			throw Net::RpcError("not_loaded", std::string(actor->GetDisplayFullName()) + " is not in a loaded area");
		}
		return actor;
	}

	void Bind(const std::string& a_agent, RE::Actor* a_actor)
	{
		if (!a_actor) {
			return;
		}
		{
			std::lock_guard lock{ g_lock };
			// One body per agent, one agent per body.
			std::erase_if(g_bindings, [&](const auto& kv) { return kv.second == a_actor->GetFormID() && kv.first != a_agent; });
			g_bindings[a_agent] = a_actor->GetFormID();
		}
		logger::info("agent '{}' bound to {} ({:08X})", a_agent, a_actor->GetDisplayFullName(), a_actor->GetFormID());
		Events::Log().Push("agent_bound", { { "agent", a_agent }, { "id", Core::FormatFormID(a_actor->GetFormID()) }, { "name", a_actor->GetDisplayFullName() } });
	}

	bool Unbind(const std::string& a_agent)
	{
		std::lock_guard lock{ g_lock };
		if (g_bindings.erase(a_agent) == 0) {
			return false;
		}
		Events::Log().Push("agent_unbound", { { "agent", a_agent } });
		return true;
	}

	std::string AgentFor(RE::FormID a_actor)
	{
		std::lock_guard lock{ g_lock };
		for (const auto& [agent, id] : g_bindings) {
			if (id == a_actor) {
				return agent;
			}
		}
		return {};
	}

	bool IsAgent(const RE::TESObjectREFR* a_ref)
	{
		return a_ref && !AgentFor(a_ref->GetFormID()).empty();
	}

	std::vector<std::pair<std::string, RE::FormID>> Snapshot()
	{
		std::lock_guard lock{ g_lock };
		return { g_bindings.begin(), g_bindings.end() };
	}

	json Describe()
	{
		json out = json::array();
		for (const auto& [agent, id] : Snapshot()) {
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
			out.push_back({ { "agent", agent },
				{ "bound", actor != nullptr },
				{ "id", Core::FormatFormID(id) },
				{ "name", actor ? actor->GetDisplayFullName() : "" },
				{ "loaded", actor && actor->Is3DLoaded() } });
		}
		return out;
	}

	void InstallSerialization()
	{
		auto* s = SKSE::GetSerializationInterface();
		s->SetUniqueID(kUID);
		s->SetSaveCallback(OnSave);
		s->SetLoadCallback(OnLoad);
		s->SetRevertCallback(OnRevert);
	}
}
