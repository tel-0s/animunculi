#pragma once

// Bodily verbs beyond walking and fighting: sitting and leaning on real furniture, gestures and
// poses (a curated list of vanilla idles, played through the game's idle system), and spells.
// Main thread.
namespace Game::Body
{
	json Sit(const json& p);         // sit or lean on furniture (bench, chair, log, crate seat, rail...)
	json Stand(const json& p);       // get up from furniture or out of a pose
	json Gestures(const json& p);    // the curated gesture list
	json Spells(const json& p);      // known spells with costs, schools, delivery, hands
	json LearnSpell(const json& p);  // read a spell tome from your inventory

	// emote(animation=...): a curated gesture name, else a raw animation-graph event. A refusal
	// comes with a `reason`; a pose refused mid-walk stops the body and retries (action_finished).
	json PlayGestureOrEvent(RE::Actor* a_actor, const std::string& a_agent, const std::string& a_name);

	// Drop a pending sit or pose retry (follow / move_to / another order took over).
	void Cancel(const std::string& a_agent);

	// Frame callback: steps sit requests (walk, activate, retry) and pose retries.
	void Tick(float a_delta);
}
