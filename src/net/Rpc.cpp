#include "Rpc.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace Net
{
	namespace
	{
		struct Method
		{
			Affinity                  affinity;
			Handler                   handler;
			std::chrono::milliseconds timeout;
		};

		std::shared_mutex                       g_lock;
		std::unordered_map<std::string, Method> g_methods;
		MainRunner                              g_mainRunner;
	}

	void Register(const std::string& a_name, Affinity a_affinity, Handler a_handler, std::chrono::milliseconds a_timeout)
	{
		std::unique_lock lock{ g_lock };
		g_methods[a_name] = Method{ a_affinity, std::move(a_handler), a_timeout };
	}

	void SetMainRunner(MainRunner a_runner)
	{
		std::unique_lock lock{ g_lock };
		g_mainRunner = std::move(a_runner);
	}

	json Dispatch(const std::string& a_method, const json& a_params)
	{
		Method     method;
		MainRunner runner;
		{
			std::shared_lock lock{ g_lock };
			const auto       it = g_methods.find(a_method);
			if (it == g_methods.end()) {
				throw RpcError("unknown_method", "no method '" + a_method + "'");
			}
			method = it->second;
			runner = g_mainRunner;
		}
		const json& params = a_params.is_object() ? a_params : json::object();
		if (method.affinity == Affinity::kMain) {
			if (!runner) {
				throw RpcError("internal", "main-thread runner not installed");
			}
			auto handler = method.handler;
			return runner([handler, params]() { return handler(params); }, method.timeout);
		}
		return method.handler(params);
	}

	std::string RequireString(const json& p, const char* a_key)
	{
		const auto it = p.find(a_key);
		if (it == p.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) {
			throw RpcError("bad_request", std::string("missing string parameter '") + a_key + "'");
		}
		return it->get<std::string>();
	}

	std::string OptString(const json& p, const char* a_key, const std::string& a_default)
	{
		const auto it = p.find(a_key);
		if (it == p.end() || it->is_null()) {
			return a_default;
		}
		if (!it->is_string()) {
			throw RpcError("bad_request", std::string("parameter '") + a_key + "' must be a string");
		}
		return it->get<std::string>();
	}

	double OptNumber(const json& p, const char* a_key, double a_default)
	{
		const auto it = p.find(a_key);
		if (it == p.end() || it->is_null()) {
			return a_default;
		}
		if (!it->is_number()) {
			throw RpcError("bad_request", std::string("parameter '") + a_key + "' must be a number");
		}
		return it->get<double>();
	}

	bool OptBool(const json& p, const char* a_key, bool a_default)
	{
		const auto it = p.find(a_key);
		if (it == p.end() || it->is_null()) {
			return a_default;
		}
		if (!it->is_boolean()) {
			throw RpcError("bad_request", std::string("parameter '") + a_key + "' must be true or false");
		}
		return it->get<bool>();
	}
}
