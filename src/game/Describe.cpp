#include "PCH.h"
#include "Describe.h"
#include "Agents.h"
#include "Targets.h"

#include "core/Text.h"

namespace Game::Describe
{
	namespace
	{
		constexpr float kU = Targets::kUnitsPerMeter;

		double R1(double v) { return std::round(v * 10.0) / 10.0; }
		double R2(double v) { return std::round(v * 100.0) / 100.0; }

		void SetIf(json& j, const char* k, bool v)
		{
			if (v) {
				j[k] = true;
			}
		}

		const char* Str(const char* s) { return s ? s : ""; }

		double Fraction(RE::Actor* a, RE::ActorValue av)
		{
			const float max = a->GetActorValueMax(av);
			if (max <= 0.f) {
				return 0.0;
			}
			return R2(std::clamp(a->AsActorValueOwner()->GetActorValue(av) / max, 0.f, 1.f));
		}

		std::string FormName(const RE::TESForm* f) { return f ? Str(f->GetName()) : ""; }

		const char* QuestType(RE::QUEST_DATA::Type t)
		{
			using T = RE::QUEST_DATA::Type;
			switch (t) {
			case T::kMainQuest: return "main";
			case T::kMagesGuild: return "college";
			case T::kThievesGuild: return "thieves_guild";
			case T::kDarkBrotherhood: return "dark_brotherhood";
			case T::kCompanionsQuest: return "companions";
			case T::kMiscellaneous: return "misc";
			case T::kDaedric: return "daedric";
			case T::kSideQuest: return "side";
			case T::kCivilWar: return "civil_war";
			case T::kDLC01_Vampire: return "dawnguard";
			case T::kDLC02_Dragonborn: return "dragonborn";
			default: return "other";
			}
		}

		std::string LocationName(RE::TESObjectCELL* a_cell)
		{
			if (!a_cell) {
				return {};
			}
			if (auto* loc = a_cell->GetLocation(); loc && loc->GetFullName() && *loc->GetFullName()) {
				return loc->GetFullName();
			}
			return Str(a_cell->GetFullName());
		}
	}

	std::string Name(RE::TESObjectREFR* a_ref)
	{
		return a_ref ? Str(a_ref->GetDisplayFullName()) : "";
	}

	std::string ItemKind(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return "";
		}
		switch (a_form->GetFormType()) {
		case RE::FormType::Weapon: return "weapon";
		case RE::FormType::Armor: return "armor";
		case RE::FormType::Ammo: return "ammo";
		case RE::FormType::AlchemyItem:
			{
				const auto* a = a_form->As<RE::AlchemyItem>();
				return a && a->IsPoison() ? "poison" : a && a->IsFood() ? "food" : "potion";
			}
		case RE::FormType::Book:
			{
				const auto* b = a_form->As<RE::TESObjectBOOK>();
				return b && b->TeachesSpell() ? "spell_tome" : "book";
			}
		case RE::FormType::Misc: return a_form->GetFormID() == 0xF ? "gold" : "misc";
		case RE::FormType::KeyMaster: return "key";
		case RE::FormType::Ingredient: return "ingredient";
		case RE::FormType::SoulGem: return "soul_gem";
		case RE::FormType::Scroll: return "scroll";
		case RE::FormType::Light: return "torch";
		case RE::FormType::Note: return "note";
		default: return "";
		}
	}

	std::string Kind(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return "";
		}
		if (a_ref->As<RE::Actor>()) {
			return "actor";
		}
		auto* base = a_ref->GetBaseObject();
		if (!base) {
			return "";
		}
		switch (base->GetFormType()) {
		case RE::FormType::Container: return "container";
		case RE::FormType::Door: return "door";
		case RE::FormType::Furniture: return "furniture";
		case RE::FormType::Activator:
		case RE::FormType::TalkingActivator: return Str(base->GetName())[0] ? "activator" : "";
		case RE::FormType::Flora:
		case RE::FormType::Tree: return Str(base->GetName())[0] ? "flora" : "";
		case RE::FormType::Light:
			{
				const auto* l = base->As<RE::TESObjectLIGH>();
				return l && l->CanBeCarried() ? "item" : "";
			}
		default:
			return ItemKind(base).empty() ? "" : "item";
		}
	}

	void Placement(json& a_out, RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_viewer)
	{
		if (!a_ref || !a_viewer || a_ref == a_viewer) {
			return;
		}
		const auto p = a_ref->GetPosition();
		const auto v = a_viewer->GetPosition();
		const float dx = p.x - v.x, dy = p.y - v.y, dz = p.z - v.z;
		a_out["dist_m"] = R1(std::sqrt(dx * dx + dy * dy + dz * dz) / kU);
		// Skyrim: +Y is north, heading (angle z) is clockwise from north, in radians.
		const float dir = std::atan2(dx, dy) * 180.f / std::numbers::pi_v<float>;
		const float heading = a_viewer->GetAngleZ() * 180.f / std::numbers::pi_v<float>;
		const float rel = Core::WrapDegrees(dir - heading);
		a_out["bearing"] = static_cast<int>(std::lround(rel));
		a_out["bearing_word"] = Core::BearingWord(rel);
		if (std::abs(dz) > 0.5f * kU) {
			a_out["dz_m"] = R1(dz / kU);
		}
	}

	json ActorState(RE::Actor* a, RE::Actor* a_viewer, bool a_detailed)
	{
		json j;
		j["id"] = Core::FormatFormID(a->GetFormID());
		j["name"] = Name(a);
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (a == player) {
			j["is_player"] = true;
		}
		if (auto* race = a->GetRace()) {
			j["race"] = Str(race->GetFullName());
		}
		if (auto* base = a->GetActorBase()) {
			j["sex"] = base->GetSex() == RE::SEX::kFemale ? "female" : "male";
		}
		j["level"] = a->GetLevel();
		const bool dead = a->IsDead();
		SetIf(j, "dead", dead);
		if (!dead) {
			j["health"] = Fraction(a, RE::ActorValue::kHealth);
			if (a_detailed) {
				j["magicka"] = Fraction(a, RE::ActorValue::kMagicka);
				j["stamina"] = Fraction(a, RE::ActorValue::kStamina);
			}
			const bool combat = a->IsInCombat();
			SetIf(j, "in_combat", combat);
			if (combat) {
				if (auto t = a->GetActorRuntimeData().currentCombatTarget.get()) {
					j["combat_target"] = { { "id", Core::FormatFormID(t->GetFormID()) }, { "name", Name(t.get()) } };
				}
			}
			if (a != player) {
				SetIf(j, "hostile_to_player", a->IsHostileToActor(player));
				SetIf(j, "follower", a->IsPlayerTeammate());
			}
			if (a_viewer && a_viewer != a && a_viewer != player) {
				SetIf(j, "hostile_to_you", a->IsHostileToActor(a_viewer));
			}
			SetIf(j, "sneaking", a->IsSneaking());
			if (auto* st = a->AsActorState()) {
				// Not Actor::IsInBleedout(): CommonLib 6.7.1 has no AE address for it (RELOCATION_ID(48461, 0)).
				SetIf(j, "bleedout", st->IsBleedingOut());
				SetIf(j, "weapon_drawn", st->IsWeaponDrawn());
				const auto sit = st->GetSitSleepState();
				const bool riding = a->IsOnMount();  // kRidingMount shares kIsSitting's value
				SetIf(j, "sitting", sit == RE::SIT_SLEEP_STATE::kIsSitting && !riding);
				SetIf(j, "sleeping", sit == RE::SIT_SLEEP_STATE::kIsSleeping);
				SetIf(j, "swimming", st->IsSwimming());
			}
			if (auto* mount = Targets::MountOf(a)) {
				j["riding"] = { { "id", Core::FormatFormID(mount->GetFormID()) }, { "name", Name(mount) } };
			}
			if (a->IsAMount()) {
				j["rideable"] = true;
				if (a->IsBeingRidden()) {
					RE::NiPointer<RE::Actor> rider;
					if (a->GetMountedBy(rider) && rider) {
						j["ridden_by"] = { { "id", Core::FormatFormID(rider->GetFormID()) }, { "name", Name(rider.get()) } };
					}
				}
				if (const auto owner = Targets::OwnerName(a); !owner.empty()) {
					j["owner"] = owner;
				}
				if (a_viewer && a_viewer != player) {
					const auto own = Targets::OwnershipFor(a, a_viewer);
					SetIf(j, "yours", own == Targets::Ownership::kMine);
					SetIf(j, "riding_it_is_a_crime", own == Targets::Ownership::kOther);
				}
			}
			SetIf(j, "essential", a->IsEssential());
			SetIf(j, "guard", a->IsGuard());
			if (Agents::IsAgent(a)) {
				j["agent"] = Agents::AgentFor(a->GetFormID());
				SetIf(j, "waiting", a->AsActorValueOwner()->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.f);
			}
		}
		if (a_detailed) {
			json eq = json::object();
			if (auto* r = a->GetEquippedObject(false)) {
				eq["right"] = FormName(r);
			}
			if (auto* l = a->GetEquippedObject(true)) {
				eq["left"] = FormName(l);
			}
			if (!eq.empty()) {
				j["equipped"] = eq;
			}
			const auto p = a->GetPosition();
			j["position"] = { std::round(p.x), std::round(p.y), std::round(p.z) };
			j["heading_deg"] = static_cast<int>(std::lround(a->GetAngleZ() * 180.f / std::numbers::pi_v<float>));
		}
		if (a_viewer && a_viewer != a) {
			Placement(j, a, a_viewer);
		}
		return j;
	}

	namespace
	{
		// "owned": taking or using it would be a crime for the one asking (an agent's own things are not).
		void OwnedFlags(json& j, RE::TESObjectREFR* a_ref, RE::Actor* a_viewer)
		{
			if (!a_viewer || a_viewer->IsPlayerRef()) {
				SetIf(j, "owned", a_ref->IsCrimeToActivate());
				return;
			}
			const auto own = Targets::OwnershipFor(a_ref, a_viewer);
			SetIf(j, "owned", own == Targets::Ownership::kOther);
			SetIf(j, "yours", own == Targets::Ownership::kMine);
		}
	}

	json Brief(RE::TESObjectREFR* a_ref, RE::Actor* a_viewer)
	{
		if (auto* actor = a_ref->As<RE::Actor>()) {
			json j = ActorState(actor, a_viewer, false);
			j["kind"] = "actor";
			return j;
		}
		json j;
		j["id"] = Core::FormatFormID(a_ref->GetFormID());
		j["kind"] = Kind(a_ref);
		j["name"] = Name(a_ref);
		Placement(j, a_ref, a_viewer);
		auto* base = a_ref->GetBaseObject();
		const auto kind = j["kind"].get<std::string>();
		if (kind == "item") {
			j["item_kind"] = ItemKind(base);
			const auto count = a_ref->extraList.GetCount();
			if (count > 1) {
				j["count"] = count;
			}
			j["value"] = base->GetGoldValue();
			OwnedFlags(j, a_ref, a_viewer);
		} else if (kind == "door") {
			if (auto dest = a_ref->extraList.GetTeleportLinkedDoor().get()) {
				j["leads_to"] = LocationName(dest->GetParentCell());
			}
			SetIf(j, "locked", a_ref->IsLocked());
		} else if (kind == "container") {
			SetIf(j, "locked", a_ref->IsLocked());
			OwnedFlags(j, a_ref, a_viewer);
		}
		return j;
	}

	json Detailed(RE::TESObjectREFR* a_ref, RE::Actor* a_viewer)
	{
		if (auto* actor = a_ref->As<RE::Actor>()) {
			json j = ActorState(actor, a_viewer, true);
			j["kind"] = "actor";
			if (a_viewer && a_viewer != actor) {
				bool unused = false;
				j["in_sight"] = a_viewer->HasLineOfSight(actor, unused);
			}
			// Not TESNPC::GetObjectVoiceType(): on 1.6.1170 that virtual returned a misaligned
			// non-form pointer for a follower and crashed inspect. The record's own VTCK member is
			// a plain load; still sanity-check it before dereferencing.
			if (auto* base = actor->GetActorBase()) {
				auto* voice = base->voiceType;
				const auto bits = reinterpret_cast<std::uintptr_t>(voice);
				if (voice && (bits & 7) == 0 && voice->GetFormType() == RE::FormType::VoiceType) {
					json v{ { "id", Core::FormatFormID(voice->GetFormID()) } };
					if (const char* edid = voice->GetFormEditorID(); edid && *edid) {
						v["name"] = edid;
					}
					j["voice_type"] = std::move(v);
				}
			}
			return j;
		}
		json j = Brief(a_ref, a_viewer);
		auto* base = a_ref->GetBaseObject();
		j["base_id"] = Core::FormatFormID(base->GetFormID());
		const auto p = a_ref->GetPosition();
		j["position"] = { std::round(p.x), std::round(p.y), std::round(p.z) };
		if (a_viewer) {
			bool unused = false;
			j["in_sight"] = a_viewer->HasLineOfSight(a_ref, unused);
		}
		const auto kind = j["kind"].get<std::string>();
		if (kind == "item") {
			j["weight"] = R1(base->GetWeight());
		}
		if (kind == "door" || kind == "container") {
			if (a_ref->IsLocked()) {
				static constexpr const char* kLevels[] = { "novice", "apprentice", "adept", "expert", "master", "requires_key" };
				const auto lvl = static_cast<int>(a_ref->GetLockLevel());
				j["lock_level"] = lvl >= 0 && lvl <= 5 ? kLevels[lvl] : "unknown";
			}
		}
		if (kind == "container") {
			std::int32_t n = 0;
			for (auto& [obj, count] : a_ref->GetInventoryCounts()) {
				n += count > 0 ? 1 : 0;
			}
			j["item_stacks"] = n;
		}
		return j;
	}

	json Location(RE::TESObjectREFR* a_who)
	{
		json j;
		auto* cell = a_who ? a_who->GetParentCell() : nullptr;
		if (!cell) {
			return j;
		}
		j["interior"] = cell->IsInteriorCell();
		if (const char* n = cell->GetFullName(); n && *n) {
			j["cell"] = n;
		}
		if (auto* ws = a_who->GetWorldspace()) {
			j["worldspace"] = Str(ws->GetFullName());
		}
		if (auto* loc = a_who->GetCurrentLocation()) {
			j["location"] = Str(loc->GetFullName());
			json parents = json::array();
			for (auto* p = loc->parentLoc; p && parents.size() < 4; p = p->parentLoc) {
				if (const char* n = p->GetFullName(); n && *n) {
					parents.push_back(n);
				}
			}
			if (!parents.empty()) {
				j["within"] = parents;
			}
			json kws = json::array();
			for (std::uint32_t i = 0; i < loc->numKeywords; ++i) {
				if (auto* kw = loc->keywords[i]) {
					std::string_view id = Str(kw->GetFormEditorID());
					if (id.starts_with("LocType")) {
						kws.push_back(std::string(id.substr(7)));
					}
				}
			}
			if (!kws.empty()) {
				j["types"] = kws;
			}
			SetIf(j, "cleared", loc->IsCleared());
		}
		if (!cell->IsInteriorCell()) {
			if (auto* sky = RE::Sky::GetSingleton(); sky && sky->currentWeather) {
				using F = RE::TESWeather::WeatherDataFlag;
				const auto& fl = sky->currentWeather->data.flags;
				j["weather"] = fl.any(F::kSnow)   ? "snow" :
				               fl.any(F::kRainy)  ? "rain" :
				               fl.any(F::kCloudy) ? "cloudy" :
				               fl.any(F::kPleasant) ? "clear" : "overcast";
			}
		}
		return j;
	}

	json Time()
	{
		json j;
		if (auto* cal = RE::Calendar::GetSingleton()) {
			char buf[256]{};
			cal->GetTimeDateString(buf, sizeof(buf), true);
			j["text"] = buf;
			j["hour"] = R2(cal->GetHour());
			j["days_passed"] = R2(cal->GetDaysPassed());
		}
		return j;
	}

	json Inventory(RE::TESObjectREFR* a_owner, const std::string& a_filter, std::size_t a_limit)
	{
		struct Row
		{
			std::string name, kind, id;
			std::int32_t count;
			bool worn;
			std::int32_t value;
			float weight;
		};
		std::vector<Row> rows;
		std::int32_t     gold = 0;
		for (auto& [obj, data] : a_owner->GetInventory()) {
			auto& [count, entry] = data;
			if (!obj || count <= 0) {
				continue;
			}
			if (obj->GetFormID() == 0xF) {
				gold = count;
				continue;
			}
			const char* name = obj->GetName();
			if (!name || !*name) {
				continue;  // hidden/scripted tokens
			}
			const auto kind = ItemKind(obj);
			if (!a_filter.empty() && !Core::IContains(name, a_filter) && !Core::IContains(kind, a_filter)) {
				continue;
			}
			rows.push_back({ name, kind, Core::FormatFormID(obj->GetFormID()), count, entry && entry->IsWorn(), obj->GetGoldValue(), obj->GetWeight() });
		}
		std::ranges::sort(rows, [](const Row& a, const Row& b) {
			if (a.worn != b.worn) {
				return a.worn;
			}
			if (a.kind != b.kind) {
				return a.kind < b.kind;
			}
			return a.name < b.name;
		});
		json items = json::array();
		for (const auto& r : rows) {
			if (items.size() >= a_limit) {
				break;
			}
			json it{ { "id", r.id }, { "name", r.name }, { "kind", r.kind }, { "count", r.count }, { "value", r.value }, { "weight", R1(r.weight) } };
			SetIf(it, "equipped", r.worn);
			items.push_back(std::move(it));
		}
		json j{ { "owner", Name(a_owner) }, { "gold", gold }, { "items", items } };
		if (rows.size() > items.size()) {
			j["truncated"] = rows.size() - items.size();
		}
		return j;
	}

	json Quests()
	{
		json quests = json::array();
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return { { "quests", quests } };
		}
		for (auto* q : dh->GetFormArray<RE::TESQuest>()) {
			if (!q || !q->IsRunning()) {
				continue;
			}
			const char* name = q->GetFullName();
			if (!name || !*name) {
				continue;
			}
			json objectives = json::array();
			for (auto* obj : q->objectives) {
				if (obj && obj->state.get() == RE::QUEST_OBJECTIVE_STATE::kDisplayed) {
					objectives.push_back(Core::CleanDisplayText(obj->displayText.c_str()));
				}
			}
			if (objectives.empty()) {
				continue;  // not something the journal is showing as in progress
			}
			json jq{ { "id", Core::FormatFormID(q->GetFormID()) }, { "name", name }, { "type", QuestType(q->GetType()) },
				{ "stage", q->GetCurrentStageID() }, { "objectives", objectives } };
			SetIf(jq, "tracked", q->IsActive());
			quests.push_back(std::move(jq));
		}
		return { { "quests", quests } };
	}

	json Nearby(RE::Actor* a_viewer, const NearbyOptions& a_opts)
	{
		json  items = json::array();
		auto* origin = a_viewer ? static_cast<RE::TESObjectREFR*>(a_viewer) : RE::PlayerCharacter::GetSingleton();
		for (auto* ref : Targets::InRange(origin, a_opts.radiusM * kU)) {
			if (items.size() >= a_opts.limit) {
				break;
			}
			const auto kind = Kind(ref);
			if (kind.empty() || Name(ref).empty()) {
				continue;
			}
			if (!a_opts.kinds.empty() && std::ranges::find(a_opts.kinds, kind) == a_opts.kinds.end()) {
				continue;
			}
			if (a_opts.losOnly && a_viewer) {
				bool unused = false;
				if (!a_viewer->HasLineOfSight(ref, unused)) {
					continue;
				}
			}
			items.push_back(Brief(ref, a_viewer));
		}
		return items;
	}
}
