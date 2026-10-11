#include "PCH.h"
#include "Knowledge.h"
#include "Agents.h"
#include "Describe.h"
#include "MainThread.h"
#include "Targets.h"

#include "core/Text.h"
#include "net/Rpc.h"

namespace Game::Knowledge
{
	namespace
	{
		using Net::RpcError;
		constexpr float kU = Targets::kUnitsPerMeter;
		constexpr float kReadReachM = 6.f;

		const char* Str(const char* s) { return s ? s : ""; }
		double      R1(double v) { return std::round(v * 10.0) / 10.0; }

		RE::Actor* Viewer(const json& p, bool& a_bound)
		{
			auto* body = Agents::Get(Agents::AgentOf(p));
			a_bound = body && body->Is3DLoaded() && body->GetParentCell();
			return a_bound ? body : RE::PlayerCharacter::GetSingleton();
		}

		// ------------------------------------------------------------------ world positions
		// Where something is on its worldspace map. Indoors, that is the map marker of the
		// enclosing location (approximate), which is also what the game's map shows.
		struct Anchor
		{
			RE::TESWorldSpace* ws{ nullptr };
			RE::NiPoint3       pos{};
			bool               approximate{ false };
		};

		Anchor WorldAnchor(RE::TESObjectREFR* a_ref)
		{
			if (!a_ref) {
				return {};
			}
			auto* cell = a_ref->GetParentCell();
			if (cell && cell->IsExteriorCell()) {
				return { a_ref->GetWorldspace(), a_ref->GetPosition(), false };
			}
			RE::BGSLocation* loc = a_ref->GetCurrentLocation();
			if (!loc && cell) {
				loc = cell->GetLocation();
			}
			for (int i = 0; loc && i < 8; ++i, loc = loc->parentLoc) {
				if (auto marker = loc->worldLocMarker.get()) {
					if (auto* mcell = marker->GetParentCell(); mcell && mcell->IsExteriorCell()) {
						return { marker->GetWorldspace(), marker->GetPosition(), true };
					}
				}
			}
			return {};
		}

		struct Marker
		{
			RE::TESObjectREFR*  ref;
			RE::MapMarkerData*  data;
		};

		std::vector<Marker> MarkersIn(RE::TESWorldSpace* a_ws)
		{
			std::vector<Marker> out;
			if (!a_ws || !a_ws->persistentCell) {
				return out;
			}
			a_ws->persistentCell->ForEachReference([&](RE::TESObjectREFR* a_r) {
				if (a_r) {
					if (auto* x = a_r->extraList.GetByType<RE::ExtraMapMarker>(); x && x->mapData) {
						out.push_back({ a_r, x->mapData });
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return out;
		}

		std::string MarkerName(const RE::MapMarkerData* d) { return Str(d->locationName.fullName.c_str()); }

		const char* MarkerType(RE::MARKER_TYPE t)
		{
			static constexpr const char* kNames[] = { "place", "city", "town", "settlement", "cave", "camp", "fort", "nordic ruin",
				"dwemer ruin", "shipwreck", "grove", "landmark", "dragon lair", "farm", "wood mill", "mine", "imperial camp",
				"stormcloak camp", "standing stone", "wheat mill", "smelter", "stable", "imperial tower", "clearing", "pass",
				"altar", "rock", "lighthouse", "orc stronghold", "giant camp", "shack", "nordic tower", "nordic dwelling", "docks",
				"shrine", "castle", "palace", "castle" };
			const auto i = static_cast<std::size_t>(t);
			return i < std::size(kNames) ? kNames[i] : "place";
		}

		// Distance/direction from the viewer's anchor to a world position on the same map.
		void Direction(json& a_out, const Anchor& a_from, RE::TESObjectREFR* a_viewer, const RE::NiPoint3& a_to)
		{
			const float dx = a_to.x - a_from.pos.x, dy = a_to.y - a_from.pos.y;
			const float d = std::sqrt(dx * dx + dy * dy) / kU;
			if (d >= 1000.f) {
				a_out["dist_km"] = R1(d / 1000.f);
			} else {
				a_out["dist_m"] = static_cast<int>(std::lround(d));
			}
			const float deg = std::atan2(dx, dy) * 180.f / std::numbers::pi_v<float>;
			a_out["direction"] = Core::CompassWord(deg);
			if (!a_from.approximate && a_viewer) {  // relative bearing only makes sense outdoors
				const float rel = Core::WrapDegrees(deg - a_viewer->GetAngleZ() * 180.f / std::numbers::pi_v<float>);
				a_out["bearing"] = static_cast<int>(std::lround(rel));
			}
		}

		std::string LocName(RE::TESObjectREFR* a_ref)
		{
			if (auto* loc = a_ref->GetCurrentLocation(); loc && loc->GetFullName() && *loc->GetFullName()) {
				return loc->GetFullName();
			}
			auto* cell = a_ref->GetParentCell();
			return cell ? Str(cell->GetFullName()) : "";
		}

		int MatchTier(std::string_view a_name, std::string_view a_spec)
		{
			if (a_name.empty()) {
				return -1;
			}
			if (Core::IEquals(a_name, a_spec)) {
				return 0;
			}
			if (a_name.size() >= a_spec.size() && Core::IEquals(a_name.substr(0, a_spec.size()), a_spec)) {
				return 1;
			}
			return Core::IContains(a_name, a_spec) ? 2 : -1;
		}

		bool HasLocType(RE::BGSLocation* a_loc, std::string_view a_type)
		{
			for (std::uint32_t i = 0; a_loc && i < a_loc->numKeywords; ++i) {
				if (auto* kw = a_loc->keywords[i]; kw && Core::IEquals(Str(kw->GetFormEditorID()), a_type)) {
					return true;
				}
			}
			return false;
		}
	}

	// ====================================================================== books
	json Read(const json& p)
	{
		MainThread::RequireInGame();
		bool       bound = false;
		auto*      reader = Viewer(p, bound);
		const auto spec = Net::OptString(p, "target");
		auto*      ui = RE::UI::GetSingleton();

		RE::TESObjectBOOK* book = nullptr;
		std::string        source;
		if (spec.empty() || Core::IEquals(spec, "open")) {
			if (!ui || !ui->IsMenuOpen(RE::BookMenu::MENU_NAME) || !(book = RE::BookMenu::GetTargetForm())) {
				throw RpcError("bad_request", "name a book (id or title), or use target=\"open\" while the player has a book open");
			}
			source = "open in the player's hands";
		} else {
			if (bound) {
				try {
					auto item = Targets::Item(reader, spec);
					book = item.object->As<RE::TESObjectBOOK>();
					source = "your inventory";
				} catch (const RpcError&) {
				}
			}
			if (!book) {
				auto* ref = Targets::Ref(spec, bound ? reader : nullptr, 40.f * kU);
				book = ref->GetBaseObject() ? ref->GetBaseObject()->As<RE::TESObjectBOOK>() : nullptr;
				if (!book) {
					throw RpcError("failed", Describe::Name(ref) + " is not a book or note");
				}
				const float d = ref->GetPosition().GetDistance(reader->GetPosition()) / kU;
				if (d > kReadReachM) {
					throw RpcError("failed", std::format("{} is {:.1f} m away; get within {:.0f} m to read it (move_to)", Describe::Name(ref), d, kReadReachM));
				}
				source = "nearby";
			}
		}

		RE::BSString raw;
		static_cast<RE::TESDescription*>(book)->GetDescription(raw, book);
		const auto text = Core::BookToText(raw.c_str() ? raw.c_str() : "");

		const auto offset = static_cast<std::size_t>(std::max(0.0, Net::OptNumber(p, "offset", 0)));
		const auto maxChars = static_cast<std::size_t>(std::clamp(Net::OptNumber(p, "max_chars", 6000), 200.0, 40000.0));
		std::size_t start = std::min(offset, text.size());
		std::size_t end = std::min(text.size(), start + maxChars);
		if (end < text.size()) {  // do not cut words (or UTF-8 sequences) in half
			const auto ws = text.find_last_of(" \n", end);
			if (ws != std::string::npos && ws > start + maxChars / 2) {
				end = ws;
			}
		}
		json j{ { "id", Core::FormatFormID(book->GetFormID()) }, { "title", Str(book->GetName()) }, { "source", source },
			{ "kind", book->IsNoteScroll() ? "note" : book->TeachesSpell() ? "spell_tome" : book->TeachesSkill() ? "skill_book" : "book" },
			{ "text", text.substr(start, end - start) }, { "total_chars", text.size() }, { "offset", start } };
		if (end < text.size()) {
			j["next_offset"] = end;
		}
		if (book->TeachesSkill()) {
			j["note"] = "reading it yourself teaches you nothing mechanically; the player would gain the skill by reading it";
		}
		return j;
	}

	// ====================================================================== map
	json Map(const json& p)
	{
		MainThread::RequireInGame();
		bool         bound = false;
		auto*        viewer = Viewer(p, bound);
		const Anchor here = WorldAnchor(viewer);
		const float  radiusKm = static_cast<float>(std::clamp(Net::OptNumber(p, "radius_km", 3.0), 0.2, 60.0));
		const auto   limit = static_cast<std::size_t>(std::clamp(Net::OptNumber(p, "limit", 12), 1.0, 60.0));

		json j;
		{
			json h = Describe::Location(viewer);
			if (auto* loc = viewer->GetCurrentLocation()) {
				for (auto* l = loc; l; l = l->parentLoc) {
					if (HasLocType(l, "LocTypeHold")) {
						h["hold"] = Str(l->GetFullName());
						break;
					}
				}
			}
			if (here.ws) {
				h["map"] = Str(here.ws->GetFullName());
			}
			if (here.approximate) {
				h["map_position"] = "indoors; distances are from this location's entrance on the map";
			}
			if (auto landmark = NearestPlace(viewer); !landmark.is_null()) {
				h["near"] = std::move(landmark);
			}
			j["here"] = std::move(h);
		}

		if (here.ws) {
			struct Row
			{
				float d;
				json  j;
			};
			std::vector<Row> rows;
			for (auto& m : MarkersIn(here.ws)) {
				if (!m.data->flags.any(RE::MapMarkerData::Flag::kVisible)) {
					continue;  // not on the player's map yet
				}
				const auto name = MarkerName(m.data);
				if (name.empty()) {
					continue;
				}
				const auto  pos = m.ref->GetPosition();
				const float d = std::hypot(pos.x - here.pos.x, pos.y - here.pos.y) / kU;
				if (d > radiusKm * 1000.f) {
					continue;
				}
				json r{ { "name", name }, { "type", MarkerType(m.data->type.get()) } };
				Direction(r, here, viewer, pos);
				if (m.data->flags.any(RE::MapMarkerData::Flag::kCanTravelTo)) {
					r["discovered"] = true;
				}
				rows.push_back({ d, std::move(r) });
			}
			std::ranges::sort(rows, {}, &Row::d);
			json places = json::array();
			for (auto& r : rows) {
				if (places.size() >= limit) {
					break;
				}
				places.push_back(std::move(r.j));
			}
			j["places"] = std::move(places);
		}

		// Quest markers: the resolved targets of every objective the journal is showing.
		json targets = json::array();
		if (auto* dh = RE::TESDataHandler::GetSingleton()) {
			for (auto* q : dh->GetFormArray<RE::TESQuest>()) {
				if (!q || !q->IsRunning() || !q->GetFullName() || !*q->GetFullName()) {
					continue;
				}
				for (auto* obj : q->objectives) {
					if (!obj || obj->state.get() != RE::QUEST_OBJECTIVE_STATE::kDisplayed || !obj->targets) {
						continue;
					}
					for (std::uint32_t i = 0; i < obj->numTargets && targets.size() < 24; ++i) {
						if (!obj->targets[i]) {
							continue;
						}
						RE::ObjectRefHandle targetHandle;
						obj->targets[i]->GetTargetRef(targetHandle, true, q);
						auto ref = targetHandle.get();
						if (!ref) {
							continue;
						}
						json t{ { "quest", q->GetFullName() }, { "objective", Core::CleanDisplayText(obj->displayText.c_str()) },
							{ "target", Describe::Name(ref.get()) }, { "where", LocName(ref.get()) } };
						if (q->IsActive()) {
							t["tracked"] = true;
						}
						if (ref->GetParentCell() && ref->GetParentCell() == viewer->GetParentCell()) {
							t["here"] = true;
							Describe::Placement(t, ref.get(), viewer);
						} else if (const Anchor a = WorldAnchor(ref.get()); a.ws && a.ws == here.ws) {
							Direction(t, here, viewer, a.pos);
						} else if (a.ws) {
							t["map"] = Str(a.ws->GetFullName());
						}
						targets.push_back(std::move(t));
					}
				}
			}
		}
		j["quest_targets"] = std::move(targets);

		// Indoors: the ways out.
		if (auto* cell = viewer->GetParentCell(); cell && cell->IsInteriorCell()) {
			struct Exit
			{
				float d;
				json  j;
			};
			std::vector<Exit> exits;
			cell->ForEachReference([&](RE::TESObjectREFR* a_r) {
				if (a_r && a_r->GetBaseObject() && a_r->GetBaseObject()->Is(RE::FormType::Door)) {
					if (auto dest = a_r->extraList.GetTeleportLinkedDoor().get()) {
						json e{ { "id", Core::FormatFormID(a_r->GetFormID()) }, { "name", Describe::Name(a_r) }, { "leads_to", LocName(dest.get()) } };
						Describe::Placement(e, a_r, viewer);
						exits.push_back({ a_r->GetPosition().GetDistance(viewer->GetPosition()), std::move(e) });
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			std::ranges::sort(exits, {}, &Exit::d);
			json ex = json::array();
			for (auto& e : exits) {
				if (ex.size() < 8) {
					ex.push_back(std::move(e.j));
				}
			}
			j["exits"] = std::move(ex);
		}
		return j;
	}

	json Locate(const json& p)
	{
		MainThread::RequireInGame();
		const auto spec = Net::RequireString(p, "name");
		bool       bound = false;
		auto*      viewer = Viewer(p, bound);
		const Anchor here = WorldAnchor(viewer);
		auto*      dh = RE::TESDataHandler::GetSingleton();

		struct Hit
		{
			int   tier;
			float d;
			json  j;
		};
		std::vector<Hit> hits;
		for (auto* ws : dh->GetFormArray<RE::TESWorldSpace>()) {
			if (!ws) {
				continue;
			}
			const std::string wsName = Str(ws->GetFullName());
			if (const int t = MatchTier(wsName, spec); t >= 0) {
				// A whole region (e.g. Blackreach): report how it connects to the rest of the world.
				std::set<std::string> entrances;
				if (ws->persistentCell) {
					ws->persistentCell->ForEachReference([&](RE::TESObjectREFR* a_r) {
						if (a_r && entrances.size() < 10) {
							if (auto dest = a_r->extraList.GetTeleportLinkedDoor().get()) {
								if (auto n = LocName(dest.get()); !n.empty() && n != wsName) {
									entrances.insert(n);
								}
							}
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
				}
				json r{ { "name", wsName }, { "type", "region (separate map)" }, { "entrances", entrances } };
				if (ws == here.ws) {
					r["note"] = "you are in it";
				}
				hits.push_back({ t, 0.f, std::move(r) });
			}
			for (auto& m : MarkersIn(ws)) {
				const auto name = MarkerName(m.data);
				const int  t = MatchTier(name, spec);
				if (t < 0) {
					continue;
				}
				json r{ { "name", name }, { "type", MarkerType(m.data->type.get()) }, { "map", wsName },
					{ "on_your_map", m.data->flags.any(RE::MapMarkerData::Flag::kVisible) },
					{ "discovered", m.data->flags.any(RE::MapMarkerData::Flag::kCanTravelTo) } };
				float d = 1e12f;
				if (ws == here.ws && here.ws) {
					Direction(r, here, viewer, m.ref->GetPosition());
					d = m.ref->GetPosition().GetDistance(here.pos);
				} else {
					r["note"] = "on another map; travel there through an entrance";
				}
				hits.push_back({ t, d, std::move(r) });
			}
		}
		std::ranges::sort(hits, [](const Hit& a, const Hit& b) { return a.tier != b.tier ? a.tier < b.tier : a.d < b.d; });
		json out = json::array();
		for (auto& h : hits) {
			if (out.size() < 6) {
				out.push_back(std::move(h.j));
			}
		}
		if (out.empty()) {
			throw RpcError("not_found", "no place called '" + spec + "' on any map");
		}
		json res{ { "matches", out } };
		if (here.approximate) {
			res["from"] = "distances are from the entrance of where you are now";
		}
		return res;
	}

	// ====================================================================== dialogue
	json DialogueOptions(const json&)
	{
		MainThread::RequireInGame();
		auto* mtm = RE::MenuTopicManager::GetSingleton();
		if (!mtm || !mtm->menuOpen) {
			return { { "open", false } };
		}
		json  topics = json::array();
		json  last = nullptr;
		auto* cs = reinterpret_cast<LPCRITICAL_SECTION>(&mtm->criticalSection);
		EnterCriticalSection(cs);
		RE::NiPointer<RE::TESObjectREFR> speaker = mtm->speaker.get();
		if (mtm->dialogueList) {
			for (auto* d : *mtm->dialogueList) {
				if (d && d->topicText.c_str() && *d->topicText.c_str()) {
					topics.push_back(d->topicText.c_str());
				}
			}
		}
		if (mtm->lastSelectedDialogue && mtm->lastSelectedDialogue->topicText.c_str()) {
			last = mtm->lastSelectedDialogue->topicText.c_str();
		}
		LeaveCriticalSection(cs);
		json j{ { "open", true }, { "topics", topics }, { "last_chosen", last },
			{ "note", "these are the player's lines; suggest one out loud, the player chooses" } };
		if (speaker) {
			j["with"] = { { "id", Core::FormatFormID(speaker->GetFormID()) }, { "name", Describe::Name(speaker.get()) } };
		}
		return j;
	}

	// ====================================================================== nearest landmark
	json NearestPlace(RE::TESObjectREFR* a_ref)
	{
		auto* cell = a_ref ? a_ref->GetParentCell() : nullptr;
		auto* ws = a_ref ? a_ref->GetWorldspace() : nullptr;
		if (!cell || !cell->IsExteriorCell() || !ws) {
			return nullptr;  // indoors the location name already says where you are
		}
		static RE::TESWorldSpace*  cachedWs = nullptr;
		static std::vector<Marker> cache;  // map-marker refs are persistent; flags are read live
		if (cachedWs != ws) {
			cache = MarkersIn(ws);
			cachedWs = ws;
		}
		const auto     here = a_ref->GetPosition();
		const Marker*  best = nullptr;
		const Marker*  bestAny = nullptr;
		float          bestD = 1e30f, bestAnyD = 1e30f;
		for (const auto& m : cache) {
			if (MarkerName(m.data).empty()) {
				continue;
			}
			const auto  p = m.ref->GetPosition();
			const float d = std::hypot(p.x - here.x, p.y - here.y);
			if (m.data->flags.any(RE::MapMarkerData::Flag::kVisible) && d < bestD) {
				best = &m;
				bestD = d;
			}
			if (d < bestAnyD) {
				bestAny = &m;
				bestAnyD = d;
			}
		}
		bool known = true;
		if (!best || bestD > 3000.f * kU) {  // nothing on the player's map within 3 km
			best = bestAny;
			known = false;
		}
		if (!best) {
			return nullptr;
		}
		const auto p = best->ref->GetPosition();
		json j{ { "name", MarkerName(best->data) }, { "type", MarkerType(best->data->type.get()) } };
		Direction(j, Anchor{ ws, here, false }, a_ref, p);
		if (!known) {
			j["on_your_map"] = false;
		}
		const std::string dist = j.contains("dist_km") ? std::format("{} km", j["dist_km"].get<double>()) : std::format("{} m", j["dist_m"].get<int>());
		j["text"] = std::format("near {} ({} {})", j["name"].get<std::string>(), dist, j["direction"].get<std::string>());
		return j;
	}
}
