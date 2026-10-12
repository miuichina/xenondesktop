#include "history/view/history_view_blur_transition.h"

#include <cassert>

int main() {
	auto transition = HistoryView::BlurTransition();
	assert(transition.value(0) == 0.);
	transition.setShown(true, 1000);
	assert(transition.value(1000) == 0.);
	assert(std::abs(transition.value(1150) - std::sqrt(.75)) < 1e-12);
	assert(transition.animating(1299));
	assert(!transition.animating(1300));
	assert(transition.value(1300) == 1.);
	transition.setShown(false, 1400);
	assert(transition.value(1400) == 1.);
	assert(std::abs(transition.value(1550) - (1. - std::sqrt(.75))) < 1e-12);
	assert(transition.value(1700) == 0.);
	auto popup = HistoryView::BlurTransition(200);
	popup.setShown(true, 1000);
	assert(!popup.animating(1200));
	assert(popup.value(1200) == 1.);
	popup.setShown(false, 1200);
	assert(popup.value(1400) == 0.);
	for (auto elapsed = 0; elapsed <= 300; ++elapsed) {
		transition = HistoryView::BlurTransition();
		transition.setShown(true, 2000);
		const auto before = transition.value(2000 + elapsed);
		transition.setShown(false, 2000 + elapsed);
		assert(transition.value(2000 + elapsed) == before);
		transition.setShown(true, 2000 + elapsed + 10);
		const auto restart = transition.value(2000 + elapsed + 10);
		transition.setShown(true, 2000 + elapsed + 20);
		assert(transition.value(2000 + elapsed + 20) >= restart);
		assert(transition.value(2000 + elapsed + 320) == 1.);
		transition.setShown(false, 2000 + elapsed + 400);
		assert(transition.value(2000 + elapsed + 700) == 0.);
	}
}
