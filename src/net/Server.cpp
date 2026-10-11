#include "Server.h"
#include "Rpc.h"

#include "core/Text.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace Net
{
	namespace
	{
		constexpr std::size_t kMaxLine = 48u * 1024u * 1024u;
		constexpr int         kProtocol = 1;

		struct Connection
		{
			SOCKET            sock{ INVALID_SOCKET };
			std::mutex        sendLock;
			std::atomic<bool> open{ true };
			std::string       peer;
			std::string       client;

			void Send(const json& a_msg)
			{
				std::string line;
				try {
					line = a_msg.dump(-1, ' ', false, json::error_handler_t::replace);
				} catch (const std::exception&) {
					line = R"({"error":{"code":"internal","message":"unserializable response"}})";
				}
				line += '\n';
				std::lock_guard lock{ sendLock };
				const char* p = line.data();
				std::size_t left = line.size();
				while (left > 0 && open) {
					const int chunk = static_cast<int>(std::min<std::size_t>(left, 1 << 20));
					const int n = ::send(sock, p, chunk, 0);
					if (n <= 0) {
						Close();
						return;
					}
					p += n;
					left -= static_cast<std::size_t>(n);
				}
			}

			void Close()
			{
				if (open.exchange(false)) {
					::shutdown(sock, SD_BOTH);
					::closesocket(sock);
				}
			}
		};

		ServerConfig                              g_config;
		HelloInfo                                 g_hello;
		LogSink                                   g_log;
		SOCKET                                    g_listen{ INVALID_SOCKET };
		std::atomic<bool>                         g_running{ false };
		std::atomic<int>                          g_port{ 0 };
		std::atomic<int>                          g_inflight{ 0 };
		std::mutex                                g_clientsLock;
		std::set<std::shared_ptr<Connection>>     g_clients;

		void Log(int a_level, const std::string& a_msg)
		{
			if (g_log) {
				g_log(a_level, a_msg);
			}
		}

		json ErrorReply(const json& a_id, const std::string& a_code, const std::string& a_message)
		{
			return json{ { "id", a_id }, { "error", { { "code", a_code }, { "message", a_message } } } };
		}

		void HandleRequest(const std::shared_ptr<Connection>& a_conn, json a_req)
		{
			const json        id = a_req.contains("id") ? a_req["id"] : json(nullptr);
			const std::string method = a_req.value("method", "");
			const json        params = a_req.contains("params") ? a_req["params"] : json::object();
			try {
				auto result = Dispatch(method, params);
				a_conn->Send(json{ { "id", id }, { "result", std::move(result) } });
			} catch (const RpcError& e) {
				a_conn->Send(ErrorReply(id, e.code, e.what()));
			} catch (const std::exception& e) {
				Log(2, "method '" + method + "' threw: " + e.what());
				a_conn->Send(ErrorReply(id, "internal", e.what()));
			} catch (...) {
				Log(2, "method '" + method + "' threw a non-standard exception");
				a_conn->Send(ErrorReply(id, "internal", "unknown exception"));
			}
		}

		bool Authenticate(const std::shared_ptr<Connection>& a_conn, const json& a_req)
		{
			const json id = a_req.contains("id") ? a_req["id"] : json(nullptr);
			if (a_req.value("method", "") != "hello") {
				a_conn->Send(ErrorReply(id, "unauthorized", "the first request must be 'hello'"));
				return false;
			}
			const auto& params = a_req.contains("params") && a_req["params"].is_object() ? a_req["params"] : json::object();
			const std::string token = params.value("token", "");
			if (!g_config.token.empty() && !Core::SecureEquals(token, g_config.token)) {
				a_conn->Send(ErrorReply(id, "unauthorized", "bad token (see Animunculi.discovery.json or [Server] Token)"));
				Log(1, "rejected client " + a_conn->peer + ": bad token");
				return false;
			}
			{
				std::lock_guard lock{ g_clientsLock };  // ClientSummary reads this from other threads
				a_conn->client = params.value("client", "?");
			}
			json info = g_hello ? g_hello() : json::object();
			info["protocol"] = kProtocol;
			a_conn->Send(json{ { "id", id }, { "result", info } });
			Log(0, "client " + a_conn->peer + " authenticated (" + a_conn->client + ")");
			return true;
		}

		void ClientLoop(std::shared_ptr<Connection> a_conn)
		{
			std::string pending;
			bool        authed = false;
			std::vector<char> buf(64 * 1024);
			while (a_conn->open && g_running) {
				const int n = ::recv(a_conn->sock, buf.data(), static_cast<int>(buf.size()), 0);
				if (n <= 0) {
					break;
				}
				pending.append(buf.data(), static_cast<std::size_t>(n));
				std::size_t start = 0;
				for (;;) {
					const auto nl = pending.find('\n', start);
					if (nl == std::string::npos) {
						break;
					}
					std::string_view line{ pending.data() + start, nl - start };
					start = nl + 1;
					if (!line.empty() && line.back() == '\r') {
						line.remove_suffix(1);
					}
					if (line.empty()) {
						continue;
					}
					json req = json::parse(line, nullptr, false);
					if (req.is_discarded() || !req.is_object()) {
						a_conn->Send(ErrorReply(nullptr, "bad_request", "each line must be one JSON object"));
						if (!authed) {
							a_conn->Close();
							break;
						}
						continue;
					}
					if (!authed) {
						if (!Authenticate(a_conn, req)) {
							a_conn->Close();
							break;
						}
						authed = true;
						continue;
					}
					if (g_inflight.fetch_add(1) >= g_config.maxInflight) {
						g_inflight.fetch_sub(1);
						a_conn->Send(ErrorReply(req.value("id", json(nullptr)), "busy", "too many requests in flight"));
						continue;
					}
					std::thread([a_conn, r = std::move(req)]() mutable {
						HandleRequest(a_conn, std::move(r));
						g_inflight.fetch_sub(1);
					}).detach();
				}
				pending.erase(0, start);
				if (pending.size() > kMaxLine) {
					Log(1, "client " + a_conn->peer + " sent an oversized line; disconnecting");
					break;
				}
			}
			a_conn->Close();
			{
				std::lock_guard lock{ g_clientsLock };
				g_clients.erase(a_conn);
			}
			Log(0, "client " + a_conn->peer + " disconnected");
		}

		void AcceptLoop()
		{
			while (g_running) {
				sockaddr_in addr{};
				int         len = sizeof(addr);
				SOCKET      s = ::accept(g_listen, reinterpret_cast<sockaddr*>(&addr), &len);
				if (s == INVALID_SOCKET) {
					if (!g_running) {
						break;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
					continue;
				}
				char host[64]{};
				::inet_ntop(AF_INET, &addr.sin_addr, host, sizeof(host));
				auto conn = std::make_shared<Connection>();
				conn->sock = s;
				conn->peer = std::string(host) + ":" + std::to_string(ntohs(addr.sin_port));
				BOOL nodelay = TRUE;
				::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
				{
					std::lock_guard lock{ g_clientsLock };
					if (static_cast<int>(g_clients.size()) >= g_config.maxClients) {
						conn->Send(ErrorReply(nullptr, "busy", "too many clients"));
						conn->Close();
						continue;
					}
					g_clients.insert(conn);
				}
				std::thread(ClientLoop, conn).detach();
			}
		}
	}

	bool Start(const ServerConfig& a_config, HelloInfo a_hello, LogSink a_log)
	{
		if (g_running) {
			return true;
		}
		g_config = a_config;
		g_hello = std::move(a_hello);
		g_log = std::move(a_log);

		WSADATA wsa{};
		if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			Log(2, "WSAStartup failed");
			return false;
		}
		in_addr bindAddr{};
		if (::inet_pton(AF_INET, g_config.bindAddress.c_str(), &bindAddr) != 1) {
			Log(2, "invalid BindAddress '" + g_config.bindAddress + "'");
			return false;
		}
		for (int i = 0; i < std::max(1, g_config.portSearch); ++i) {
			const int port = g_config.port + i;
			SOCKET    s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (s == INVALID_SOCKET) {
				Log(2, "socket() failed: " + std::to_string(::WSAGetLastError()));
				return false;
			}
			BOOL exclusive = TRUE;
			::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_addr = bindAddr;
			addr.sin_port = htons(static_cast<u_short>(port));
			if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && ::listen(s, 8) == 0) {
				g_listen = s;
				g_port = port;
				break;
			}
			Log(1, "port " + std::to_string(port) + " unavailable (" + std::to_string(::WSAGetLastError()) + ")");
			::closesocket(s);
		}
		if (g_listen == INVALID_SOCKET) {
			Log(2, "could not bind any port starting at " + std::to_string(g_config.port));
			return false;
		}
		g_running = true;
		std::thread(AcceptLoop).detach();
		Log(0, "listening on " + g_config.bindAddress + ":" + std::to_string(g_port.load()));
		return true;
	}

	void Stop()
	{
		if (!g_running.exchange(false)) {
			return;
		}
		::closesocket(g_listen);
		g_listen = INVALID_SOCKET;
		std::lock_guard lock{ g_clientsLock };
		for (auto& c : g_clients) {
			c->Close();
		}
		g_clients.clear();
	}

	int ActualPort() { return g_port; }

	int ClientCount()
	{
		std::lock_guard lock{ g_clientsLock };
		return static_cast<int>(g_clients.size());
	}

	std::string ClientSummary()
	{
		std::lock_guard lock{ g_clientsLock };
		std::string     out;
		for (auto& c : g_clients) {
			if (!out.empty()) {
				out += ", ";
			}
			out += c->client.empty() ? c->peer : c->client;
		}
		return out;
	}
}
