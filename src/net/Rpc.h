#pragma once
// Method registry and dispatch for the wire protocol (docs/PROTOCOL.md). No game dependencies.

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>

namespace Net
{
	using json = nlohmann::json;

	struct RpcError : std::runtime_error
	{
		RpcError(std::string a_code, const std::string& a_message) :
			std::runtime_error(a_message), code(std::move(a_code)) {}
		std::string code;
	};

	enum class Affinity
	{
		kWorker,  // runs on the connection's worker thread (must be thread-safe)
		kMain,    // marshalled onto the game's main thread
	};

	using Handler = std::function<json(const json& a_params)>;
	using MainRunner = std::function<json(const std::function<json()>&, std::chrono::milliseconds)>;

	void Register(const std::string& a_name, Affinity a_affinity, Handler a_handler,
		std::chrono::milliseconds a_timeout = std::chrono::milliseconds{ 5000 });
	void SetMainRunner(MainRunner a_runner);

	// Throws RpcError. `unknown_method` if not registered.
	json Dispatch(const std::string& a_method, const json& a_params);

	// Parameter helpers (throw bad_request on type mismatch).
	std::string RequireString(const json& p, const char* a_key);
	std::string OptString(const json& p, const char* a_key, const std::string& a_default = {});
	double      OptNumber(const json& p, const char* a_key, double a_default);
	bool        OptBool(const json& p, const char* a_key, bool a_default);
}
