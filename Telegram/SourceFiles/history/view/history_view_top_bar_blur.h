#pragma once

#include "ui/rp_widget.h"
#include "base/weak_ptr.h"

#include <QtGui/QImage>

class Painter;

namespace HistoryView {

struct TopBarBlurFrame {
	QImage backdrop;
	QImage foreground;
	QImage original;
	QColor tint;
	float radius = 0.f;
	float fade = 1.f;
	float backdropOpacity = 1.f;
	float cropTop = 0.f;
	float cropHeight = 1.f;
};

[[nodiscard]] bool UpdateBlurFrame(
	TopBarBlurFrame &cached,
	TopBarBlurFrame frame);

class BlurBackdropCache final {
public:
	void invalidate();
	[[nodiscard]] bool intersects(QRect area) const;
	void paint(
		Painter &p,
		QRect area,
		float64 scale,
		Fn<void(Painter&, QRect)> render);

private:
	QImage _image;
	QRect _area;
	float64 _scale = 0.;
	uint64 _revision = 0;

};

void UpdateBlurForeground(
	QImage &cached,
	QSize size,
	float64 ratio,
	Fn<void(Painter&)> render);

[[nodiscard]] float64 BlurTextureDownscale();
[[nodiscard]] QImage PrepareBlurImage(
	const QImage &image,
	QSize size,
	bool fullResolution = false);
[[nodiscard]] TopBarBlurFrame MakeBlurFrame(
	QImage backdrop,
	QImage foreground,
	QColor tint,
	int height,
	float tintOpacity = -1.f,
	float strength = 1.f);
void PaintBlurBackdrop(
	Painter &p,
	QRect area,
	QImage backdrop,
	QColor tint,
	float tintOpacity = -1.f,
	float strength = 1.f);

[[nodiscard]] std::unique_ptr<Ui::RpWidgetWrap> CreateTopBarBlur(
	QWidget *parent,
	Fn<TopBarBlurFrame()> frame,
	Fn<void()> failed);

class BlurBackground final : public base::has_weak_ptr {
public:
	BlurBackground(
		not_null<Ui::RpWidget*> target,
		Fn<QImage()> backdrop,
		Fn<QColor()> tint,
		float tintOpacity = -1.f,
		Fn<float()> strength = nullptr);
	~BlurBackground();
	void invalidate();

private:
	void paint();
	void createSurface();
	not_null<Ui::RpWidget*> _target;
	Fn<QImage()> _backdrop;
	Fn<QColor()> _tint;
	float _tintOpacity = -1.f;
	Fn<float()> _strength;
	QImage _image;
	QImage _original;
	QImage _raster;
	qint64 _surfaceImageKey = 0;
	QColor _surfaceTint;
	float64 _surfaceOpacity = -1.;
	float _surfaceFade = -1.f;
	QColor _rasterTint;
	float64 _rasterOpacity = -1.;
	std::unique_ptr<Ui::RpWidgetWrap> _surface;
	bool _failed = false;
	bool _surfacePending = false;
	rpl::lifetime _lifetime;

};

} // namespace HistoryView
