#pragma once
// Line-delimited JSON over TCP with token auth (docs/PROTOCOL.md). Winsock only; no game dependencies.

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace Net
{
	struct ServerConfig
	{
		std::string bindAddress{ "127.0.0.1" };
		int         port{ 47700 };
		int         portSearch{ 10 };  // try port..port+portSearch-1 if busy
		std::string token;
		int         maxClients{ 8 };
		int         maxInflight{ 64 };  // concurrent requests across all clients
	};

	using HelloInfo = std::function<nlohmann::json()>;
	using LogSink = std::function<void(int a_level, const std::string& a_message)>;  // 0 info, 1 warn, 2 error

	bool        Start(const ServerConfig& a_config, HelloInfo a_hello, LogSink a_log);
	void        Stop();
	int         ActualPort();
	int         ClientCount();
	std::string ClientSummary();
}
