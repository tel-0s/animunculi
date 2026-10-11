#pragma once

namespace Game::MainThread
{
	// Runs `a_fn` on the game's main thread via the SKSE task queue and waits for the result.
	// Throws Net::RpcError("busy") on timeout; the task is then skipped if it has not started.
	json Run(const std::function<json()>& a_fn, std::chrono::milliseconds a_timeout);

	// Per-frame callbacks, driven by PlayerCharacter::Update (gameplay frames only; not while
	// the game is paused in menus or at loading screens). Register before InstallFrameHook.
	using FrameCallback = std::function<void(float a_delta)>;
	void AddFrameCallback(FrameCallback a_callback);
	void InstallFrameHook();

	std::uint64_t FrameCount();
	// Milliseconds since the last gameplay frame (large while paused or loading).
	std::int64_t MsSinceLastFrame();

	// True when a save is loaded and the player is in a cell. Main thread only.
	bool InGame();
	void RequireInGame();
}
