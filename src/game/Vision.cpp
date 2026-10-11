#include "PCH.h"
#include "Vision.h"
#include "Agents.h"
#include "MainThread.h"

#include "Settings.h"
#include "core/Text.h"
#include "net/Rpc.h"

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#pragma warning(push, 0)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_STATIC
#include <stb_image_resize2.h>
#pragma warning(pop)

using Microsoft::WRL::ComPtr;

namespace Game::Vision
{
	namespace
	{
		const RE::BSFixedString& HeadNode()
		{
			static const RE::BSFixedString name{ "NPC Head [Head]" };
			return name;
		}

		struct Frame
		{
			std::vector<std::uint8_t> rgb;
			std::uint32_t             width{ 0 };
			std::uint32_t             height{ 0 };
			bool                      povApplied{ false };
			std::string               error;
		};

		struct Request
		{
			bool                    pov{ false };
			int                     framesWaited{ 0 };
			float                   povError{ -1.f };
			std::promise<Frame>     promise;
			std::atomic<bool>       done{ false };
		};

		using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
		PresentFn        g_origPresent{ nullptr };
		IDXGISwapChain*  g_swap{ nullptr };
		std::atomic<bool> g_installed{ false };

		std::mutex               g_reqLock;
		std::shared_ptr<Request> g_request;

		// Agent POV: a short free-camera session (what Photo Mode does), steered to the agent's eyes.
		// The main thread owns the session; Present only reads the flags.
		struct PovSession
		{
			RE::FormID actor{ 0 };
			bool       toggled{ false };  // we entered free camera, so we must leave it
			int        frames{ 0 };       // gameplay frames since the session began
			int        freeFrames{ 0 };   // ... of which the camera was actually in free mode
			int        enteredAt{ -1 };   // frame on which free mode was first seen
		};
		std::optional<PovSession> g_pov;  // main thread only
		std::atomic<bool>         g_povReady{ false };
		std::atomic<bool>         g_povOverride{ false };  // the free camera's rendered position is ours
		std::atomic<int>          g_povHookCalls{ 0 };     // GetTranslation calls answered with our eye
		std::mutex                g_povLock;
		RE::NiPoint3              g_povEye;
		RE::NiPoint3              g_povForward;
		int                       g_exitFramesLeft{ 0 };  // main thread: pending exit from free camera
		std::string               g_povLast;              // diagnostic summary of the last session

		ComPtr<ID3D11Texture2D> g_staging;
		ComPtr<ID3D11Texture2D> g_resolve;
		D3D11_TEXTURE2D_DESC    g_stagingDesc{};

		// -------------------------------------------------------------- pixel conversion
		float HalfToFloat(std::uint16_t h)
		{
			const std::uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
			if (e == 0) {
				return (s ? -1.f : 1.f) * std::ldexp(static_cast<float>(m), -24);
			}
			if (e == 31) {
				return m ? 0.f : (s ? -65504.f : 65504.f);
			}
			return (s ? -1.f : 1.f) * std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
		}

		std::uint8_t LinearToSrgb8(float v)
		{
			v = v / (1.f + v);  // gentle tonemap for HDR (scRGB) back buffers
			v = std::clamp(v * 1.6f, 0.f, 1.f);
			v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
			return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f));
		}

		bool ConvertRow(DXGI_FORMAT a_fmt, const std::uint8_t* a_src, std::uint8_t* a_dst, std::uint32_t a_width)
		{
			switch (a_fmt) {
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				for (std::uint32_t x = 0; x < a_width; ++x) {
					a_dst[x * 3 + 0] = a_src[x * 4 + 0];
					a_dst[x * 3 + 1] = a_src[x * 4 + 1];
					a_dst[x * 3 + 2] = a_src[x * 4 + 2];
				}
				return true;
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8X8_UNORM:
			case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
				for (std::uint32_t x = 0; x < a_width; ++x) {
					a_dst[x * 3 + 0] = a_src[x * 4 + 2];
					a_dst[x * 3 + 1] = a_src[x * 4 + 1];
					a_dst[x * 3 + 2] = a_src[x * 4 + 0];
				}
				return true;
			case DXGI_FORMAT_R10G10B10A2_UNORM:
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
				for (std::uint32_t x = 0; x < a_width; ++x) {
					std::uint32_t p;
					std::memcpy(&p, a_src + x * 4, 4);
					a_dst[x * 3 + 0] = static_cast<std::uint8_t>((p & 0x3FF) >> 2);
					a_dst[x * 3 + 1] = static_cast<std::uint8_t>(((p >> 10) & 0x3FF) >> 2);
					a_dst[x * 3 + 2] = static_cast<std::uint8_t>(((p >> 20) & 0x3FF) >> 2);
				}
				return true;
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
				for (std::uint32_t x = 0; x < a_width; ++x) {
					std::uint16_t h[3];
					std::memcpy(h, a_src + x * 8, 6);
					for (int c = 0; c < 3; ++c) {
						a_dst[x * 3 + c] = LinearToSrgb8(std::max(0.f, HalfToFloat(h[c])));
					}
				}
				return true;
			default:
				return false;
			}
		}

		// -------------------------------------------------------------- frame grab (render thread)
		Frame Grab(IDXGISwapChain* a_swap)
		{
			Frame                   f;
			ComPtr<ID3D11Texture2D> back;
			if (FAILED(a_swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) {
				auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
				auto* tex = renderer ? reinterpret_cast<ID3D11Texture2D*>(renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER].texture) : nullptr;
				if (!tex) {
					f.error = "no back buffer available";
					return f;
				}
				back = tex;
			}
			ComPtr<ID3D11Device> dev;
			back->GetDevice(&dev);
			ComPtr<ID3D11DeviceContext> ctx;
			dev->GetImmediateContext(&ctx);

			D3D11_TEXTURE2D_DESC desc{};
			back->GetDesc(&desc);
			ID3D11Texture2D* source = back.Get();
			if (desc.SampleDesc.Count > 1) {
				D3D11_TEXTURE2D_DESC rd = desc;
				rd.SampleDesc = { 1, 0 };
				rd.BindFlags = 0;
				rd.MiscFlags = 0;
				rd.MipLevels = 1;
				rd.ArraySize = 1;
				D3D11_TEXTURE2D_DESC have{};
				if (g_resolve) {
					g_resolve->GetDesc(&have);
				}
				if (!g_resolve || have.Width != rd.Width || have.Height != rd.Height || have.Format != rd.Format) {
					if (FAILED(dev->CreateTexture2D(&rd, nullptr, g_resolve.ReleaseAndGetAddressOf()))) {
						f.error = "could not create resolve texture";
						return f;
					}
				}
				ctx->ResolveSubresource(g_resolve.Get(), 0, back.Get(), 0, desc.Format);
				source = g_resolve.Get();
			}
			if (!g_staging || g_stagingDesc.Width != desc.Width || g_stagingDesc.Height != desc.Height || g_stagingDesc.Format != desc.Format) {
				D3D11_TEXTURE2D_DESC sd{};
				sd.Width = desc.Width;
				sd.Height = desc.Height;
				sd.MipLevels = 1;
				sd.ArraySize = 1;
				sd.Format = desc.Format;
				sd.SampleDesc = { 1, 0 };
				sd.Usage = D3D11_USAGE_STAGING;
				sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				if (FAILED(dev->CreateTexture2D(&sd, nullptr, g_staging.ReleaseAndGetAddressOf()))) {
					f.error = "could not create staging texture";
					return f;
				}
				g_stagingDesc = sd;
			}
			ctx->CopyResource(g_staging.Get(), source);
			D3D11_MAPPED_SUBRESOURCE m{};
			if (FAILED(ctx->Map(g_staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
				f.error = "could not map staging texture";
				return f;
			}
			f.width = desc.Width;
			f.height = desc.Height;
			f.rgb.resize(static_cast<std::size_t>(desc.Width) * desc.Height * 3);
			bool ok = true;
			for (std::uint32_t y = 0; y < desc.Height && ok; ++y) {
				ok = ConvertRow(desc.Format, static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch,
					f.rgb.data() + static_cast<std::size_t>(y) * desc.Width * 3, desc.Width);
			}
			ctx->Unmap(g_staging.Get(), 0);
			if (!ok) {
				f.rgb.clear();
				f.error = "unsupported back buffer format " + std::to_string(static_cast<int>(desc.Format));
			}
			return f;
		}

		bool PovLooksApplied(float& a_err)
		{
			auto* cam = RE::Main::WorldRootCamera();
			if (!cam) {
				a_err = -1.f;
				return false;
			}
			std::lock_guard lock{ g_povLock };
			a_err = cam->world.translate.GetDistance(g_povEye);
			return a_err < 30.f;
		}

		HRESULT STDMETHODCALLTYPE Present_Hook(IDXGISwapChain* a_this, UINT a_sync, UINT a_flags)
		{
			if (a_this == g_swap) {
				std::shared_ptr<Request> req;
				{
					std::lock_guard lock{ g_reqLock };
					req = g_request;
				}
				if (req && !req->done) {
					bool grab = true;
					bool pov = false;
					if (req->pov) {
						if (g_povReady) {
							pov = PovLooksApplied(req->povError);
						} else if (++req->framesWaited < 45) {
							grab = false;  // wait for the free camera to settle on the agent's eyes
						}
					}
					if (grab) {
						Frame f;
						try {
							f = Grab(a_this);
						} catch (...) {
							f.error = "capture threw";
						}
						f.povApplied = pov;
						if (!req->done.exchange(true)) {
							req->promise.set_value(std::move(f));
						}
					}
				}
			}
			return g_origPresent(a_this, a_sync, a_flags);
		}

		// -------------------------------------------------------------- POV session (main thread)
		// The renderer asks the free camera for its position through this virtual (Photo Mode's
		// IGCS bridge overrides the same one). While an agent-view session runs, it gets the eyes.
		struct FreeCameraTranslation
		{
			static void thunk(RE::FreeCameraState* a_this, RE::NiPoint3& a_out)
			{
				func(a_this, a_out);
				if (g_povOverride.load(std::memory_order_acquire)) {
					std::lock_guard lock{ g_povLock };
					a_out = g_povEye;
					g_povHookCalls.fetch_add(1, std::memory_order_relaxed);
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		bool ApplyPov()
		{
			auto* cam = RE::PlayerCamera::GetSingleton();
			auto* actor = g_pov ? RE::TESForm::LookupByID<RE::Actor>(g_pov->actor) : nullptr;
			if (!cam || !actor || !actor->Is3DLoaded() || !cam->IsInFreeCameraMode() || !cam->currentState) {
				return false;
			}
			RE::NiPoint3 eye = actor->GetPosition();
			eye.z += 120.f * actor->GetScale();
			if (auto* root = actor->Get3D()) {
				if (auto* head = root->GetObjectByName(HeadNode())) {
					eye = head->world.translate;
				}
			}
			// Free-camera rotation uses the same convention as an actor's angles: x = pitch, y = yaw.
			const float        yaw = actor->GetAngleZ();
			const float        pitch = std::clamp(actor->GetAngleX(), -1.2f, 1.2f);
			const RE::NiPoint3 fwd{ std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch) };
			eye += fwd * 16.f;  // just in front of the face, so the head does not block the view
			auto* st = static_cast<RE::FreeCameraState*>(cam->currentState.get());
			st->translation = eye;
			st->rotation.x = pitch;
			st->rotation.y = yaw;
			{
				std::lock_guard lock{ g_povLock };
				g_povEye = eye;
				g_povForward = fwd;
			}
			g_povOverride.store(true, std::memory_order_release);
			return true;
		}

		void BeginPov(RE::Actor* a_actor)
		{
			auto*      cam = RE::PlayerCamera::GetSingleton();
			const bool wasFree = cam->IsInFreeCameraMode();
			g_povHookCalls = 0;
			g_povOverride = false;
			if (!wasFree) {
				cam->ToggleFreeCameraMode(false);
			}
			g_pov = PovSession{ a_actor->GetFormID(), !wasFree };
			g_povReady = false;
			ApplyPov();
		}

		void EndPov()
		{
			g_povOverride = false;
			if (!g_pov) {
				return;
			}
			g_povLast = std::format("toggled={} frames={} free_after={} free_frames={} hook_calls={}", g_pov->toggled,
				g_pov->frames, g_pov->enteredAt, g_pov->freeFrames, g_povHookCalls.load());
			auto* cam = RE::PlayerCamera::GetSingleton();
			if (g_pov->toggled && cam) {
				if (cam->IsInFreeCameraMode()) {
					cam->ToggleFreeCameraMode(false);
				} else {
					g_exitFramesLeft = 120;  // the switch into free camera has not landed yet; leave once it does
				}
			}
			g_pov.reset();
			g_povReady = false;
		}

		std::vector<std::uint8_t> Encode(const Frame& a_f, int a_maxDim, const std::string& a_format, int a_quality, std::uint32_t& a_w, std::uint32_t& a_h)
		{
			a_w = a_f.width;
			a_h = a_f.height;
			std::vector<std::uint8_t> scaled;
			const std::uint8_t*       pixels = a_f.rgb.data();
			const auto                longest = std::max(a_w, a_h);
			if (a_maxDim > 0 && longest > static_cast<std::uint32_t>(a_maxDim)) {
				const double s = static_cast<double>(a_maxDim) / longest;
				const auto   w = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(a_w * s)));
				const auto   h = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(a_h * s)));
				scaled.resize(static_cast<std::size_t>(w) * h * 3);
				stbir_resize_uint8_srgb(pixels, static_cast<int>(a_w), static_cast<int>(a_h), 0, scaled.data(), static_cast<int>(w), static_cast<int>(h), 0, STBIR_RGB);
				pixels = scaled.data();
				a_w = w;
				a_h = h;
			}
			std::vector<std::uint8_t> out;
			auto sink = [](void* ctx, void* data, int size) {
				auto* v = static_cast<std::vector<std::uint8_t>*>(ctx);
				v->insert(v->end(), static_cast<std::uint8_t*>(data), static_cast<std::uint8_t*>(data) + size);
			};
			if (a_format == "png") {
				stbi_write_png_to_func(sink, &out, static_cast<int>(a_w), static_cast<int>(a_h), 3, pixels, static_cast<int>(a_w * 3));
			} else {
				stbi_write_jpg_to_func(sink, &out, static_cast<int>(a_w), static_cast<int>(a_h), 3, pixels, std::clamp(a_quality, 30, 100));
			}
			return out;
		}

		std::filesystem::path KeepsakeDir()
		{
			std::filesystem::path dir = Settings::Get().keepsakeFolder.empty() ? std::filesystem::path{} :
			                                                                     std::filesystem::path{ std::u8string(Settings::Get().keepsakeFolder.begin(), Settings::Get().keepsakeFolder.end()) };
			if (dir.empty()) {
				auto logs = logger::log_directory();  // Documents/My Games/Skyrim Special Edition/SKSE
				dir = (logs ? logs->parent_path() : std::filesystem::current_path()) / "Screenshots" / "Animunculi";
			}
			std::filesystem::create_directories(dir);
			return dir;
		}

		std::string Slug(std::string_view a_text)
		{
			std::string out;
			for (const char c : a_text) {
				if (std::isalnum(static_cast<unsigned char>(c))) {
					out += c;
				} else if ((c == ' ' || c == '-' || c == '_') && !out.empty() && out.back() != '-') {
					out += '-';
				}
				if (out.size() >= 48) {
					break;
				}
			}
			while (!out.empty() && out.back() == '-') {
				out.pop_back();
			}
			return out;
		}
	}

	bool Install()
	{
		if (g_installed) {
			return true;
		}
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) {
			return false;
		}
		auto* swap = reinterpret_cast<IDXGISwapChain*>(renderer->GetRuntimeData().renderWindows[0].swapChain);
		if (!swap) {
			return false;
		}
		void** vtable = *reinterpret_cast<void***>(swap);
		DWORD  old{};
		if (!VirtualProtect(&vtable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
			logger::error("vision: cannot patch the swap chain vtable");
			return false;
		}
		g_origPresent = reinterpret_cast<PresentFn>(vtable[8]);
		vtable[8] = reinterpret_cast<void*>(&Present_Hook);
		VirtualProtect(&vtable[8], sizeof(void*), old, &old);
		g_swap = swap;
		if (Settings::Get().allowAgentPerspective) {
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_FreeCameraState[0] };
			FreeCameraTranslation::func = vtbl.write_vfunc(0x5, FreeCameraTranslation::thunk);
		}
		g_installed = true;
		logger::info("vision: Present hooked on swap chain {} (agent perspective {})", static_cast<void*>(swap),
			Settings::Get().allowAgentPerspective ? "enabled (free camera)" : "disabled");
		return true;
	}

	bool Installed() { return g_installed; }

	void Tick(float)
	{
		if (g_exitFramesLeft > 0 && !g_pov) {
			auto* cam = RE::PlayerCamera::GetSingleton();
			if (cam && cam->IsInFreeCameraMode()) {
				cam->ToggleFreeCameraMode(false);
				g_exitFramesLeft = 0;
			} else {
				--g_exitFramesLeft;
			}
		}
		if (!g_pov) {
			return;
		}
		++g_pov->frames;
		if (ApplyPov()) {
			if (g_pov->enteredAt < 0) {
				g_pov->enteredAt = g_pov->frames;
			}
			++g_pov->freeFrames;
			// ready once the renderer has taken our position for a couple of frames (or, if the
			// translation hook is never asked, after a few frames with the state's fields set)
			if ((g_povHookCalls.load() >= 2 && g_pov->freeFrames >= 2) || g_pov->freeFrames >= 6) {
				g_povReady = true;
			}
		}
	}

	json Capture(const json& a_params)
	{
		const auto& cfg = Settings::Get();
		if (!cfg.visionEnabled) {
			throw Net::RpcError("disabled", "vision is disabled ([Vision] Enabled)");
		}
		if (!g_installed) {
			throw Net::RpcError("failed", "vision is unavailable (the Present hook is not installed; see Animunculi.log)");
		}
		const auto perspective = Net::OptString(a_params, "perspective", "player");
		const auto format = Core::Lower(Net::OptString(a_params, "format", "jpeg")) == "png" ? std::string("png") : std::string("jpeg");
		const int  quality = static_cast<int>(Net::OptNumber(a_params, "quality", 80));
		const int  maxDim = std::clamp(static_cast<int>(Net::OptNumber(a_params, "max_dim", 1024)), 64, cfg.maxDimension);
		const bool pov = perspective == "agent";
		const bool save = Net::OptBool(a_params, "save", false);
		const auto caption = Net::OptString(a_params, "caption");
		if (pov && !cfg.allowAgentPerspective) {
			throw Net::RpcError("disabled", "agent perspective is disabled ([Vision] AllowAgentPerspective)");
		}

		// Validate on the main thread and, for the agent's view, start the free-camera session.
		MainThread::Run([&]() -> json {
			MainThread::RequireInGame();
			if (!pov) {
				return nullptr;
			}
			auto* ui = RE::UI::GetSingleton();
			if (ui && (ui->GameIsPaused() || ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME))) {
				throw Net::RpcError("failed", "close menus and dialogue first; the agent view needs the game running");
			}
			BeginPov(Agents::Require(a_params, true));
			return nullptr;
		}, 3s);

		// Whatever happens, leave free camera again.
		struct PovGuard
		{
			bool active;
			void End()
			{
				if (!active) {
					return;
				}
				active = false;
				try {
					MainThread::Run([]() -> json { EndPov(); return nullptr; }, 3s);
				} catch (...) {
					SKSE::GetTaskInterface()->AddTask([] { EndPov(); });
				}
			}
			~PovGuard() { End(); }
		} guard{ pov };

		auto req = std::make_shared<Request>();
		req->pov = pov;
		auto fut = req->promise.get_future();
		{
			std::lock_guard lock{ g_reqLock };
			if (g_request && !g_request->done) {
				throw Net::RpcError("busy", "another capture is in progress");
			}
			g_request = req;
		}
		if (fut.wait_for(5s) != std::future_status::ready) {
			req->done = true;
			throw Net::RpcError("busy", "no frame was presented within 5 s (paused in a menu that does not render, or minimized?)");
		}
		Frame f = fut.get();
		guard.End();  // back to the player's camera before the (slow) encoding
		if (!f.error.empty()) {
			throw Net::RpcError("failed", f.error);
		}

		json out{ { "perspective", pov ? "agent" : "player" }, { "source_width", f.width }, { "source_height", f.height } };
		if (pov) {
			out["pov_applied"] = f.povApplied;  // false: this is the player's view after all
			logger::info("vision: agent view {}: {} camera_error={:.1f} waited={} frames", f.povApplied ? "ok" : "NOT APPLIED",
				g_povLast, req->povError, req->framesWaited);
			if (!f.povApplied) {
				out["diagnostic"] = std::format("{} camera_error={:.1f}", g_povLast, req->povError);
			}
		}
		if (save) {  // keepsake: full resolution, lossless
			std::uint32_t fw = 0, fh = 0;
			const auto    png = Encode(f, 0, "png", 100, fw, fh);
			char          stamp[32]{};
			{
				const std::time_t now = std::time(nullptr);
				std::tm           local{};
				localtime_s(&local, &now);
				std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
			}
			auto          name = std::format("Animunculi_{}_{}", stamp, pov && f.povApplied ? "agent" : "player");
			if (const auto slug = Slug(caption); !slug.empty()) {
				name += "_" + slug;
			}
			try {
				const auto    path = KeepsakeDir() / (name + ".png");
				std::ofstream file{ path, std::ios::binary };
				file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
				if (!file) {
					throw std::runtime_error("write failed");
				}
				out["saved_to"] = path.string();
			} catch (const std::exception& e) {
				out["save_error"] = e.what();
			}
		}
		std::uint32_t w = 0, h = 0;
		auto          bytes = Encode(f, maxDim, format, quality, w, h);
		if (bytes.empty()) {
			throw Net::RpcError("failed", "image encoding failed");
		}
		out["mime"] = format == "png" ? "image/png" : "image/jpeg";
		out["data_b64"] = Core::Base64Encode(bytes);
		out["width"] = w;
		out["height"] = h;
		return out;
	}
}
