#pragma once

// In-game text chat with the agent, drawn through SKSE Menu Framework (optional dependency).
// A hotkey toggles a small window: the conversation so far plus an input line. Enter sends the
// text to agents as a `player_chat` event.
namespace Game::ChatUI
{
	void Install();  // kDataLoaded; no-op when SKSE Menu Framework is not installed
	void Open();
	bool Available();
	void Tick(float);  // main thread: refresh the status line
	void SetThinking(bool a_thinking);  // any thread: "<name> is thinking..." in the status line
}
