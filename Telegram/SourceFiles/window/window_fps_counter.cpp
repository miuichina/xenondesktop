#include "window/window_fps_counter.h"

#include "base/options.h"
#include "base/object_ptr.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "styles/style_window.h"

#include <QtWidgets/QApplication>

namespace Window {
namespace {

constexpr auto kRefreshInterval = crl::time(200);
constexpr auto kSecond = qint64(1'000'000'000);

base::options::toggle OptionShowFps({
	.id = kOptionShowFps,
	.name = "Show FPS",
});

} // namespace

void SetupFpsCounter(not_null<Ui::RpWidget*> parent) {
	const auto counter = std::make_shared<object_ptr<FpsCounter>>(nullptr);
	rpl::merge(
		rpl::single(rpl::empty_value()),
		OptionShowFps.changes()
	) | rpl::on_next([=] {
		if (OptionShowFps.value()) {
			if (!*counter) {
				*counter = object_ptr<FpsCounter>(parent);
			}
		} else {
			counter->destroy();
		}
	}, parent->lifetime());
}

FpsCounter::FpsCounter(not_null<Ui::RpWidget*> parent)
: Ui::RpWidget(parent)
, _window(parent->window())
, _refreshTimer([=] { refresh(); }) {
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setAttribute(Qt::WA_OpaquePaintEvent);
	_clock.start();
	parent->sizeValue() | rpl::on_next([=](QSize size) {
		const auto padding = st::fpsCounterPadding;
		resize(
			st::fpsCounterWidth,
			padding.top() + 2 * st::normalFont->height + padding.bottom());
		move(
			std::max(0, size.width() - width() - st::fpsCounterMargin),
			st::fpsCounterMargin);
	}, lifetime());
	style::PaletteChanged() | rpl::on_next([=] {
		update();
	}, lifetime());
	qApp->installEventFilter(this);
	_refreshTimer.callEach(kRefreshInterval);
	refresh();
	show();
	raise();
}

bool FpsCounter::eventFilter(QObject *watched, QEvent *event) {
	if (watched == _window && event->type() == QEvent::UpdateRequest) {
		if (!_framePending) {
			_framePending = true;
			_contentPainted = false;
			InvokeQueued(this, [=] {
				if (_contentPainted) {
					recordFrame();
				}
				_framePending = false;
				raise();
			});
		}
	} else if (_framePending
		&& event->type() == QEvent::Paint
		&& watched != this) {
		if (const auto widget = qobject_cast<QWidget*>(watched)
			; widget && widget->window() == _window) {
			_contentPainted = true;
		}
	}
	return Ui::RpWidget::eventFilter(watched, event);
}

void FpsCounter::recordFrame() {
	const auto now = _clock.nsecsElapsed();
	if (_lastFrame) {
		_frames.push_back({ now, now - _lastFrame });
	}
	_lastFrame = now;
	++_frameCount;
	while (!_frames.empty() && _frames.front().time <= now - kSecond) {
		_frames.pop_front();
	}
}

void FpsCounter::refresh() {
	const auto now = _clock.nsecsElapsed();
	while (!_frames.empty() && _frames.front().time <= now - kSecond) {
		_frames.pop_front();
	}
	const auto rate = [](qint64 interval) {
		return (interval > 0) ? double(kSecond) / interval : 0.;
	};
	const auto current = _frames.empty()
		? 0.
		: rate(std::max(_frames.back().interval, now - _lastFrame));
	const auto average = (now > 0)
		? double(_frameCount) * kSecond / now
		: 0.;
	auto minimum = current;
	auto maximum = current;
	for (const auto &frame : _frames) {
		const auto fps = rate(frame.interval);
		minimum = std::min(minimum, fps);
		maximum = std::max(maximum, fps);
	}
	const auto first = u"UI FPS %1   Avg %2"_q
		.arg(current, 0, 'f', 1)
		.arg(average, 0, 'f', 1);
	const auto second = u"1s Min %1   Max %2"_q
		.arg(minimum, 0, 'f', 1)
		.arg(maximum, 0, 'f', 1);
	if (_firstLine != first || _secondLine != second) {
		_firstLine = first;
		_secondLine = second;
		update();
	}
}

void FpsCounter::paintEvent(QPaintEvent *event) {
	Painter p(this);
	p.fillRect(rect(), st::windowBg);
	p.setPen(st::windowFg);
	p.setFont(st::normalFont);
	const auto padding = st::fpsCounterPadding;
	const auto top = padding.top() + st::normalFont->ascent;
	p.drawText(padding.left(), top, _firstLine);
	p.drawText(padding.left(), top + st::normalFont->height, _secondLine);
}

} // namespace Window
