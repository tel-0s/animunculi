// Native unit tests for the game-independent core: WAV decoding, lip-sync analysis, text helpers,
// the event log and the TCP/JSON server. Run build/vs2026/Release/animunculi_tests.exe.

#include "core/EventLog.h"
#include "core/ItemBuffer.h"
#include "core/Text.h"
#include "core/Wav.h"
#include "game/Guard.h"
#include "net/Rpc.h"
#include "net/Server.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

namespace
{
	int g_failures = 0;
	int g_checks = 0;

#define CHECK(cond)                                                                   \
	do {                                                                              \
		++g_checks;                                                                   \
		if (!(cond)) {                                                                \
			++g_failures;                                                             \
			std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
		}                                                                             \
	} while (0)

	using json = nlohmann::json;

	void Put16(std::vector<std::uint8_t>& v, std::uint16_t x) { v.push_back(x & 0xFF), v.push_back(x >> 8); }
	void Put32(std::vector<std::uint8_t>& v, std::uint32_t x)
	{
		for (int i = 0; i < 4; ++i) {
			v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
		}
	}

	std::vector<std::uint8_t> MakeWav(std::uint16_t tag, std::uint16_t ch, std::uint32_t rate, std::uint16_t bits,
		const std::vector<std::uint8_t>& data, bool streamed = false, bool extensible = false)
	{
		std::vector<std::uint8_t> v;
		v.insert(v.end(), { 'R', 'I', 'F', 'F' });
		Put32(v, streamed ? 0xFFFFFFFF : 0);  // patched below
		v.insert(v.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
		Put32(v, extensible ? 40 : 16);
		Put16(v, extensible ? 0xFFFE : tag);
		Put16(v, ch);
		Put32(v, rate);
		Put32(v, rate * ch * bits / 8);
		Put16(v, static_cast<std::uint16_t>(ch * bits / 8));
		Put16(v, bits);
		if (extensible) {
			Put16(v, 22);
			Put16(v, bits);
			Put32(v, 0);
			Put16(v, tag);  // SubFormat GUID starts with the format tag
			for (int i = 0; i < 14; ++i) {
				v.push_back(0);
			}
		}
		v.insert(v.end(), { 'd', 'a', 't', 'a' });
		Put32(v, streamed ? 0xFFFFFFFF : static_cast<std::uint32_t>(data.size()));
		v.insert(v.end(), data.begin(), data.end());
		if (!streamed) {
			const auto riff = static_cast<std::uint32_t>(v.size() - 8);
			std::memcpy(v.data() + 4, &riff, 4);
		}
		return v;
	}

	std::vector<std::uint8_t> Pcm16(const std::vector<float>& samples)
	{
		std::vector<std::uint8_t> d;
		for (float s : samples) {
			Put16(d, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(s * 32767.f))));
		}
		return d;
	}

	void TestText()
	{
		std::puts("text");
		for (std::size_t n = 0; n < 20; ++n) {
			std::vector<std::uint8_t> v(n);
			for (std::size_t i = 0; i < n; ++i) {
				v[i] = static_cast<std::uint8_t>(i * 37 + 11);
			}
			const auto enc = Core::Base64Encode(v);
			const auto dec = Core::Base64Decode(enc);
			CHECK(dec && *dec == v);
		}
		CHECK(Core::Base64Encode(std::vector<std::uint8_t>{ 'M', 'a', 'n' }) == "TWFu");
		CHECK(!Core::Base64Decode("ab$d"));
		CHECK(Core::Base64Decode("-_-_") == Core::Base64Decode("+/+/"));

		CHECK(Core::ParseFormID("0x0001A696") == 0x0001A696u);
		CHECK(Core::ParseFormID("0XFF000800") == 0xFF000800u);
		CHECK(!Core::ParseFormID("1A696"));
		CHECK(Core::ParseFormID("1A696", true) == 0x1A696u);
		CHECK(!Core::ParseFormID("0x123456789"));
		CHECK(!Core::ParseFormID("Lydia"));
		CHECK(Core::FormatFormID(0x14) == "0x00000014");

		CHECK(Core::SecureEquals("abc", "abc"));
		CHECK(!Core::SecureEquals("abc", "abd"));
		CHECK(!Core::SecureEquals("abc", "abcd"));
		CHECK(!Core::SecureEquals("", "x"));

		CHECK(Core::CleanDisplayText("Bring <Alias=Item> to  <Alias=Npc>\n") == "Bring Item to Npc");
		CHECK(Core::CleanDisplayText("Plain") == "Plain");
		CHECK(Core::IEquals("LYDIA", "lydia"));
		CHECK(Core::IContains("Whiterun Guard", "guard"));
		CHECK(!Core::IContains("Lydia", "guard"));

		CHECK(Core::BearingWord(0) == "ahead");
		CHECK(Core::BearingWord(45) == "ahead-right");
		CHECK(Core::BearingWord(-90) == "left");
		CHECK(Core::BearingWord(179) == "behind");
		CHECK(Core::BearingWord(-135) == "behind-left");
		CHECK(std::abs(Core::WrapDegrees(270.f) + 90.f) < 1e-4f);
		CHECK(std::abs(Core::WrapDegrees(-190.f) - 170.f) < 1e-4f);
	}

	void TestBookText()
	{
		std::puts("book text");
		const std::string_view book =
			"<font face='$HandwrittenFont'><p align=\"center\">The Lusty Argonian Maid</p>\r\n"
			"<p align='left'>Lifts-Her-Tail: Certainly not, kind sir! I am here but to clean your floors.</p>"
			"[pagebreak]\n"
			"<img src='img://Textures/Interface/Books/Illuminated_Letters/C_letter.png' width='58' height='61'>"
			"rantius &amp; Lifts-Her-Tail&nbsp;&nbsp;talk.<br>Line two, &lt;quoted&gt;, &quot;hi&quot;<br>"
			"Hello <Alias=Player>!</font>";
		const auto text = Core::BookToText(book);
		CHECK(text.starts_with("The Lusty Argonian Maid\n\nLifts-Her-Tail: Certainly not"));
		CHECK(text.find("\n\n---\n\n") != std::string::npos);
		CHECK(text.find("rantius & Lifts-Her-Tail talk.\nLine two, <quoted>, \"hi\"\nHello Player!") != std::string::npos);
		CHECK(text.find('<') == text.find("<quoted>"));
		CHECK(text.find("img://") == std::string::npos);
		CHECK(text.find("\n\n\n") == std::string::npos);
		CHECK(Core::BookToText("") == "");
		CHECK(Core::BookToText("[pagebreak]Only page two") == "Only page two");

		CHECK(Core::CompassWord(0) == "N");
		CHECK(Core::CompassWord(44) == "NE");
		CHECK(Core::CompassWord(90) == "E");
		CHECK(Core::CompassWord(-90) == "W");
		CHECK(Core::CompassWord(359) == "N");
		CHECK(Core::CompassWord(200) == "S");
	}

	void TestWav()
	{
		std::puts("wav");
		Core::PcmClip clip;
		std::string   err;

		std::vector<float> ramp{ 0.f, 0.5f, -0.5f, 1.f, -1.f };
		CHECK(Core::DecodeWav(MakeWav(1, 1, 22050, 16, Pcm16(ramp)), clip, err));
		CHECK(clip.sampleRate == 22050 && clip.channels == 1 && clip.frames() == 5);
		CHECK(std::abs(clip.samples[1] - 0.5f) < 1e-3f && std::abs(clip.samples[4] + 1.f) < 1e-3f);

		// 24-bit stereo
		std::vector<std::uint8_t> d24{ 0x00, 0x00, 0x40, 0x00, 0x00, 0xC0 };  // +0.5, -0.5
		CHECK(Core::DecodeWav(MakeWav(1, 2, 48000, 24, d24), clip, err));
		CHECK(clip.channels == 2 && clip.frames() == 1);
		CHECK(std::abs(clip.samples[0] - 0.5f) < 1e-4f && std::abs(clip.samples[1] + 0.5f) < 1e-4f);

		// float32, WAVE_FORMAT_EXTENSIBLE
		std::vector<std::uint8_t> df(8);
		const float fs[2] = { 0.25f, -0.75f };
		std::memcpy(df.data(), fs, 8);
		CHECK(Core::DecodeWav(MakeWav(3, 1, 24000, 32, df, false, true), clip, err));
		CHECK(clip.frames() == 2 && std::abs(clip.samples[1] + 0.75f) < 1e-6f);

		// 8-bit unsigned
		CHECK(Core::DecodeWav(MakeWav(1, 1, 8000, 8, { 128, 255, 0 }), clip, err));
		CHECK(std::abs(clip.samples[0]) < 1e-6f && clip.samples[1] > 0.99f && std::abs(clip.samples[2] + 1.f) < 1e-6f);

		// streamed header sizes (TTS servers)
		CHECK(Core::DecodeWav(MakeWav(1, 1, 16000, 16, Pcm16(ramp), true), clip, err));
		CHECK(clip.frames() == 5);

		// rejects
		CHECK(!Core::DecodeWav(std::vector<std::uint8_t>{ 'I', 'D', '3', 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, clip, err));
		CHECK(!Core::DecodeWav(MakeWav(2, 1, 22050, 4, { 1, 2, 3, 4 }), clip, err));  // ADPCM
		CHECK(err.find("unsupported") != std::string::npos);
		CHECK(!Core::DecodeWav(MakeWav(1, 6, 22050, 16, Pcm16(ramp)), clip, err));     // 5.1

		auto i16 = Core::ToInt16(Core::PcmClip{ 1, 1, { 1.f, -1.f, 0.f } });
		CHECK(i16[0] == 32767 && i16[1] == -32767 && i16[2] == 0);
	}

	void TestVisemes()
	{
		std::puts("visemes");
		// 0.3 s silence, 0.4 s 200 Hz tone, 0.3 s silence, 0.4 s 3 kHz tone
		const std::uint32_t rate = 22050;
		Core::PcmClip       clip{ rate, 1, {} };
		auto                tone = [&](float hz, float secs, float amp) {
			for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(secs * rate); ++i) {
				clip.samples.push_back(amp * std::sin(2.f * std::numbers::pi_v<float> * hz * i / rate));
			}
		};
		tone(0, 0.3f, 0);
		tone(200, 0.4f, 0.6f);
		tone(0, 0.3f, 0);
		tone(3000, 0.4f, 0.6f);
		const auto v = Core::AnalyseVisemes(clip);
		CHECK(!v.empty());
		auto at = [&](double s) { return v[std::min(v.size() - 1, static_cast<std::size_t>(s * Core::kVisemeRate))]; };
		CHECK(at(0.1).open < 0.05f);
		CHECK(at(0.5).open > 0.7f);
		CHECK(at(0.95).open < 0.1f);
		CHECK(at(1.2).open > 0.7f);
		CHECK(at(1.2).bright > at(0.5).bright + 0.3f);  // 3 kHz reads brighter than 200 Hz
		bool closure = false;
		for (std::size_t i = static_cast<std::size_t>(0.25 * Core::kVisemeRate); i < static_cast<std::size_t>(0.35 * Core::kVisemeRate); ++i) {
			closure |= v[i].closed > 0.5f;
		}
		CHECK(closure);  // lips close just before the first syllable

		for (const auto& x : v) {
			float w[16];
			Core::VisemeToPhonemes(x, w);
			for (float f : w) {
				CHECK(f >= 0.f && f <= 1.f);
			}
		}
		float w[16];
		Core::VisemeToPhonemes({ 1.f, 0.5f, 0.f }, w);
		CHECK(w[0] > 0.5f);   // Aah dominant for an open neutral vowel
		Core::VisemeToPhonemes({ 0.f, 0.5f, 1.f }, w);
		CHECK(w[2] > 0.9f);   // BMP for closed lips

		CHECK(Core::ReadingSeconds("") == 1.5);
		CHECK(Core::ReadingSeconds("one two three four five six seven eight nine ten") > 3.5);
		CHECK(Core::ReadingSeconds(std::string(5000, 'a') + " b") <= 14.0);
	}

	void TestEventLog()
	{
		std::puts("event log");
		Core::EventLog log{ 4 };
		CHECK(log.NextSeq() == 1);
		log.Push("a", { { "x", 1 } });
		log.Push("b");
		log.Push("a", { { "x", 2 } });
		auto r = log.Query(1, {}, 10);
		CHECK(r.events.size() == 3 && r.nextSeq == 4);
		CHECK(r.events[0]["type"] == "a" && r.events[0]["x"] == 1 && r.events[0].contains("t"));
		r = log.Query(1, { "a" }, 10);
		CHECK(r.events.size() == 2 && r.nextSeq == 4);
		r = log.Query(1, {}, 2);
		CHECK(r.events.size() == 2 && r.nextSeq == 3);  // resume at the first unreturned event
		r = log.Query(4, { "a" }, 10);
		CHECK(r.events.empty() && r.nextSeq == 4);
		log.Push("c");
		log.Push("d");  // capacity 4 evicts seq 1
		r = log.Query(1, {}, 10);
		CHECK(r.dropped == 1 && r.events.front()["seq"] == 2);

		const auto t0 = std::chrono::steady_clock::now();
		r = log.Wait(log.NextSeq(), {}, 10, std::chrono::milliseconds(80));
		CHECK(r.events.empty() && std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(70));

		const auto since = log.NextSeq();
		std::thread producer([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			log.Push("noise");
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			log.Push("player_chat", { { "text", "hi" } });
		});
		r = log.Wait(since, { "player_chat" }, 10, std::chrono::seconds(5));
		producer.join();
		CHECK(r.events.size() == 1 && r.events[0]["text"] == "hi");
	}

	void TestItemBuffer()
	{
		std::puts("item buffer");
		using namespace std::chrono_literals;
		Core::ItemBuffer b{ 1500ms };
		const auto t0 = Core::ItemBuffer::Clock::now();
		// NFF RefreshVisual: ring added, removed 0.3 s later -> nothing reported
		CHECK(b.Add({ "default", 0x56208BB4, 1, true, json{ { "item", "Silver Ring" } }, t0 }));
		CHECK(!b.Add({ "default", 0x56208BB4, 1, false, json::object(), t0 + 300ms }));
		CHECK(b.Ready(t0 + 5s).empty());
		// a real gift is reported once it has been held long enough
		CHECK(b.Add({ "default", 0x0003EADE, 2, true, json{ { "item", "Potion" } }, t0 }));
		CHECK(b.Ready(t0 + 1s).empty());
		auto ready = b.Ready(t0 + 2s);
		CHECK(ready.size() == 1 && ready[0].payload["item"] == "Potion");
		// different count or agent does not cancel
		CHECK(b.Add({ "default", 0xA, 1, true, {}, t0 }));
		CHECK(b.Add({ "default", 0xA, 2, false, {}, t0 }));
		CHECK(b.Add({ "serana", 0xA, 1, false, {}, t0 }));
		CHECK(b.Ready(t0 + 2s).size() == 3);
	}

	void TestGuard()
	{
		std::puts("guard");
		Game::Guard::Fault fault;
		// an access violation is contained and reported
		const bool ok = Game::Guard::Run([](void*) {
			volatile int* bad = nullptr;
			*bad = 1;
		}, nullptr, fault);
		CHECK(!ok && fault.code == 0xC0000005u && fault.address != nullptr);
		CHECK(Game::Guard::Describe(fault.address).find("animunculi_tests.exe+") == 0);
		// normal completion
		int value = 0;
		CHECK(Game::Guard::Run([](void* c) { *static_cast<int*>(c) = 42; }, &value, fault) && value == 42);
		// C++ exceptions are not intercepted: they reach the caller's handlers
		bool caught = false;
		try {
			Game::Guard::Run([](void*) { throw std::runtime_error("cxx"); }, nullptr, fault);
		} catch (const std::runtime_error&) {
			caught = true;
		}
		CHECK(caught);
	}

	// ---------------------------------------------------------------- server
	struct Client
	{
		SOCKET      s{ INVALID_SOCKET };
		std::string buf;

		bool Connect(int port)
		{
			s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			sockaddr_in a{};
			a.sin_family = AF_INET;
			a.sin_port = htons(static_cast<u_short>(port));
			::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
			DWORD timeout = 3000;
			::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
			return ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
		}
		void Send(const std::string& line)
		{
			const auto l = line + "\n";
			::send(s, l.data(), static_cast<int>(l.size()), 0);
		}
		std::optional<json> Recv()
		{
			for (;;) {
				if (auto nl = buf.find('\n'); nl != std::string::npos) {
					auto line = buf.substr(0, nl);
					buf.erase(0, nl + 1);
					return json::parse(line);
				}
				char tmp[4096];
				const int n = ::recv(s, tmp, sizeof(tmp), 0);
				if (n <= 0) {
					return std::nullopt;
				}
				buf.append(tmp, static_cast<std::size_t>(n));
			}
		}
		~Client() { ::closesocket(s); }
	};

	void TestServer()
	{
		std::puts("server");
		Net::Register("echo", Net::Affinity::kWorker, [](const json& p) { return p; });
		Net::Register("slow", Net::Affinity::kWorker, [](const json&) {
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			return json{ { "slow", true } };
		});
		Net::Register("fails", Net::Affinity::kWorker, [](const json&) -> json { throw Net::RpcError("not_found", "nope"); });
		Net::Register("on_main", Net::Affinity::kMain, [](const json&) { return json{ { "main", true } }; });
		Net::SetMainRunner([](const std::function<json()>& fn, std::chrono::milliseconds) { return fn(); });

		CHECK(Net::Dispatch("echo", json{ { "a", 1 } })["a"] == 1);
		CHECK(Net::Dispatch("on_main", json::object())["main"] == true);
		bool threw = false;
		try {
			Net::Dispatch("nope", json::object());
		} catch (const Net::RpcError& e) {
			threw = e.code == "unknown_method";
		}
		CHECK(threw);

		Net::ServerConfig cfg;
		cfg.port = 47790;
		cfg.token = "s3cret";
		CHECK(Net::Start(cfg, [] { return json{ { "plugin", "test" }, { "event_seq", 7 } }; }, [](int, const std::string& m) { std::printf("    [net] %s\n", m.c_str()); }));
		const int port = Net::ActualPort();

		{
			Client c;
			CHECK(c.Connect(port));
			c.Send(R"({"id":1,"method":"echo","params":{}})");
			auto r = c.Recv();
			CHECK(r && (*r)["error"]["code"] == "unauthorized");
			CHECK(!c.Recv());  // closed
		}
		{
			Client c;
			CHECK(c.Connect(port));
			c.Send(R"({"id":1,"method":"hello","params":{"token":"wrong"}})");
			auto r = c.Recv();
			CHECK(r && (*r)["error"]["code"] == "unauthorized");
		}
		{
			Client c;
			CHECK(c.Connect(port));
			c.Send(R"({"id":"h","method":"hello","params":{"token":"s3cret","client":"test"}})");
			auto r = c.Recv();
			CHECK(r && (*r)["id"] == "h" && (*r)["result"]["protocol"] == 1 && (*r)["result"]["event_seq"] == 7);
			// pipelined: slow first, fast second -> fast answers first
			c.Send(R"({"id":2,"method":"slow"})");
			c.Send(R"({"id":3,"method":"echo","params":{"text":"héllo wörld"}})");
			auto a = c.Recv();
			auto b = c.Recv();
			CHECK(a && (*a)["id"] == 3 && (*a)["result"]["text"] == "héllo wörld");
			CHECK(b && (*b)["id"] == 2 && (*b)["result"]["slow"] == true);
			c.Send(R"({"id":4,"method":"fails"})");
			auto e = c.Recv();
			CHECK(e && (*e)["error"]["code"] == "not_found" && (*e)["error"]["message"] == "nope");
			c.Send("this is not json");
			auto bad = c.Recv();
			CHECK(bad && (*bad)["error"]["code"] == "bad_request");
			c.Send(R"({"id":5,"method":"missing"})");
			auto um = c.Recv();
			CHECK(um && (*um)["error"]["code"] == "unknown_method");
			// large payload (2 MB) round trip
			std::string big(2 * 1024 * 1024, 'x');
			c.Send(json{ { "id", 6 }, { "method", "echo" }, { "params", { { "blob", big } } } }.dump());
			auto large = c.Recv();
			CHECK(large && (*large)["result"]["blob"].get<std::string>().size() == big.size());
		}
		Net::Stop();
	}
}

// `animunculi_tests --serve <port> <token>`: a headless endpoint using the real server and event
// log, so the Python bridge can be integration-tested against the C++ implementation.
int Serve(int a_port, const std::string& a_token)
{
	static Core::EventLog log{ 256 };
	auto events = [](const json& p, bool wait) {
		const auto since = p.value("since", std::uint64_t{ 0 });
		const auto limit = p.value("limit", std::size_t{ 100 });
		std::vector<std::string> types;
		if (p.contains("types") && p["types"].is_array()) {
			for (auto& t : p["types"]) {
				types.push_back(t.get<std::string>());
			}
		}
		auto r = wait ? log.Wait(since, types, limit, std::chrono::milliseconds(p.value("timeout_ms", 0))) : log.Query(since, types, limit);
		return json{ { "events", r.events }, { "next_seq", r.nextSeq }, { "dropped", r.dropped } };
	};
	Net::Register("ping", Net::Affinity::kWorker, [](const json&) { return json{ { "pong", true } }; });
	Net::Register("status", Net::Affinity::kWorker, [](const json&) { return json{ { "in_game", false }, { "headless", true } }; });
	Net::Register("events", Net::Affinity::kWorker, [events](const json& p) { return events(p, false); });
	Net::Register("wait_events", Net::Affinity::kWorker, [events](const json& p) { return events(p, true); });
	// test hook: queue an event after a delay
	Net::Register("emit", Net::Affinity::kWorker, [](const json& p) {
		std::thread([p] {
			std::this_thread::sleep_for(std::chrono::milliseconds(p.value("delay_ms", 0)));
			log.Push(p.value("type", "custom"), p.value("data", json::object()));
		}).detach();
		return json{ { "ok", true } };
	});
	Net::Register("echo", Net::Affinity::kWorker, [](const json& p) { return p; });
	Net::ServerConfig cfg;
	cfg.port = a_port;
	cfg.portSearch = 1;
	cfg.token = a_token;
	if (!Net::Start(cfg, [] { return json{ { "plugin", "Animunculi" }, { "plugin_version", "headless" }, { "event_seq", log.NextSeq() } }; }, {})) {
		return 3;
	}
	std::printf("serving on %d\n", Net::ActualPort());
	std::fflush(stdout);
	for (;;) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
}

int main(int argc, char** argv)
{
	WSADATA wsa;
	::WSAStartup(MAKEWORD(2, 2), &wsa);
	if (argc == 4 && std::string(argv[1]) == "--serve") {
		return Serve(std::atoi(argv[2]), argv[3]);
	}
	TestText();
	TestBookText();
	TestWav();
	TestVisemes();
	TestEventLog();
	TestItemBuffer();
	TestGuard();
	TestServer();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
