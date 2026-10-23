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

#include <deque>

namespace Ui {
class IconButton;
class LayerWidget;
class RippleButton;
class FlatLabel;
} // namespace Ui

namespace Media::Video {
class FrameReader;
} // namespace Media::Video

namespace Editor {

class Crop;
class SegmentPlayer;
class TimelineSeeker;
class VideoTimeline;
class VideoQualitySlider;

struct VideoEditorDescriptor {
	QString path;
	QByteArray content;
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

	[[nodiscard]] rpl::producer<VideoModifications> doneRequests() const {
		return _done.events();
	}
	[[nodiscard]] rpl::producer<> cancelRequests() const {
		return _cancel.events();
	}

	[[nodiscard]] VideoModifications collect() const;

private:
	void setupStreaming();
	void setupControls();
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
	void invalidateCoverPreview();
	void togglePause();
	void paintPlayBadge(QPainter &p);
	void applyGeometry();
	void paint(QPainter &p);
	void keyPressEvent(QKeyEvent *e) override;
	void stepFrame(int direction);
	void confirm();

	const QString _path;
	const QByteArray _content;
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

	std::unique_ptr<SegmentPlayer> _player;
	base::unique_qptr<Crop> _crop;
	base::unique_qptr<VideoTimeline> _timeline;
	std::unique_ptr<TimelineSeeker> _seeker;
	base::unique_qptr<VideoQualitySlider> _quality;
	base::unique_qptr<Ui::RpWidget> _controls;
	base::unique_qptr<Ui::RpWidget> _bar;
	base::unique_qptr<Ui::IconButton> _rotate;
	base::unique_qptr<Ui::IconButton> _flip;
	base::unique_qptr<Ui::IconButton> _gifButton;
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

	QRect _frameRect;
	QTransform _frameMatrix;
	QRect _innerRect;
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
