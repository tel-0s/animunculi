#pragma once

#include "core/Wav.h"

// Speech for embodied agents: positional playback (XAudio2 + X3DAudio) from the actor's head,
// amplitude-driven lip sync on the FaceGen phoneme morphs, subtitles, and expression overrides.
namespace Game::Voice
{
	struct SayRequest
	{
		std::string                  agent;
		RE::Actor*                   actor{ nullptr };
		std::string                  text;
		std::optional<Core::PcmClip> clip;
		std::string                  emotion;       // optional expression while speaking
		RE::TESObjectREFR*           lookAt{ nullptr };
		bool                         subtitle{ true };
		bool                         interrupt{ true };
	};

	// Main thread. Returns {speech_id, duration_ms, lipsync}.
	json Say(SayRequest a_req);
	void Stop(const std::string& a_agent);
	bool IsSpeaking(const std::string& a_agent);

	// Expression override for a_ms milliseconds (0 = until cleared). Main thread.
	bool SetExpression(RE::Actor* a_actor, std::string_view a_emotion, float a_strength, std::uint32_t a_ms);

	void Tick(float a_delta);  // frame callback (main thread)
	void StartAudioThread();   // pause/resume watchdog; call once at load

	// For the chat window: recent lines spoken by agents (thread-safe copy).
	struct Line
	{
		std::string agent, speaker, text;
	};
	std::vector<Line> RecentLines(std::size_t a_max);
}
