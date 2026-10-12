#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace HistoryView {

class BlurTransition final {
public:
	static constexpr auto kDuration = std::int64_t(300);

	explicit BlurTransition(std::int64_t duration = kDuration)
	: _duration(std::max(duration, std::int64_t(1))) {
	}

	void setShown(bool shown, std::int64_t now) {
		const auto target = shown ? 1. : 0.;
		if (_to == target) {
			return;
		}
		_from = value(now);
		_to = target;
		_started = now;
	}

	[[nodiscard]] double value(std::int64_t now) const {
		const auto t = std::clamp((now - _started) / double(_duration), 0., 1.);
		const auto remaining = 1. - t;
		return _from + (_to - _from)
			* std::sqrt(1. - remaining * remaining);
	}

	[[nodiscard]] bool animating(std::int64_t now) const {
		return _from != _to && now < _started + _duration;
	}

	[[nodiscard]] bool shown() const {
		return _to == 1.;
	}

private:
	std::int64_t _duration = kDuration;
	double _from = 0.;
	double _to = 0.;
	std::int64_t _started = 0;

};

} // namespace HistoryView
