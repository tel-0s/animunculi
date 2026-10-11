#include "PCH.h"
#include "Voice.h"
#include "Agents.h"
#include "Events.h"
#include "MainThread.h"
#include "Papyrus.h"
#include "Targets.h"

#include "Settings.h"
#include "core/Text.h"
#include "net/Rpc.h"

// CommonLib pins _WIN32_WINNT to Windows 7; XAudio2 2.9 (the system xaudio2_9.dll) needs Windows 8+.
#pragma push_macro("_WIN32_WINNT")
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include <x3daudio.h>
#include <xaudio2.h>
#pragma pop_macro("_WIN32_WINNT")

namespace Game::Voice
{
	namespace
	{
		const RE::BSFixedString& HeadNode()
		{
			static const RE::BSFixedString name{ "NPC Head [Head]" };
			return name;
		}

		using Clock = std::chrono::steady_clock;
		constexpr float kU = Targets::kUnitsPerMeter;

		// ------------------------------------------------------------------ engine
		struct Engine
		{
			IXAudio2*               xa{ nullptr };
			IXAudio2MasteringVoice* master{ nullptr };
			X3DAUDIO_HANDLE         x3d{};
			UINT32                  outChannels{ 2 };
			bool                    ready{ false };
			std::string             error{ "not started" };
		};
		Engine            g_engine;
		std::atomic<bool> g_engineUp{ false };

		bool InitEngine()
		{
			HRESULT hr = XAudio2Create(&g_engine.xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
			if (FAILED(hr)) {
				g_engine.error = std::format("XAudio2Create failed ({:#x})", static_cast<std::uint32_t>(hr));
				return false;
			}
			hr = g_engine.xa->CreateMasteringVoice(&g_engine.master);
			if (FAILED(hr)) {
				g_engine.error = std::format("no audio output device ({:#x})", static_cast<std::uint32_t>(hr));
				return false;
			}
			DWORD mask = 0;
			g_engine.master->GetChannelMask(&mask);
			XAUDIO2_VOICE_DETAILS details{};
			g_engine.master->GetVoiceDetails(&details);
			g_engine.outChannels = std::clamp<UINT32>(details.InputChannels, 1, 8);
			if (FAILED(X3DAudioInitialize(mask ? mask : SPEAKER_STEREO, X3DAUDIO_SPEED_OF_SOUND, g_engine.x3d))) {
				g_engine.error = "X3DAudioInitialize failed";
				return false;
			}
			g_engine.ready = true;
			g_engine.error.clear();
			logger::info("voice: XAudio2 ready ({} output channels, mask {:#x})", g_engine.outChannels, mask);
			return true;
		}

		// ------------------------------------------------------------------ speech state
		struct Speech
		{
			std::uint64_t             id{ 0 };
			std::string               agent;
			RE::FormID                actor{ 0 };
			std::string               text;
			std::vector<std::int16_t> pcm;  // mono
			std::uint32_t             rate{ 0 };
			std::vector<Core::Viseme> visemes;
			double                    duration{ 0 };
			double                    elapsed{ 0 };  // gameplay time, for subtitle-only lines
			IXAudio2SourceVoice*      voice{ nullptr };
			bool                      started{ false };
			bool                      paused{ false };
			std::string               emotion;
			RE::FormID                lookAt{ 0 };
			bool                      subtitle{ true };
			bool                      subtitleInjected{ false };  // our entry is in SubtitleManager right now
			bool                      subtitleShown{ false };     // we injected at least once (cleanup owed)
			int                       subtitleEvictions{ 0 };
			bool                      lipsActive{ false };
		};

		struct AgentVoice
		{
			std::unique_ptr<Speech>             current;
			std::deque<std::unique_ptr<Speech>> queue;
		};

		std::mutex                                  g_lock;  // guards g_agents (audio thread pauses voices)
		std::map<std::string, AgentVoice>           g_agents;
		std::atomic<std::uint64_t>                  g_nextId{ 1 };
		std::mutex                                  g_linesLock;
		std::deque<Line>                            g_lines;

		struct ExpressionTimer
		{
			RE::FormID        actor;
			Clock::time_point until;
		};
		std::vector<ExpressionTimer> g_exprTimers;

		// ------------------------------------------------------------------ helpers
		float MasterVolume()
		{
			static float    cached = 1.f;
			static Clock::time_point next{};
			if (!Settings::Get().followMasterVolume) {
				return 1.f;
			}
			if (Clock::now() >= next) {
				next = Clock::now() + 2s;
				if (auto* prefs = RE::INIPrefSettingCollection::GetSingleton()) {
					if (auto* s = prefs->GetSetting("fAudioMasterVolume:AudioMenu")) {
						cached = std::clamp(s->GetFloat(), 0.f, 1.f);
					}
				}
			}
			return cached;
		}

		int ExpressionIndex(std::string_view a_emotion)
		{
			const auto e = Core::Lower(a_emotion);
			if (e == "anger" || e == "angry") {
				return 0;
			}
			if (e == "fear" || e == "afraid" || e == "scared") {
				return 1;
			}
			if (e == "happy" || e == "joy" || e == "happiness") {
				return 2;
			}
			if (e == "sad" || e == "sadness") {
				return 3;
			}
			if (e == "surprise" || e == "surprised") {
				return 4;
			}
			if (e == "puzzled" || e == "confused") {
				return 5;
			}
			if (e == "disgust" || e == "disgusted") {
				return 6;
			}
			if (e == "neutral" || e == "calm") {
				return 7;
			}
			return -1;
		}

		void ApplyPhonemes(RE::Actor* a_actor, const float a_w[16])
		{
			auto* fg = a_actor->GetFaceGenAnimationData();
			if (!fg) {
				return;
			}
			RE::BSSpinLockGuard guard{ fg->lock };
			const auto n = std::min<std::uint32_t>(16, fg->phenomeKeyFrame.count);
			for (std::uint32_t i = 0; i < n; ++i) {
				fg->phenomeKeyFrame.SetValue(i, a_w[i]);
			}
		}

		void ClearPhonemes(RE::Actor* a_actor)
		{
			const float zero[16]{};
			ApplyPhonemes(a_actor, zero);
		}

		bool SubtitlePresent(RE::SubtitleManager* sm, RE::ObjectRefHandle a_handle, const std::string& a_text)
		{
			for (auto& info : sm->subtitles) {
				if (info.speaker == a_handle && info.subtitle.c_str() && a_text == info.subtitle.c_str()) {
					return true;
				}
			}
			return false;
		}

		void InjectSubtitle(RE::Actor* a_actor, Speech& s)
		{
			auto* sm = RE::SubtitleManager::GetSingleton();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!sm || !player) {
				return;
			}
			RE::BSSpinLockGuard guard{ sm->lock };
			const auto          handle = a_actor->GetHandle();
			if (SubtitlePresent(sm, handle, s.text)) {
				return;
			}
			RE::SubtitleInfo info{};
			info.speaker = handle;
			info.subtitle = s.text.c_str();
			info.targetDistance = a_actor->GetPosition().GetDistance(player->GetPosition());
			info.forceDisplay = true;
			sm->subtitles.push_back(info);
			s.subtitleInjected = true;
			s.subtitleShown = true;
		}

		// The HUD keeps showing the last subtitle it was sent until it is told otherwise. The game's
		// own removal path sends that message; editing SubtitleManager's array directly does not.
		void HideHudSubtitle()
		{
			auto* queue = RE::UIMessageQueue::GetSingleton();
			auto* strings = RE::InterfaceStrings::GetSingleton();
			if (!queue || !strings) {
				return;
			}
			if (auto* data = static_cast<RE::HUDData*>(queue->CreateUIMessageData(strings->hudData))) {
				data->type = RE::HUD_MESSAGE_TYPE::kHideSubtitle;
				queue->AddMessage(RE::HUDMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kUpdate, data);
			}
		}

		void RemoveSubtitle(RE::Actor* a_actor, Speech& s)
		{
			if (!s.subtitleShown) {
				return;
			}
			s.subtitleShown = false;
			s.subtitleInjected = false;
			auto* sm = RE::SubtitleManager::GetSingleton();
			if (!sm) {
				return;
			}
			bool hide = false;
			{
				RE::BSSpinLockGuard guard{ sm->lock };
				const auto          handle = a_actor ? a_actor->GetHandle() : RE::ObjectRefHandle{};
				// every copy (an eviction and re-injection can leave more than one)
				std::size_t removed = 0;
				for (auto it = sm->subtitles.begin(); it != sm->subtitles.end();) {
					if ((!a_actor || it->speaker == handle) && it->subtitle.c_str() && s.text == it->subtitle.c_str()) {
						it = sm->subtitles.erase(it);
						++removed;
					} else {
						++it;
					}
				}
				const bool wasCurrent = a_actor && sm->currentSpeaker == handle;
				if (wasCurrent) {
					sm->currentSpeaker = RE::ObjectRefHandle{};  // let the manager pick (and show) whatever remains
				}
				hide = wasCurrent || sm->subtitles.empty();
				static int logged = 0;
				if (logged < 3) {
					++logged;
					logger::info("voice: subtitle removed ({} entr{}), was current: {}, others left: {}, hiding: {}", removed,
						removed == 1 ? "y" : "ies", wasCurrent, sm->subtitles.size(), hide);
				}
			}
			if (hide) {
				HideHudSubtitle();
			}
		}

		// Safety net (g_lock held): if an agent body is still the manager's current speaker with
		// nothing queued and no line in progress, our subtitle outlived its line. Clear it once.
		void SweepStaleSubtitle()
		{
			auto* sm = RE::SubtitleManager::GetSingleton();
			if (!sm) {
				return;
			}
			RE::ObjectRefHandle current;
			{
				RE::BSSpinLockGuard guard{ sm->lock };
				if (!sm->subtitles.empty()) {
					return;
				}
				current = sm->currentSpeaker;
			}
			if (current.native_handle() == 0) {
				return;
			}
			auto ref = current.get();
			if (!ref || !Agents::IsAgent(ref.get())) {
				return;
			}
			for (auto& [agent, av] : g_agents) {
				if (av.current && av.current->actor == ref->GetFormID()) {
					return;  // still speaking
				}
			}
			{
				RE::BSSpinLockGuard guard{ sm->lock };
				if (!sm->subtitles.empty() || sm->currentSpeaker != current) {
					return;
				}
				sm->currentSpeaker = RE::ObjectRefHandle{};
			}
			logger::info("voice: cleared a stale subtitle left on the HUD by {}", ref->GetDisplayFullName());
			HideHudSubtitle();
		}

		void ShowSubtitle(RE::Actor* a_actor, Speech& s)
		{
			if (!s.subtitle || s.text.empty()) {
				return;
			}
			const auto& mode = Settings::Get().subtitleMode;
			if (mode == "none") {
				return;
			}
			if (mode == "notification" || s.subtitleEvictions >= 3) {
				const auto msg = std::string(a_actor->GetDisplayFullName()) + ": " + s.text;
				RE::SendHUDMessage::ShowHUDMessage(msg.c_str(), nullptr, false);
				return;
			}
			InjectSubtitle(a_actor, s);
		}

		void SetHeadtrack(RE::Actor* a_actor, RE::TESObjectREFR* a_target)
		{
			if (auto* high = a_actor->GetHighProcess()) {
				if (a_target) {
					high->SetHeadtrackTarget(RE::HighProcessData::HEAD_TRACK_TYPE::kDialogue, a_target);
				} else {
					high->ClearHeadtrackTarget(RE::HighProcessData::HEAD_TRACK_TYPE::kDialogue, false);
				}
			}
		}

		void Update3D(RE::Actor* a_actor, Speech& s)
		{
			if (!s.voice || !g_engine.ready) {
				return;
			}
			auto* cam = RE::PlayerCamera::GetSingleton();
			if (!cam || !cam->cameraRoot) {
				return;
			}
			const auto& w = cam->cameraRoot->world;
			const RE::NiPoint3 right{ w.rotate.entry[0][0], w.rotate.entry[1][0], w.rotate.entry[2][0] };
			const RE::NiPoint3 fwd{ w.rotate.entry[0][1], w.rotate.entry[1][1], w.rotate.entry[2][1] };
			const RE::NiPoint3 up{ w.rotate.entry[0][2], w.rotate.entry[1][2], w.rotate.entry[2][2] };

			RE::NiPoint3 mouth = a_actor->GetPosition();
			mouth.z += 110.f * a_actor->GetScale();
			if (auto* root = a_actor->Get3D()) {
				if (auto* head = root->GetObjectByName(HeadNode())) {
					mouth = head->world.translate;
				}
			}
			const RE::NiPoint3 d = mouth - w.translate;

			X3DAUDIO_LISTENER listener{};
			listener.OrientFront = { 0.f, 0.f, 1.f };
			listener.OrientTop = { 0.f, 1.f, 0.f };

			static X3DAUDIO_DISTANCE_CURVE_POINT points[3];
			static X3DAUDIO_DISTANCE_CURVE       curve{ points, 3 };
			const auto&                          cfg = Settings::Get();
			points[0] = { 0.f, 1.f };
			points[1] = { cfg.fullVolumeDistanceM / cfg.silentDistanceM, 1.f };
			points[2] = { 1.f, 0.f };

			X3DAUDIO_EMITTER emitter{};
			emitter.Position = { d.Dot(right) / kU, d.Dot(up) / kU, d.Dot(fwd) / kU };
			emitter.OrientFront = { 0.f, 0.f, -1.f };
			emitter.OrientTop = { 0.f, 1.f, 0.f };
			emitter.ChannelCount = 1;
			emitter.CurveDistanceScaler = cfg.silentDistanceM;
			emitter.pVolumeCurve = &curve;
			emitter.InnerRadius = 1.0f;
			emitter.InnerRadiusAngle = X3DAUDIO_PI / 4.f;

			float                matrix[8]{};
			X3DAUDIO_DSP_SETTINGS dsp{};
			dsp.SrcChannelCount = 1;
			dsp.DstChannelCount = g_engine.outChannels;
			dsp.pMatrixCoefficients = matrix;
			X3DAudioCalculate(g_engine.x3d, &listener, &emitter, X3DAUDIO_CALCULATE_MATRIX, &dsp);
			s.voice->SetOutputMatrix(g_engine.master, 1, g_engine.outChannels, matrix);
			s.voice->SetVolume(cfg.volume * MasterVolume());
		}

		void AddLine(const Speech& s, RE::Actor* a_actor)
		{
			std::lock_guard lock{ g_linesLock };
			g_lines.push_back({ s.agent, a_actor ? a_actor->GetDisplayFullName() : s.agent, s.text });
			while (g_lines.size() > 100) {
				g_lines.pop_front();
			}
		}

		// Main thread, g_lock held.
		bool Start(Speech& s)
		{
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(s.actor);
			if (!actor) {
				return false;
			}
			if (!s.pcm.empty() && g_engineUp) {
				WAVEFORMATEX fmt{};
				fmt.wFormatTag = WAVE_FORMAT_PCM;
				fmt.nChannels = 1;
				fmt.nSamplesPerSec = s.rate;
				fmt.wBitsPerSample = 16;
				fmt.nBlockAlign = 2;
				fmt.nAvgBytesPerSec = s.rate * 2;
				HRESULT hr = g_engine.xa->CreateSourceVoice(&s.voice, &fmt);
				if (SUCCEEDED(hr)) {
					XAUDIO2_BUFFER buf{};
					buf.AudioBytes = static_cast<UINT32>(s.pcm.size() * sizeof(std::int16_t));
					buf.pAudioData = reinterpret_cast<const BYTE*>(s.pcm.data());
					buf.Flags = XAUDIO2_END_OF_STREAM;
					hr = s.voice->SubmitSourceBuffer(&buf);
				}
				if (FAILED(hr)) {
					logger::warn("voice: could not start playback ({:#x}); subtitle only", static_cast<std::uint32_t>(hr));
					if (s.voice) {
						s.voice->DestroyVoice();
						s.voice = nullptr;
					}
				} else {
					Update3D(actor, s);
					s.voice->Start(0);
				}
			}
			s.started = true;
			ShowSubtitle(actor, s);
			if (!s.emotion.empty()) {
				SetExpression(actor, s.emotion, 0.75f, 0);
			}
			if (s.lookAt) {
				if (auto* target = RE::TESForm::LookupByID<RE::TESObjectREFR>(s.lookAt)) {
					SetHeadtrack(actor, target);
				}
			}
			AddLine(s, actor);
			Events::Log().Push("speech_started", { { "speech_id", s.id }, { "agent", s.agent }, { "speaker", actor->GetDisplayFullName() }, { "text", s.text },
				{ "duration_ms", static_cast<std::int64_t>(s.duration * 1000.0) } });
			Papyrus::SendModEvent("Animunculi_SpeechStarted", s.text, static_cast<float>(s.duration), actor);
			return true;
		}

		// Main thread, g_lock held.
		void Finish(Speech& s, bool a_interrupted)
		{
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(s.actor);
			if (s.voice) {
				s.voice->Stop(0);
				s.voice->DestroyVoice();  // blocks until the voice is released; pcm stays alive until then
				s.voice = nullptr;
			}
			if (actor) {
				if (s.lipsActive) {
					ClearPhonemes(actor);
				}
				if (!s.emotion.empty()) {
					actor->ClearExpressionOverride();
				}
				if (s.lookAt) {
					SetHeadtrack(actor, nullptr);
				}
			}
			RemoveSubtitle(actor, s);
			if (s.started) {
				Events::Log().Push("speech_finished", { { "speech_id", s.id }, { "agent", s.agent }, { "interrupted", a_interrupted } });
				if (actor) {
					Papyrus::SendModEvent("Animunculi_SpeechFinished", s.text, a_interrupted ? 1.f : 0.f, actor);
				}
			}
		}

		// Main thread, g_lock held. Returns false when the line is over.
		bool Advance(Speech& s, float a_delta)
		{
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(s.actor);
			if (!actor || actor->IsDead()) {
				return false;
			}
			double t = 0;
			if (s.voice) {
				XAUDIO2_VOICE_STATE st{};
				s.voice->GetState(&st);
				if (st.BuffersQueued == 0) {
					return false;
				}
				t = static_cast<double>(st.SamplesPlayed) / s.rate;
				Update3D(actor, s);
			} else {
				s.elapsed += a_delta;
				t = s.elapsed;
				if (t >= s.duration) {
					return false;
				}
			}

			if (!s.visemes.empty() && Settings::Get().lipSync && actor->Is3DLoaded()) {
				const auto idx = std::min<std::size_t>(static_cast<std::size_t>(t * Core::kVisemeRate), s.visemes.size() - 1);
				auto       v = s.visemes[idx];
				v.open = std::clamp(v.open * Settings::Get().lipSyncGain, 0.f, 1.f);
				float w[16];
				Core::VisemeToPhonemes(v, w);
				ApplyPhonemes(actor, w);
				s.lipsActive = true;
			}

			// The game may prune subtitles it did not create; re-add a few times, then fall back.
			if (s.subtitleInjected) {
				if (auto* sm = RE::SubtitleManager::GetSingleton()) {
					bool present;
					{
						RE::BSSpinLockGuard guard{ sm->lock };
						present = SubtitlePresent(sm, actor->GetHandle(), s.text);
					}
					if (!present) {
						s.subtitleInjected = false;
						if (++s.subtitleEvictions >= 3) {
							logger::info("voice: subtitle was removed by the game; using HUD notifications instead");
						}
						ShowSubtitle(actor, s);
					}
				}
			}
			return true;
		}

		void AudioThread()
		{
			const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			if (Settings::Get().voiceEnabled && InitEngine()) {
				g_engineUp = true;
			} else if (Settings::Get().voiceEnabled) {
				logger::error("voice: {}; agents will speak in subtitles only", g_engine.error);
			}
			bool paused = false;
			for (;;) {
				std::this_thread::sleep_for(50ms);
				if (!g_engineUp) {
					continue;
				}
				// Pause voices while the game is paused (menus) or loading: no gameplay frames arrive then.
				const bool shouldPause = MainThread::MsSinceLastFrame() > 250;
				if (shouldPause == paused) {
					continue;
				}
				paused = shouldPause;
				std::lock_guard lock{ g_lock };
				for (auto& [agent, av] : g_agents) {
					if (av.current && av.current->voice) {
						if (paused) {
							av.current->voice->Stop(0);
						} else {
							av.current->voice->Start(0);
						}
						av.current->paused = paused;
					}
				}
			}
			if (SUCCEEDED(co)) {
				CoUninitialize();
			}
		}
	}

	json Say(SayRequest a_req)
	{
		if (!a_req.actor) {
			throw Net::RpcError("not_bound", "no body to speak with");
		}
		auto s = std::make_unique<Speech>();
		s->id = g_nextId++;
		s->agent = a_req.agent;
		s->actor = a_req.actor->GetFormID();
		s->text = a_req.text;
		s->emotion = a_req.emotion;
		s->subtitle = a_req.subtitle;
		s->lookAt = a_req.lookAt ? a_req.lookAt->GetFormID() : 0;
		bool lipsync = false;
		if (a_req.clip && Settings::Get().voiceEnabled) {
			auto& clip = *a_req.clip;
			s->rate = clip.sampleRate;
			const auto frames = clip.frames();
			s->pcm.resize(frames);
			for (std::size_t i = 0; i < frames; ++i) {
				float v = clip.channels == 2 ? 0.5f * (clip.samples[i * 2] + clip.samples[i * 2 + 1]) : clip.samples[i];
				s->pcm[i] = static_cast<std::int16_t>(std::lround(std::clamp(v, -1.f, 1.f) * 32767.f));
			}
			s->duration = clip.seconds();
			if (Settings::Get().lipSync) {
				s->visemes = Core::AnalyseVisemes(clip);
				lipsync = !s->visemes.empty();
			}
		} else {
			s->duration = Core::ReadingSeconds(a_req.text);
		}
		const auto id = s->id;
		const auto duration = s->duration;
		double     queuedAhead = 0;

		std::lock_guard lock{ g_lock };
		auto&           av = g_agents[a_req.agent];
		if (a_req.interrupt) {
			if (av.current) {
				Finish(*av.current, true);
				av.current.reset();
			}
			av.queue.clear();
		}
		if (!av.current) {
			av.current = std::move(s);
			if (!Start(*av.current)) {
				av.current.reset();
				throw Net::RpcError("not_loaded", "the body is gone");
			}
		} else {
			queuedAhead = std::max(0.0, av.current->duration - av.current->elapsed);
			for (auto& q : av.queue) {
				queuedAhead += q->duration;
			}
			av.queue.push_back(std::move(s));
		}
		json out{ { "speech_id", id }, { "duration_ms", static_cast<std::int64_t>(duration * 1000.0) }, { "lipsync", lipsync },
			{ "audio", g_engineUp.load() && a_req.clip.has_value() } };
		if (queuedAhead > 0) {
			out["queued_behind_ms"] = static_cast<std::int64_t>(queuedAhead * 1000.0);
		}
		if (a_req.clip && !g_engineUp) {
			out["audio_error"] = g_engine.error;
		}
		return out;
	}

	void Stop(const std::string& a_agent)
	{
		std::lock_guard lock{ g_lock };
		auto            it = g_agents.find(a_agent);
		if (it == g_agents.end()) {
			return;
		}
		it->second.queue.clear();
		if (it->second.current) {
			Finish(*it->second.current, true);
			it->second.current.reset();
		}
	}

	bool IsSpeaking(const std::string& a_agent)
	{
		std::lock_guard lock{ g_lock };
		auto            it = g_agents.find(a_agent);
		return it != g_agents.end() && it->second.current != nullptr;
	}

	bool SetExpression(RE::Actor* a_actor, std::string_view a_emotion, float a_strength, std::uint32_t a_ms)
	{
		const int idx = ExpressionIndex(a_emotion);
		auto*     fg = a_actor ? a_actor->GetFaceGenAnimationData() : nullptr;
		if (idx < 0 || !fg) {
			return false;
		}
		fg->SetExpressionOverride(static_cast<std::uint32_t>(idx), std::clamp(a_strength, 0.f, 1.f));
		std::erase_if(g_exprTimers, [&](const ExpressionTimer& t) { return t.actor == a_actor->GetFormID(); });
		if (a_ms > 0) {
			g_exprTimers.push_back({ a_actor->GetFormID(), Clock::now() + std::chrono::milliseconds(a_ms) });
		}
		return true;
	}

	void Tick(float a_delta)
	{
		{
			std::lock_guard lock{ g_lock };
			for (auto& [agent, av] : g_agents) {
				if (av.current && !Advance(*av.current, a_delta)) {
					Finish(*av.current, false);
					av.current.reset();
				}
				while (!av.current && !av.queue.empty()) {
					av.current = std::move(av.queue.front());
					av.queue.pop_front();
					if (!Start(*av.current)) {
						av.current.reset();
					}
				}
			}
			static std::uint32_t frame = 0;
			if (++frame % 60 == 0) {
				SweepStaleSubtitle();
			}
		}
		const auto now = Clock::now();
		for (auto it = g_exprTimers.begin(); it != g_exprTimers.end();) {
			if (now >= it->until) {
				if (auto* a = RE::TESForm::LookupByID<RE::Actor>(it->actor)) {
					a->ClearExpressionOverride();
				}
				it = g_exprTimers.erase(it);
			} else {
				++it;
			}
		}
	}

	void StartAudioThread()
	{
		static bool started = false;
		if (!started) {
			started = true;
			std::thread(AudioThread).detach();
		}
	}

	std::vector<Line> RecentLines(std::size_t a_max)
	{
		std::lock_guard lock{ g_linesLock };
		const auto      n = std::min(a_max, g_lines.size());
		return { g_lines.end() - static_cast<std::ptrdiff_t>(n), g_lines.end() };
	}
}
