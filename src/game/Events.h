#pragma once

#include "core/EventLog.h"

// Feeds world events into the log that agents poll/long-poll (docs/PROTOCOL.md "Events").
namespace Game::Events
{
	Core::EventLog& Log();

	void Install();         // register game event sinks (kDataLoaded)
	void Tick(float);       // frame callback: overheard speech, dialogue choices, agent load state

	// Player -> agent text (chat window, Papyrus, etc.). Thread-safe.
	void PlayerChat(const std::string& a_text, const std::string& a_source, const std::string& a_agent = {});
}
