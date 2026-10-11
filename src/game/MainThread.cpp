#include "PCH.h"
#include "MainThread.h"

#include "net/Rpc.h"

namespace Game::MainThread
{
	namespace
	{
		struct Pending
		{
			std::mutex              m;
			std::condition_variable cv;
			bool                    started{ false };
			bool                    done{ false };
			bool                    abandoned{ false };
			json                    result;
			std::string             errCode;
			std::string             errMsg;
		};

		std::vector<FrameCallback>  g_callbacks;
		std::atomic<std::uint64_t>  g_frames{ 0 };
		std::atomic<std::int64_t>   g_lastFrameMs{ 0 };

		std::int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		void Tick(float a_delta)
		{
			g_frames.fetch_add(1, std::memory_order_relaxed);
			g_lastFrameMs.store(NowMs(), std::memory_order_relaxed);
			for (auto& cb : g_callbacks) {
				try {
					cb(a_delta);
				} catch (const std::exception& e) {
					static std::uint64_t lastLog = 0;
					if (g_frames - lastLog > 600) {
						lastLog = g_frames;
						logger::error("frame callback threw: {}", e.what());
					}
				}
			}
		}

		struct PlayerUpdateHook
		{
			static void thunk(RE::PlayerCharacter* a_this, float a_delta)
			{
				func(a_this, a_delta);
				Tick(a_delta);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	json Run(const std::function<json()>& a_fn, std::chrono::milliseconds a_timeout)
	{
		auto p = std::make_shared<Pending>();
		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks) {
			throw Net::RpcError("internal", "SKSE task interface unavailable");
		}
		tasks->AddTask([p, a_fn]() {
			{
				std::lock_guard lock{ p->m };
				if (p->abandoned) {
					return;
				}
				p->started = true;
			}
			json        result;
			std::string code, msg;
			try {
				result = a_fn();
			} catch (const Net::RpcError& e) {
				code = e.code;
				msg = e.what();
			} catch (const std::exception& e) {
				code = "internal";
				msg = e.what();
				logger::error("main-thread request threw: {}", e.what());
			} catch (...) {
				code = "internal";
				msg = "unknown exception";
			}
			{
				std::lock_guard lock{ p->m };
				p->result = std::move(result);
				p->errCode = std::move(code);
				p->errMsg = std::move(msg);
				p->done = true;
			}
			p->cv.notify_all();
		});

		std::unique_lock lock{ p->m };
		if (!p->cv.wait_for(lock, a_timeout, [&] { return p->done; })) {
			if (!p->started) {
				p->abandoned = true;  // the task will skip itself
				throw Net::RpcError("busy", "the game did not service the request within " + std::to_string(a_timeout.count()) + " ms");
			}
			// Already running: callers pass stack references into a_fn, so we must not return early.
			p->cv.wait(lock, [&] { return p->done; });
		}
		if (!p->errCode.empty()) {
			throw Net::RpcError(p->errCode, p->errMsg);
		}
		return std::move(p->result);
	}

	void AddFrameCallback(FrameCallback a_callback)
	{
		g_callbacks.push_back(std::move(a_callback));
	}

	void InstallFrameHook()
	{
		static bool installed = false;
		if (installed) {
			return;
		}
		installed = true;
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
		PlayerUpdateHook::func = vtbl.write_vfunc(0xAD, PlayerUpdateHook::thunk);
		logger::info("frame hook installed (PlayerCharacter::Update)");
	}

	std::uint64_t FrameCount() { return g_frames.load(std::memory_order_relaxed); }

	std::int64_t MsSinceLastFrame()
	{
		const auto last = g_lastFrameMs.load(std::memory_order_relaxed);
		return last == 0 ? INT64_MAX / 2 : NowMs() - last;
	}

	bool InGame()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		return player && player->GetParentCell() != nullptr && player->Is3DLoaded();
	}

	void RequireInGame()
	{
		if (!InGame()) {
			throw Net::RpcError("not_in_game", "no game is loaded (main menu or loading screen)");
		}
	}
}
