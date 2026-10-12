#include "history/view/history_view_blur_kernel.h"

#include <cassert>
#include <cstdio>
#include <chrono>

int main() {
	for (auto step = 0; step <= 6400; ++step) {
		const auto radius = step / 100.f;
		const auto kernel = HistoryView::MakeBlurKernel(radius);
		assert(kernel.count <= 32);
		auto expanded = std::array<double, 65>();
		expanded[0] = kernel.center;
		for (auto i = 0; i < kernel.count; ++i) {
			const auto &tap = kernel.taps[i];
			const auto index = int(std::floor(tap[0]));
			const auto fraction = tap[0] - index;
			expanded[index] += tap[1] * (1. - fraction);
			if (index < 64) {
				expanded[index + 1] += tap[1] * fraction;
			}
		}
		const auto sigma = std::max(double(radius) * .5, .15);
		auto reference = std::array<double, 65>();
		reference[0] = 1.;
		auto total = 1.;
		for (auto i = 1; i <= int(std::ceil(radius)); ++i) {
			reference[i] = std::exp(-.5 * i * i / (sigma * sigma));
			total += 2. * reference[i];
		}
		for (auto i = 0; i <= 64; ++i) {
			assert(std::abs(expanded[i] - reference[i] / total) < 0.000001);
		}
	}
	const auto start = std::chrono::steady_clock::now();
	auto checksum = 0.;
	for (auto i = 0; i < 100000; ++i) {
		checksum += HistoryView::MakeBlurKernel((i % 6400) / 100.f).center;
	}
	const auto elapsed = std::chrono::duration<double, std::micro>(
		std::chrono::steady_clock::now() - start).count();
	std::printf("6401 Gaussian kernels match the dense reference. Mean kernel cost: %.2f us; checksum %.4f\n",
		elapsed / 100000., checksum);
}
