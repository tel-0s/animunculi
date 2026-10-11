#include "EventLog.h"

#include <algorithm>
#include <cmath>

namespace Core
{
	std::uint64_t EventLog::Push(std::string a_type, nlohmann::json a_data)
	{
		if (!a_data.is_object()) {
			a_data = nlohmann::json{ { "value", std::move(a_data) } };
		}
		const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		double hours = -1.0;
		if (gameHours) {
			hours = gameHours();
		}
		std::uint64_t seq;
		{
			std::lock_guard lock{ _lock };
			seq = _nextSeq++;
			a_data["seq"] = seq;
			a_data["type"] = std::move(a_type);
			a_data["t"] = now;
			if (hours >= 0.0) {
				a_data["game_hours"] = std::round(hours * 100.0) / 100.0;
			}
			_events.push_back(std::move(a_data));
			while (_events.size() > _capacity) {
				_events.pop_front();
			}
		}
		_cv.notify_all();
		return seq;
	}

	EventLog::Result EventLog::QueryLocked(std::uint64_t a_since, const std::vector<std::string>& a_types, std::size_t a_limit) const
	{
		Result r;
		r.nextSeq = _nextSeq;
		if (!_events.empty()) {
			const auto oldest = _events.front()["seq"].get<std::uint64_t>();
			if (a_since < oldest && a_since > 0) {
				r.dropped = oldest - a_since;
			}
		}
		a_limit = std::max<std::size_t>(1, a_limit);
		for (const auto& ev : _events) {
			const auto seq = ev["seq"].get<std::uint64_t>();
			if (seq < a_since) {
				continue;
			}
			if (!a_types.empty()) {
				const auto& type = ev["type"].get_ref<const std::string&>();
				if (std::ranges::find(a_types, type) == a_types.end()) {
					continue;
				}
			}
			if (r.events.size() == a_limit) {
				r.nextSeq = seq;  // resume here next time
				return r;
			}
			r.events.push_back(ev);
		}
		return r;
	}

	EventLog::Result EventLog::Query(std::uint64_t a_since, const std::vector<std::string>& a_types, std::size_t a_limit) const
	{
		std::lock_guard lock{ _lock };
		return QueryLocked(a_since, a_types, a_limit);
	}

	EventLog::Result EventLog::Wait(std::uint64_t a_since, const std::vector<std::string>& a_types, std::size_t a_limit,
		std::chrono::milliseconds a_timeout)
	{
		std::unique_lock lock{ _lock };
		const auto       deadline = std::chrono::steady_clock::now() + a_timeout;
		for (;;) {
			auto r = QueryLocked(a_since, a_types, a_limit);
			if (!r.events.empty() || _shutdown) {
				return r;
			}
			if (_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
				return QueryLocked(a_since, a_types, a_limit);
			}
		}
	}

	std::uint64_t EventLog::NextSeq() const
	{
		std::lock_guard lock{ _lock };
		return _nextSeq;
	}

	void EventLog::Shutdown()
	{
		{
			std::lock_guard lock{ _lock };
			_shutdown = true;
		}
		_cv.notify_all();
	}
}
