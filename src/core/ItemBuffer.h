#pragma once
// Holds inventory-change events briefly so an add/remove pair of the same item cancels out.
// Scripts often add and remove an item within a moment to poke the engine: Nether's Follower
// Framework adds and removes nwsFF_silverRing to refresh a follower's 3D after load doors.
// Header-only, no game dependencies (unit-tested).

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace Core
{
	class ItemBuffer
	{
	public:
		using Clock = std::chrono::steady_clock;

		struct Change
		{
			std::string       agent;
			std::uint32_t     item{ 0 };
			std::int32_t      count{ 0 };
			bool              received{ false };
			nlohmann::json    payload;
			Clock::time_point at{};
		};

		explicit ItemBuffer(std::chrono::milliseconds a_hold = std::chrono::milliseconds{ 1500 }) :
			_hold(a_hold) {}

		// Returns false when a_change cancelled an opposite pending change (both are dropped).
		bool Add(Change a_change)
		{
			std::lock_guard lock{ _m };
			for (auto it = _q.begin(); it != _q.end(); ++it) {
				if (it->agent == a_change.agent && it->item == a_change.item && it->count == a_change.count && it->received != a_change.received) {
					_q.erase(it);
					return false;
				}
			}
			_q.push_back(std::move(a_change));
			return true;
		}

		// Changes that have been held long enough, oldest first.
		std::vector<Change> Ready(Clock::time_point a_now)
		{
			std::vector<Change> out;
			std::lock_guard     lock{ _m };
			while (!_q.empty() && a_now - _q.front().at >= _hold) {
				out.push_back(std::move(_q.front()));
				_q.pop_front();
			}
			return out;
		}

	private:
		std::mutex                _m;
		std::deque<Change>        _q;
		std::chrono::milliseconds _hold;
	};
}
