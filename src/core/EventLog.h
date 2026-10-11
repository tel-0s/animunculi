#pragma once
// Thread-safe ring buffer of world events with sequence numbers and long-poll waiting.
// Pure C++ (nlohmann::json), unit-tested.

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace Core
{
	class EventLog
	{
	public:
		explicit EventLog(std::size_t a_capacity = 1024) :
			_capacity(a_capacity) {}

		// Adds `type`, `seq` and `t` (unix ms) to `a_data`, plus `game_hours` from the clock hook.
		std::uint64_t Push(std::string a_type, nlohmann::json a_data = nlohmann::json::object());

		struct Result
		{
			std::vector<nlohmann::json> events;
			std::uint64_t               nextSeq{ 0 };
			std::uint64_t               dropped{ 0 };  // events older than `since` that fell out of the buffer
		};

		// Events with seq >= a_since (optionally filtered by type). When a filter skips events,
		// nextSeq still advances past them so a caller never re-scans them.
		Result Query(std::uint64_t a_since, const std::vector<std::string>& a_types, std::size_t a_limit) const;

		// Blocks until Query would return something, the timeout passes, or Shutdown().
		Result Wait(std::uint64_t a_since, const std::vector<std::string>& a_types, std::size_t a_limit,
			std::chrono::milliseconds a_timeout);

		std::uint64_t NextSeq() const;
		void          Shutdown();

		std::function<double()> gameHours;  // optional; set by the game layer

	private:
		Result QueryLocked(std::uint64_t a_since, const std::vector<std::string>& a_types, std::size_t a_limit) const;

		mutable std::mutex          _lock;
		std::condition_variable     _cv;
		std::deque<nlohmann::json>  _events;
		std::uint64_t               _nextSeq{ 1 };
		std::size_t                 _capacity;
		bool                        _shutdown{ false };
	};
}
