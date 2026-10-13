#pragma once

#include "ui/rp_widget.h"
#include "base/timer.h"

#include <QtCore/QElapsedTimer>
#include <deque>

namespace Window {

inline constexpr auto kOptionShowFps = "xenon-show-fps";

void SetupFpsCounter(not_null<Ui::RpWidget*> parent);

class FpsCounter final : public Ui::RpWidget {
public:
	explicit FpsCounter(not_null<Ui::RpWidget*> parent);

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void paintEvent(QPaintEvent *event) override;

private:
	struct Frame {
		qint64 time = 0;
		qint64 interval = 0;
	};

	void recordFrame();
	void refresh();

	const not_null<QWidget*> _window;
	QElapsedTimer _clock;
	base::Timer _refreshTimer;
	std::deque<Frame> _frames;
	qint64 _lastFrame = 0;
	uint64 _frameCount = 0;
	bool _framePending = false;
	bool _contentPainted = false;
	QString _firstLine;
	QString _secondLine;

};

} // namespace Window
