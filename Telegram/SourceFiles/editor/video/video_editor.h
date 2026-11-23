/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "ui/effects/animations.h"
#include "editor/video/video_editor_common.h"
#include "ui/rp_widget.h"

#include <atomic>
#include <deque>

namespace Ui {
class IconButton;
class ImportantTooltip;
class LayerWidget;
class RippleButton;
class FlatLabel;
} // namespace Ui

namespace Media::Video {
class FrameReader;
} // namespace Media::Video

namespace Editor {

class CaptionBar;
class Crop;
class SegmentPlayer;
class TimelineSeeker;
class VideoTimeline;
class VideoQualitySlider;

struct VideoEditorDescriptor {
	QString path;
	QByteArray content;
	QImage preview;
	QSize dimensions;
	crl::time duration = 0;
	VideoEditorData data;

	VideoModifications initial;
	std::shared_ptr<Media::Video::FrameReader> frameReader;
};

class VideoEditor final
	: public Ui::RpWidget
	, public base::has_weak_ptr {
public:
	VideoEditor(
		not_null<QWidget*> parent,
		VideoEditorDescriptor descriptor);
	~VideoEditor();

	void startPlayback();

	[[nodiscard]] rpl::producer<VideoModifications> doneRequests() const {
		return _done.events();
	}
	[[nodiscard]] rpl::producer<> cancelRequests() const {
		return _cancel.events();
	}

	[[nodiscard]] VideoModifications collect() const;

	[[nodiscard]] TextWithTags caption() const;
	[[nodiscard]] int period() const;
	[[nodiscard]] bool edited() const;

private:
	void setupStreaming();
	void setupControls();
	void setupGifButton();
	void showGifTooltip();
	void hideGifTooltip();
	void setupTimeline();
	void setupQuality();
	void setupSizeEstimate();
	void refreshSizeEstimate();
	void refreshQualityLevels();
	void setupTapToPause();
	void updateBubble();
	void startCapture();
	[[nodiscard]] QRect captureFrom() const;
	[[nodiscard]] QSize bubbleSize() const;
	void refreshCoverPreview();
	void refreshFramePreview();
	[[nodiscard]] QSize framePreviewBox() const;
	void activateFramePreview(bool resetQuality);
	void updateFramePreviewQuality(crl::time elapsed, bool completed);
	void invalidateCoverPreview();
	void togglePause();
	void paintPlayBadge(QPainter &p);
	void paintCanvas(QPainter &p);
	void applyGeometry();
	void paint(QPainter &p);
	void keyPressEvent(QKeyEvent *e) override;
	void keyReleaseEvent(QKeyEvent *e) override;
	void stepFrame(int direction);
	void confirm();
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	bool eventHook(QEvent *e) override;

	void layoutCanvas(QRect contentRect);
	[[nodiscard]] QRectF canvasMediaRect() const;
	[[nodiscard]] QRectF canvasBasePlacement() const;
	[[nodiscard]] float64 canvasScale() const;
	[[nodiscard]] QSize frameRequestSize() const;
	void clampCanvasShift();
	void restoreCanvasPlacement(QRect rect);
	void zoomCanvas(float64 factor, QPoint about);
	void canvasChanged();

	const QString _path;
	const QByteArray _content;
	const QImage _preview;
	const QSize _dimensions;
	const crl::time _duration = 0;
	const VideoEditorData _data;

	const VideoModifications _initial;
	const std::shared_ptr<Media::Video::FrameReader> _frameReader;

	Media::Encode::SourceInfo _source;

	PhotoModifications _geometry;
	crl::time _from = 0;
	crl::time _till = 0;
	crl::time _cover = 0;
	bool _gif = false;
	bool _gifAllowed = true;

	std::unique_ptr<SegmentPlayer> _player;
	base::unique_qptr<Crop> _crop;
	base::unique_qptr<VideoTimeline> _timeline;
	std::unique_ptr<TimelineSeeker> _seeker;
	base::unique_qptr<VideoQualitySlider> _quality;
	base::unique_qptr<Ui::RpWidget> _controls;
	base::unique_qptr<CaptionBar> _captionBar;
	base::unique_qptr<Ui::RpWidget> _bar;
	base::unique_qptr<Ui::IconButton> _rotate;
	base::unique_qptr<Ui::IconButton> _flip;
	base::unique_qptr<Ui::IconButton> _gifButton;
	base::unique_qptr<Ui::ImportantTooltip> _gifTooltip;
	base::unique_qptr<Ui::RippleButton> _confirm;
	base::unique_qptr<Ui::RippleButton> _cancelButton;
	base::unique_qptr<Ui::FlatLabel> _about;
	base::unique_qptr<Ui::FlatLabel> _hint;
	base::unique_qptr<Ui::RpWidget> _bubble;
	base::unique_qptr<Ui::RpWidget> _capture;
	Ui::Animations::Simple _captureShown;
	QImage _bubblePreview;
	QImage _framePreview;
	QSize _framePreviewBox;
	crl::time _framePreviewAt = -1;
	bool _framePreviewBusy = false;
	crl::time _requestedFramePreviewAt = -1;
	QSize _requestedFramePreviewBox;
	std::shared_ptr<std::atomic<bool>> _framePreviewCancel;
	std::shared_ptr<std::atomic<bool>> _frameStepCancel;
	int _frameStepKey = 0;
	bool _framePreviewActive = false;
	bool _reducedFramePreview = false;
	int _fastFramePreviews = 0;
	base::Timer _framePreviewIdleTimer;

	QRect _frameRect;
	QTransform _frameMatrix;
	QRect _innerRect;
	QRect _contentRect;
	QRect _canvasRect;
	QRect _mediaRect;
	float64 _canvasZoom = 1.;
	QPointF _canvasShift;
	std::optional<QRect> _canvasRestored;
	QPointF _canvasDragShift;
	std::optional<QPoint> _canvasPress;
	bool _canvasMoved = false;
	bool _dragging = false;
	bool _userPaused = false;
	bool _steppingFrame = false;
	bool _confirmAfterFrameStep = false;
	std::deque<bool> _pendingFrameSteps;
	crl::time _bubbleCover = -1;
	bool _bubbleBusy = false;
	bool _bubbleVisible = false;
	bool _draggingHead = false;
	crl::time _bubbleShownAt = 0;
	base::Timer _bubbleHideTimer;
	Ui::Animations::Simple _bubbleShown;

	rpl::event_stream<VideoModifications> _done;
	rpl::event_stream<> _cancel;

};

void InitVideoEditorLayer(
	not_null<Ui::LayerWidget*> layer,
	not_null<VideoEditor*> editor,
	Fn<void(VideoModifications)> doneCallback);

} // namespace Editor
