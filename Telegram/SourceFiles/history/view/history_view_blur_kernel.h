#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace HistoryView {

struct BlurKernel {
	float center = 1.f;
	int count = 0;
	std::array<std::array<float, 4>, 32> taps = {};
};

[[nodiscard]] inline BlurKernel MakeBlurKernel(float radius) {
	radius = std::clamp(radius, 0.f, 64.f);
	const auto limit = int(std::ceil(radius));
	const auto sigma = std::max(double(radius) * .5, .15);
	auto result = BlurKernel();
	auto total = 1.;
	for (auto i = 1; i <= limit; i += 2) {
		const auto first = std::exp(-.5 * i * i / (sigma * sigma));
		const auto next = i + 1;
		const auto second = (next <= limit)
			? std::exp(-.5 * next * next / (sigma * sigma))
			: 0.;
		const auto weight = first + second;
		result.taps[result.count++] = {
			float(i + second / weight), float(weight), 0.f, 0.f };
		total += 2. * weight;
	}
	result.center = float(1. / total);
	for (auto &tap : result.taps) {
		tap[1] /= float(total);
	}
	result.taps[0][2] = result.center;
	return result;
}

} // namespace HistoryView
