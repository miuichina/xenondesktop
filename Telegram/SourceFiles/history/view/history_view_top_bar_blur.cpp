#include "history/view/history_view_top_bar_blur.h"
#include "history/view/history_view_blur_kernel.h"
#include "history/view/history_view_top_bar_widget.h"

#include "base/options.h"
#include "window/main_window.h"
#include "crl/crl_on_main.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"
#include "ui/qt_object_factory.h"
#include "ui/ui_utility.h"

#include "ui/gl/gl_surface.h"
#include "ui/rhi/rhi_image.h"
#include "ui/rhi/rhi_renderer.h"
#include "ui/rhi/rhi_shader.h"
#include "styles/style_chat.h"

#ifdef Q_OS_WIN
#include "platform/win/main_window_win.h"
#endif

#include <QtCore/QVariant>

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
#include <rhi/qrhi.h>
#include <QRhiWidget>
#endif

namespace HistoryView {
namespace {

auto BlurSnapshotDepth = 0;

base::options::toggle OptionRealtimeBlur({
	.id = kOptionRealtimeBlur,
	.name = "Real-time blur",
});

class BlurCover final : public Ui::RpWidget {
public:
	using Ui::RpWidget::RpWidget;

};

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)

struct BlurUniforms {
	float step[2] = {};
	float flip = 0.f;
	float count = 0.f;
	std::array<std::array<float, 4>, 32> taps = {};
};

struct GlassUniforms {
	float tint[4] = {};
	float crop[2] = {};
	float flip = 0.f;
	float opacity = 1.f;
	float fade = 1.f;
	float padding[3] = {};
};

static_assert(sizeof(BlurUniforms) == 528);
static_assert(sizeof(GlassUniforms) == 48);

class BlurRenderer final : public Ui::GL::Renderer, public Ui::Rhi::Renderer {
public:
	BlurRenderer(
		Fn<TopBarBlurFrame()> frame,
		Fn<void()> failed,
		Fn<void()> ready,
		Fn<void()> reset)
	: _frame(std::move(frame))
	, _failed(std::move(failed))
	, _rendered(std::move(ready))
	, _reset(std::move(reset)) {
	}

	void render(QRhi *rhi, QRhiRenderTarget *rt, QRhiCommandBuffer *cb) override;
	void releaseResources() override;

private:
	struct Pass {
		std::unique_ptr<QRhiTexture> texture;
		std::unique_ptr<QRhiTextureRenderTarget> target;
		std::unique_ptr<QRhiRenderPassDescriptor> descriptor;
		std::unique_ptr<QRhiBuffer> uniforms;
		std::unique_ptr<QRhiShaderResourceBindings> bindings;
		std::unique_ptr<QRhiGraphicsPipeline> pipeline;

		void clear() {
			pipeline.reset();
			bindings.reset();
			uniforms.reset();
			target.reset();
			descriptor.reset();
			texture.reset();
		}
	};

	bool prepare(QRhi *rhi, QRhiRenderTarget *rt, QSize size);
	bool preparePass(Pass &pass, bool glass, QRhiRenderTarget *rt);
	void draw(QRhiCommandBuffer *cb, Pass &pass, QRhiRenderTarget *rt);

	Fn<TopBarBlurFrame()> _frame;
	Fn<void()> _failed;
	Fn<void()> _rendered;
	Fn<void()> _reset;
	QRhi *_rhi = nullptr;
	QRhiRenderPassDescriptor *_screenDescriptor = nullptr;
	int _sampleCount = 0;
	QSize _size;
	Ui::Rhi::Image _backdrop;
	Ui::Rhi::Image _foreground;
	Ui::Rhi::Image _original;
	std::unique_ptr<QRhiBuffer> _vertices;
	std::unique_ptr<QRhiSampler> _sampler;
	Pass _horizontal;
	Pass _vertical;
	Pass _glass;
	qint64 _backdropKey = 0;
	float _radius = -1.f;
	float _kernelRadius = -1.f;
	BlurKernel _kernel;
	bool _ready = false;

};

bool BlurRenderer::preparePass(Pass &pass, bool glass, QRhiRenderTarget *rt) {
	if (!glass) {
		pass.texture.reset(_rhi->newTexture(
			QRhiTexture::RGBA8, _size, 1, QRhiTexture::RenderTarget));
		if (!pass.texture->create()) {
			return false;
		}
		pass.target.reset(_rhi->newTextureRenderTarget(
			QRhiTextureRenderTargetDescription(
				QRhiColorAttachment(pass.texture.get()))));
		pass.descriptor.reset(pass.target->newCompatibleRenderPassDescriptor());
		pass.target->setRenderPassDescriptor(pass.descriptor.get());
		if (!pass.target->create()) {
			return false;
		}
		rt = pass.target.get();
	}
	pass.uniforms.reset(_rhi->newBuffer(
		QRhiBuffer::Dynamic,
		QRhiBuffer::UniformBuffer,
		glass ? sizeof(GlassUniforms) : sizeof(BlurUniforms)));
	if (!pass.uniforms->create()) {
		return false;
	}
	pass.bindings.reset(_rhi->newShaderResourceBindings());
	const auto uniform = QRhiShaderResourceBinding::uniformBuffer(
		0, QRhiShaderResourceBinding::FragmentStage, pass.uniforms.get());
	const auto texture = QRhiShaderResourceBinding::sampledTexture(
		1,
		QRhiShaderResourceBinding::FragmentStage,
		glass ? _vertical.texture.get()
			: (&pass == &_horizontal)
			? _backdrop.texture()
			: _horizontal.texture.get(),
		_sampler.get());
	if (glass) {
		pass.bindings->setBindings({
			uniform,
			texture,
			QRhiShaderResourceBinding::sampledTexture(
				2,
				QRhiShaderResourceBinding::FragmentStage,
				_foreground.texture(),
				_sampler.get()),
			QRhiShaderResourceBinding::sampledTexture(
				3,
				QRhiShaderResourceBinding::FragmentStage,
				_original.image().isNull()
					? _backdrop.texture()
					: _original.texture(),
				_sampler.get()),
		});
	} else {
		pass.bindings->setBindings({ uniform, texture });
	}
	if (!pass.bindings->create()) {
		return false;
	}
	QRhiVertexInputLayout layout;
	layout.setBindings({ { 4 * sizeof(float) } });
	layout.setAttributes({
		{ 0, 0, QRhiVertexInputAttribute::Float2, 0 },
		{ 0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float) },
	});
	pass.pipeline.reset(_rhi->newGraphicsPipeline());
	pass.pipeline->setShaderStages({
		{ QRhiShaderStage::Vertex, Ui::Rhi::ShaderFromFile(
			u":/shaders/passthrough.vert.qsb"_q) },
		{ QRhiShaderStage::Fragment, Ui::Rhi::ShaderFromFile(glass
			? u":/shaders/top_bar_glass.frag.qsb"_q
			: u":/shaders/top_bar_blur.frag.qsb"_q) },
	});
	pass.pipeline->setVertexInputLayout(layout);
	pass.pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
	pass.pipeline->setSampleCount(rt->sampleCount());
	pass.pipeline->setShaderResourceBindings(pass.bindings.get());
	pass.pipeline->setRenderPassDescriptor(rt->renderPassDescriptor());
	return pass.pipeline->create();
}

bool BlurRenderer::prepare(QRhi *rhi, QRhiRenderTarget *rt, QSize size) {
	if (_ready && _rhi == rhi && _size == size
		&& _screenDescriptor == rt->renderPassDescriptor()
		&& _sampleCount == rt->sampleCount()) {
		return true;
	}
	_glass.clear();
	_vertical.clear();
	_horizontal.clear();
	_ready = false;
	_rhi = rhi;
	_size = size;
	_screenDescriptor = rt->renderPassDescriptor();
	_sampleCount = rt->sampleCount();
	_vertices.reset(rhi->newBuffer(
		QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, 16 * sizeof(float)));
	_sampler.reset(rhi->newSampler(
		QRhiSampler::Linear,
		QRhiSampler::Linear,
		QRhiSampler::None,
		QRhiSampler::ClampToEdge,
		QRhiSampler::ClampToEdge));
	_ready = _vertices->create()
		&& _sampler->create()
		&& preparePass(_horizontal, false, rt)
		&& preparePass(_vertical, false, rt)
		&& preparePass(_glass, true, rt);
	_backdropKey = 0;
	return _ready;
}

void BlurRenderer::draw(
		QRhiCommandBuffer *cb,
		Pass &pass,
		QRhiRenderTarget *rt) {
	cb->setGraphicsPipeline(pass.pipeline.get());
	const auto size = rt->pixelSize();
	cb->setViewport(QRhiViewport(0, 0, size.width(), size.height()));
	cb->setShaderResources(pass.bindings.get());
	const auto binding = QRhiCommandBuffer::VertexInput(_vertices.get(), 0);
	cb->setVertexInput(0, 1, &binding);
	cb->draw(4);
}

void BlurRenderer::render(
		QRhi *rhi,
		QRhiRenderTarget *rt,
		QRhiCommandBuffer *cb) {
	const auto frame = _frame();
	if (frame.backdrop.isNull() || frame.foreground.isNull()) {
		_reset();
		cb->beginPass(rt, Qt::transparent, { 1.f, 0 });
		cb->endPass();
		return;
	}
	if (_rhi && _rhi != rhi) {
		releaseResources();
	}
	const auto foregroundSize = _foreground.image().size();
	const auto originalSize = _original.image().size();
	_backdrop.setImage(frame.backdrop);
	_foreground.setImage(frame.foreground);
	_original.setImage(frame.original);
	if (foregroundSize != frame.foreground.size()
		|| originalSize != frame.original.size()) {
		_ready = false;
	}
	const auto updates = rhi->nextResourceUpdateBatch();
	_backdrop.upload(rhi, updates);
	_foreground.upload(rhi, updates);
	_original.upload(rhi, updates);
	if (!prepare(rhi, rt, frame.backdrop.size())) {
		updates->release();
		cb->beginPass(rt, Qt::transparent, { 1.f, 0 });
		cb->endPass();
		LOG(("Blur: GPU resource creation failed; switching to CPU fallback."));
		_failed();
		return;
	}
	const auto flip = rhi->isYUpInNDC() ? 1.f : -1.f;
	const auto vertices = std::array<float, 16>{
		-1.f, flip, 0.f, 0.f,
		1.f, flip, 1.f, 0.f,
		-1.f, -flip, 0.f, 1.f,
		1.f, -flip, 1.f, 1.f,
	};
	updates->updateDynamicBuffer(_vertices.get(), 0, sizeof(vertices), vertices.data());
	const auto textureFlip = rhi->isYUpInFramebuffer() ? 1.f : 0.f;
	if (_kernelRadius != frame.radius) {
		_kernel = MakeBlurKernel(frame.radius);
		_kernelRadius = frame.radius;
	}
	const auto horizontal = BlurUniforms{
		{ 1.f / _size.width(), 0.f }, 0.f, float(_kernel.count), _kernel.taps };
	const auto vertical = BlurUniforms{
		{ 0.f, 1.f / _size.height() }, textureFlip, float(_kernel.count), _kernel.taps };
	updates->updateDynamicBuffer(
		_horizontal.uniforms.get(), 0, sizeof(horizontal), &horizontal);
	updates->updateDynamicBuffer(
		_vertical.uniforms.get(), 0, sizeof(vertical), &vertical);
	const auto glass = GlassUniforms{
		{ float(frame.tint.redF()), float(frame.tint.greenF()),
			float(frame.tint.blueF()), float(frame.tint.alphaF()) },
		{ frame.cropTop, frame.cropHeight },
		textureFlip,
		frame.backdropOpacity,
		frame.fade,
	};
	updates->updateDynamicBuffer(_glass.uniforms.get(), 0, sizeof(glass), &glass);
	const auto stale = (_backdropKey != frame.backdrop.cacheKey())
		|| (_radius != frame.radius);
	if (stale) {
		cb->beginPass(_horizontal.target.get(), Qt::transparent, { 1.f, 0 }, updates);
		draw(cb, _horizontal, _horizontal.target.get());
		cb->endPass();
		cb->beginPass(_vertical.target.get(), Qt::transparent, { 1.f, 0 });
		draw(cb, _vertical, _vertical.target.get());
		cb->endPass();
		_backdropKey = frame.backdrop.cacheKey();
		_radius = frame.radius;
	} else {
		cb->resourceUpdate(updates);
	}
	cb->beginPass(rt, Qt::transparent, { 1.f, 0 });
	draw(cb, _glass, rt);
	cb->endPass();
	if (_rendered) {
		_rendered();
	}
}

void BlurRenderer::releaseResources() {
	_glass.clear();
	_vertical.clear();
	_horizontal.clear();
	_vertices.reset();
	_sampler.reset();
	_backdrop.destroy();
	_foreground.destroy();
	_original.destroy();
	_rhi = nullptr;
	_screenDescriptor = nullptr;
	_backdropKey = 0;
	_radius = -1.f;
	_ready = false;
	if (_reset) {
		_reset();
	}
}

#endif

} // namespace

BlurSnapshot::BlurSnapshot(not_null<QWidget*> source) {
	++BlurSnapshotDepth;
	for (const auto child : source->findChildren<QWidget*>()) {
		const auto cover = dynamic_cast<BlurCover*>(child);
		if (cover && cover->parentWidget()->isVisibleTo(source)) {
			const auto surface = cover->parentWidget();
			_surfaces.push_back(surface);
			surface->hide();
		}
	}
}

BlurSnapshot::~BlurSnapshot() {
	--BlurSnapshotDepth;
	for (const auto &surface : _surfaces) {
		if (surface) {
			surface->show();
		}
	}
}

bool IsBlurSnapshotActive() {
	return BlurSnapshotDepth != 0;
}

QPixmap GrabBlurSnapshot(not_null<QWidget*> source, QRect rect) {
	const auto snapshot = BlurSnapshot(source);
	return Ui::GrabWidget(source, rect);
}

bool UpdateBlurFrame(TopBarBlurFrame &cached, TopBarBlurFrame frame) {
	const auto changed = cached.backdrop.cacheKey() != frame.backdrop.cacheKey()
		|| cached.foreground.cacheKey() != frame.foreground.cacheKey()
		|| cached.original.cacheKey() != frame.original.cacheKey()
		|| cached.tint != frame.tint
		|| cached.radius != frame.radius
		|| cached.fade != frame.fade
		|| cached.backdropOpacity != frame.backdropOpacity
		|| cached.cropTop != frame.cropTop
		|| cached.cropHeight != frame.cropHeight;
	cached = std::move(frame);
	return changed;
}

void BlurBackdropCache::invalidate() {
	++_revision;
	_image = QImage();
}

bool BlurBackdropCache::intersects(QRect area) const {
	return !_image.isNull() && _area.intersects(area);
}

void BlurBackdropCache::paint(
		Painter &p,
		QRect area,
		float64 scale,
		Fn<void(Painter&, QRect)> render) {
	if (area.isEmpty()) {
		return;
	}
	auto image = _image;
	auto bounds = _area;
	if (image.isNull() || _scale != scale
		|| bounds.width() != area.width() || !bounds.contains(area)) {
		const auto revision = _revision;
		bounds = area.adjusted(0, -area.height(), 0, area.height());
		const auto textureSize = QSize(
			std::max(int(std::ceil(bounds.width() * scale)), 1),
			std::max(int(std::ceil(bounds.height() * scale)), 1));
		const auto ratio = scale * BlurTextureDownscale();
		image = QImage(
			QSize(
				std::max(int(std::ceil(bounds.width() * ratio)), 1),
				std::max(int(std::ceil(bounds.height() * ratio)), 1)),
			QImage::Format_ARGB32_Premultiplied);
		image.setDevicePixelRatio(ratio);
		image.fill(Qt::transparent);
		{
			Painter cached(&image);
			cached.translate(-bounds.topLeft());
			cached.setClipRect(bounds);
			render(cached, bounds);
		}
		image = image.scaled(
			textureSize,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
		image.setDevicePixelRatio(1.);
		if (_revision == revision) {
			_scale = scale;
			_area = bounds;
			_image = image;
		}
	}
	p.save();
	p.setClipRect(area, Qt::IntersectClip);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.drawImage(QRectF(bounds.topLeft(), QSizeF(image.size()) / scale), image);
	p.restore();
}

void UpdateBlurForeground(
		QImage &cached,
		QSize size,
		float64 ratio,
		Fn<void(Painter&)> render) {
	auto image = QImage(size * ratio, QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(ratio);
	image.fill(Qt::transparent);
	{
		Painter p(&image);
		render(p);
	}
	if (image != cached || cached.devicePixelRatioF() != ratio) {
		cached = std::move(image);
	}
}

float64 BlurTextureDownscale() {
	const auto intensity = (std::clamp(
		base::options::value<int>(kOptionBlurIntensity), 1, 100) - 1) / 99.;
	return 1. + 3. * intensity * intensity;
}

QImage PrepareBlurImage(const QImage &source, QSize size, bool fullResolution) {
	if (source.isNull() || size.isEmpty()) {
		return QImage();
	}
	const auto padding = st::topBarBlurRadius;
	const auto scale = source.devicePixelRatioF() / (fullResolution ? 1. : BlurTextureDownscale());
	const auto bandHeight = size.height() + 2 * padding;
	auto result = QImage(
		QSize(
			std::max(int(std::ceil(size.width() * scale)), 1),
			std::max(int(std::ceil(bandHeight * scale)), 1)),
		QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	Painter p(&result);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.scale(result.width() / float64(size.width()), result.height() / float64(bandHeight));
	p.drawImage(QRect(0, padding, size.width(), size.height()), source);
	p.drawImage(QRect(0, 0, size.width(), padding), source,
		QRect(0, 0, source.width(), 1));
	p.drawImage(QRect(0, padding + size.height(), size.width(), padding), source,
		QRect(0, source.height() - 1, source.width(), 1));
	return result;
}

TopBarBlurFrame MakeBlurFrame(
		QImage backdrop,
		QImage foreground,
		QColor tint,
		int height,
		float tintOpacity,
		float strength) {
	const auto padding = st::topBarBlurRadius;
	const auto bandHeight = height + 2 * padding;
	const auto radius = float(padding * backdrop.height()
		/ float64(bandHeight)
		* std::clamp(base::options::value<int>(kOptionBlurIntensity), 1, 200)
		/ 100. * strength);
	tint.setAlphaF(tintOpacity >= 0.f ? tintOpacity : st::topBarBlurTintOpacity);
	auto opacity = 1.f;
#ifdef Q_OS_WIN
	opacity = float(Platform::WindowsBackdropPaintOpacity());
#endif
	return {
		.backdrop = std::move(backdrop),
		.foreground = std::move(foreground),
		.tint = tint,
		.radius = radius,
		.backdropOpacity = opacity,
		.cropTop = padding / float(bandHeight),
		.cropHeight = height / float(bandHeight),
	};
}

void PaintBlurBackdrop(
		Painter &p,
		QRect area,
		QImage backdrop,
		QColor tint,
		float tintOpacity,
		float strength) {
	if (backdrop.isNull() || area.isEmpty()) {
		return;
	}
	const auto padding = st::topBarBlurRadius;
	const auto factor = backdrop.height() / float64(area.height() + 2 * padding);
	const auto radius = std::max(1, int(base::SafeRound(padding * factor
		* std::clamp(base::options::value<int>(kOptionBlurIntensity), 1, 200)
		/ 100. * strength)));
	if (strength > 0.f) {
		backdrop = Images::BlurLargeImage(std::move(backdrop), radius);
	}
	p.save();
#ifdef Q_OS_WIN
	p.setOpacity(p.opacity() * Platform::WindowsBackdropPaintOpacity());
#endif
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.drawImage(area, backdrop, QRectF(
		0, padding * factor, backdrop.width(), area.height() * factor));
	tint.setAlphaF(tintOpacity >= 0.f ? tintOpacity : st::topBarBlurTintOpacity);
	p.fillRect(area, tint);
	p.restore();
}

std::unique_ptr<Ui::RpWidgetWrap> CreateTopBarBlur(
		QWidget *parent,
		Fn<TopBarBlurFrame()> frame,
		Fn<void()> failed) {
#ifdef Q_OS_WIN
	if (parent && parent->window()->testAttribute(Qt::WA_TranslucentBackground)) {
		return nullptr;
	}
#endif // Q_OS_WIN
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	if (Ui::GL::WidgetsRhiSupported()) {
		struct StartupState {
			QPointer<Ui::RpWidget> cover;
			TopBarBlurFrame frame;
			QImage raster;
			bool rendered = false;
		};
		const auto state = std::make_shared<StartupState>();
		auto surface = Ui::GL::CreateSurfaceRhi(
			parent,
			std::make_unique<BlurRenderer>(frame, failed, [=] {
				state->rendered = true;
			}, [=] {
				state->rendered = false;
				if (const auto cover = state->cover.data()) {
					crl::on_main(cover, [=] {
						if (!state->rendered) {
							cover->show();
							cover->update();
						}
					});
				}
			}));
		const auto widget = surface->rpWidget();
		widget->setAttribute(Qt::WA_OpaquePaintEvent, false);
		const auto cover = Ui::CreateChild<BlurCover>(widget);
		state->cover = cover;
		cover->setAttribute(Qt::WA_TransparentForMouseEvents);
		cover->setFocusPolicy(Qt::NoFocus);
		surface->sizeValue() | rpl::on_next([=] {
			state->rendered = false;
			cover->setGeometry(widget->rect());
			cover->show();
		}, cover->lifetime());
		surface->events() | rpl::on_next([=](not_null<QEvent*> event) {
			if (event->type() == QEvent::Show
				|| event->type() == QEvent::ScreenChangeInternal) {
				state->rendered = false;
				cover->show();
				cover->update();
			}
		}, cover->lifetime());
		cover->paintRequest() | rpl::on_next([=] {
			const auto changed = UpdateBlurFrame(state->frame, frame());
			const auto &current = state->frame;
			const auto ratio = cover->devicePixelRatioF();
			const auto size = cover->size() * ratio;
			if (changed || state->raster.size() != size
				|| state->raster.devicePixelRatioF() != ratio) {
				state->raster = QImage(size, QImage::Format_ARGB32_Premultiplied);
				state->raster.setDevicePixelRatio(ratio);
				state->raster.fill(Qt::transparent);
				Painter p(&state->raster);
				p.setRenderHint(QPainter::SmoothPixmapTransform);
				const auto draw = [&](const QImage &image) {
					p.drawImage(cover->rect(), image, QRectF(
						0.,
						current.cropTop * image.height(),
						image.width(),
						current.cropHeight * image.height()));
				};
				p.setOpacity(current.backdropOpacity);
				if (!current.original.isNull()) {
					draw(current.original);
				}
				auto blurred = current.backdrop;
				if (current.radius > 0.f && !blurred.isNull()) {
					blurred = Images::BlurLargeImage(
						std::move(blurred),
						std::max(1, int(base::SafeRound(current.radius))));
				}
				p.setOpacity(current.backdropOpacity * current.fade);
				draw(blurred);
				p.fillRect(cover->rect(), current.tint);
				p.setOpacity(1.);
				p.drawImage(cover->rect(), current.foreground);
			}
			Painter p(cover);
			p.drawImage(QPoint(), state->raster);
		}, cover->lifetime());
		cover->show();
		QObject::connect(
			static_cast<QRhiWidget*>(widget),
			&QRhiWidget::renderFailed,
			cover,
			[=] { failed(); });
		QObject::connect(
			static_cast<QRhiWidget*>(widget),
			&QRhiWidget::frameSubmitted,
			cover,
			[=] {
				if (state->rendered && !cover->isHidden()) {
					crl::on_main(cover, [=] {
						if (state->rendered) {
							cover->hide();
							state->raster = QImage();
							state->frame = TopBarBlurFrame();
						}
					});
				}
			});
		return surface;
	}
#endif
	[[maybe_unused]] static const auto logged = [] {
		LOG(("Blur: CPU fallback, Qt RHI unavailable or disabled."));
		return true;
	}();
	return nullptr;
}

BlurBackground::BlurBackground(
		not_null<Ui::RpWidget*> target,
		Fn<QImage()> backdrop,
		Fn<QColor()> tint,
		float tintOpacity,
		Fn<float()> strength)
: _target(target)
, _backdrop(std::move(backdrop))
, _tint(std::move(tint))
, _tintOpacity(tintOpacity)
, _strength(std::move(strength)) {
	_target->paintRequest() | rpl::on_next([=] { paint(); }, _lifetime);
	_target->sizeValue() | rpl::on_next([=] { invalidate(); }, _lifetime);
	rpl::merge(
		base::options::lookup<bool>(kOptionBlurEffects).changes(),
		base::options::lookup<int>(kOptionBlurIntensity).changes()
	) | rpl::on_next([=] { invalidate(); }, _lifetime);
}

BlurBackground::~BlurBackground() = default;

void BlurBackground::invalidate() {
	_image = QImage();
	_original = QImage();
	_raster = QImage();
	_target->update();
}

void BlurBackground::paint() {
	const auto enabled = base::options::value<bool>(kOptionBlurEffects);
	if (enabled && _image.isNull()) {
		_image = _backdrop();
		if (_strength && !_image.isNull()) {
			_original = _image;
			const auto downscale = BlurTextureDownscale();
			_image = _original.scaled(
				QSize(
					std::max(int(std::ceil(_original.width() / downscale)), 1),
					std::max(int(std::ceil(_original.height() / downscale)), 1)),
				Qt::IgnoreAspectRatio,
				Qt::SmoothTransformation);
		}
	}
	const auto shown = enabled && !_image.isNull();
	if (_strength) {
		const auto translucent = _target->window()->property("xenonWindowsBackdrop").toBool();
		_target->setAttribute(Qt::WA_OpaquePaintEvent, shown && !translucent);
	}
	if (shown && !_surface && !_failed && !_surfacePending
		&& !IsBlurSnapshotActive()) {
		_surfacePending = true;
		crl::on_main(this, [=] {
			_surfacePending = false;
			if (base::options::value<bool>(kOptionBlurEffects)) {
				createSurface();
				_target->update();
			}
		});
	}
	if (_surface && !IsBlurSnapshotActive()) {
		_surface->rpWidget()->setGeometry(_target->rect());
		_surface->rpWidget()->setVisible(shown);
		const auto tint = _tint();
		auto opacity = 1.;
#ifdef Q_OS_WIN
		opacity = Platform::WindowsBackdropPaintOpacity();
#endif
		const auto fade = _strength ? _strength() : 1.f;
		if (shown && (_surfaceImageKey != _image.cacheKey()
			|| _surfaceTint != tint || _surfaceOpacity != opacity
			|| _surfaceFade != fade)) {
			_surfaceImageKey = _image.cacheKey();
			_surfaceTint = tint;
			_surfaceOpacity = opacity;
			_surfaceFade = fade;
			_surface->rpWidget()->update();
		}
	} else if (shown) {
		const auto tint = _tint();
		const auto ratio = _target->devicePixelRatioF();
		auto opacity = 1.;
#ifdef Q_OS_WIN
		opacity = Platform::WindowsBackdropPaintOpacity();
#endif
		const auto strength = _strength ? _strength() : 1.f;
		if (_raster.isNull() || _rasterTint != tint
			|| _rasterOpacity != opacity || _rasterStrength != strength
			|| _raster.devicePixelRatioF() != ratio) {
			_rasterStrength = strength;
			_rasterTint = tint;
			_rasterOpacity = opacity;
			_raster = QImage(
				_target->size() * ratio,
				QImage::Format_ARGB32_Premultiplied);
			_raster.setDevicePixelRatio(ratio);
			_raster.fill(Qt::transparent);
			Painter p(&_raster);
			PaintBlurBackdrop(
				p,
				_target->rect(),
				_image,
				tint,
				_tintOpacity,
				strength);
		}
		Painter p(_target);
		if (_strength) {
			PaintBlurBackdrop(
				p,
				_target->rect(),
				_original,
				Qt::transparent,
				0.f,
				0.f);
		}
		p.setOpacity(_strength ? _strength() : 1.f);
		p.drawImage(QPoint(), _raster);
	}
}

void BlurBackground::createSurface() {
	_surface = CreateTopBarBlur(_target, [=] {
		static const auto foreground = [] {
			auto image = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
			image.fill(Qt::transparent);
			return image;
		}();
		const auto strength = _strength ? _strength() : 1.f;
		auto frame = MakeBlurFrame(
			_image,
			foreground,
			_tint(),
			_target->height(),
			_tintOpacity,
			strength);
		frame.original = _original;
		frame.fade = strength;
		return frame;
	}, [=] {
		crl::on_main(this, [=] {
			_failed = true;
			_surface.reset();
			_target->update();
		});
	});
	_failed = !_surface;
	_raster = QImage();
	if (_surface) {
		const auto widget = _surface->rpWidget();
		widget->setAttribute(Qt::WA_TransparentForMouseEvents);
		widget->setFocusPolicy(Qt::NoFocus);
		widget->setAttribute(Qt::WA_OpaquePaintEvent, false);
		widget->lower();
	}
}

} // namespace HistoryView
