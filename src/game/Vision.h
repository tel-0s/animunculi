#pragma once

// Frame capture for vision-capable models. "player" perspective copies the presented frame;
// "agent" perspective briefly switches to the free camera (like Photo Mode) at the agent's eyes.
namespace Game::Vision
{
	bool Install();  // hooks IDXGISwapChain::Present; call after the renderer exists
	bool Installed();
	void Tick(float);  // frame callback: steers the free camera during an agent-view capture

	// Worker thread. Blocks until a frame is captured and encoded (or throws busy/disabled/failed).
	json Capture(const json& a_params);
}
