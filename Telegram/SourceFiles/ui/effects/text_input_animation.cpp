#include "ui/effects/text_input_animation.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "lang/lang_keys.h"
#include "ui/widgets/fields/input_field.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTextBoundaryFinder>
#include <QtCore/QTimer>
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtGui/QPainter>
#include <QtGui/QTextBlock>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLineEdit>
#pragma push_macro("slots")
#define slots Q_SLOTS
#include <QtWidgets/private/qlineedit_p.h>
#include <QtWidgets/private/qwidgetlinecontrol_p.h>
#pragma pop_macro("slots")
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTextEdit>
#include <array>
#include <cmath>
#include <deque>
#include <numbers>

namespace Ui::TextInputAnimation {
namespace {

constexpr auto kMaxParticles = 384;
constexpr auto kMaxAnimatedEdit = 32;
constexpr auto kHiddenGlyphFormat = QTextFormat::UserProperty + 0x5845;

rpl::event_stream<> Changed;

std::string Key(const char *name) {
	return std::string("xenon-text-animation-") + name;
}

int Read(const char *key, int initial = 0) {
	return Core::App().settings().readPref<int>(Key(key), initial);
}

void Write(const char *key, int value) {
	Core::App().settings().writePref<int>(Key(key), value);
	Core::App().saveSettingsDelayed();
	Changed.fire({});
}

std::string PhaseKey(Mode mode, Phase phase) {
	return Key(("phase-" + std::to_string(int(mode))
		+ "-" + std::to_string(int(phase))).c_str());
}

double Random() {
	return QRandomGenerator::global()->generateDouble();
}

double Ease(double t) {
	t = std::clamp(t, 0., 1.);
	return t * t * (3. - 2. * t);
}

QImage BlurGlyph(const QImage &source, int radius) {
	const auto padding = radius * 3;
	auto image = QImage(source.size() + QSize(padding * 2, padding * 2),
		QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	{
		auto p = QPainter(&image);
		auto copy = source;
		copy.setDevicePixelRatio(1.);
		p.drawImage(QPoint(padding, padding), copy);
	}
	for (auto pass = 0; pass < 3; ++pass) {
		for (const auto horizontal : { true, false }) {
			auto output = QImage(image.size(), image.format());
			const auto input = reinterpret_cast<const QRgb*>(image.constBits());
			const auto destination = reinterpret_cast<QRgb*>(output.bits());
			const auto stride = image.bytesPerLine() / sizeof(QRgb);
			const auto lines = horizontal ? image.height() : image.width();
			const auto length = horizontal ? image.width() : image.height();
			const auto window = radius * 2 + 1;
			for (auto line = 0; line < lines; ++line) {
				const auto start = horizontal ? line * stride : line;
				const auto step = horizontal ? 1 : stride;
				const auto read = [&](int index) {
					return index < 0 || index >= length ? QRgb(0)
						: input[start + index * step];
				};
				auto red = 0;
				auto green = 0;
				auto blue = 0;
				auto alpha = 0;
				const auto accumulate = [&](QRgb pixel, int sign) {
					red += qRed(pixel) * sign;
					green += qGreen(pixel) * sign;
					blue += qBlue(pixel) * sign;
					alpha += qAlpha(pixel) * sign;
				};
				for (auto i = 0; i <= radius; ++i) accumulate(read(i), 1);
				for (auto i = 0; i < length; ++i) {
					destination[start + i * step] = qRgba(
						red / window, green / window, blue / window, alpha / window);
					accumulate(read(i - radius), -1);
					accumulate(read(i + radius + 1), 1);
				}
			}
			image = std::move(output);
		}
	}
	image.setDevicePixelRatio(source.devicePixelRatio());
	return image;
}

struct Glyph {
	QImage image;
	QRectF rect;
	int start = 0;
	int end = 0;
};

struct GlyphRaster {
	QImage source;
	QHash<int, QImage> blurred;
};

struct Particle {
	QImage image;
	std::shared_ptr<GlyphRaster> raster;
	QPointF position;
	QPointF velocity;
	QPointF acceleration;
	QPointF displacement;
	double angle = 0.;
	double spin = 0.;
	double rotationStrength = 0.;
	double gravity = 0.;
	double direction = 0.;
	double bounce = 0.;
	double blur = 0.;
	int fade = 0;
	int blurDuration = 0;
	int slideDuration = 0;
	int duration = 0;
	qint64 born = 0;
	qint64 impact = 0;
	bool appearance = false;
	bool fall = false;
	bool ignoreFloor = false;
	bool released = false;
	Mode mode = Mode::Blur;
	QTextCursor cursor;
	int from = 0;
	int till = 0;
};

class InputAnimation final : public QObject {
public:
	explicit InputAnimation(not_null<InputField*> field);
	InputAnimation(QWidget *field, QTextEdit *edit, QLineEdit *line = nullptr);
	~InputAnimation();

private:
	bool eventFilter(QObject *object, QEvent *event) override;
	bool editorAlive() const;
	QWidget *editor() const;
	QWidget *viewport() const;
	QString plainText() const;
	std::pair<int, int> selection() const;
	QRectF positionRect(int position) const;
	void setCursorWidth(int width);
	void setLineFormats(const QList<QTextLayout::FormatRange> &formats);
	void refresh();
	void clear();
	void frame();
	void paint(QPainter &p);
	void selections();
	void finishAppearance();
	int value(Parameter parameter) const;
	bool disabled(Mode mode, Phase phase) const;
	std::vector<Glyph> capture(int start, int end);
	void add(const Glyph &glyph, bool appearance);
	QPointF origin() const;
	QRectF cursorRect() const;

	const not_null<QWidget*> _field;
	const QPointer<QTextEdit> _edit;
	const QPointer<QLineEdit> _line;
	QPointer<QWidget> _overlay;
	QTimer _timer;
	QElapsedTimer _clock;
	qint64 _lastFrame = 0;
	qint64 _cursorMoved = 0;
	qint64 _spaceTime = -10000;
	qint64 _enterTime = -10000;
	qint64 _deleteTime = -10000;
	QRectF _cursor;
	QPointF _cursorDelta;
	QRegion _painted;
	std::deque<Particle> _particles;
	std::vector<std::weak_ptr<GlyphRaster>> _rasters;
	QList<QTextEdit::ExtraSelection> _originalSelections;
	int _originalCursorWidth = 1;
	bool _handling = false;
	bool _preedit = false;
	bool _hiding = false;
	bool _smoothCursor = false;
	bool _enabled = false;
	bool _fall = false;
	bool _ignoreFloor = false;
	Mode _mode = Mode::Blur;
	std::array<int, 16> _values = {};
	std::array<std::array<bool, 3>, 3> _disabled = {};
	QString _observedText;
	rpl::lifetime _lifetime;

};

class Overlay final : public QWidget {
public:
	Overlay(QWidget *parent, Fn<void(QPainter&)> paint)
	: QWidget(parent), _paint(std::move(paint)) {
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setAttribute(Qt::WA_NoSystemBackground);
		setFocusPolicy(Qt::NoFocus);
	}

private:
	void paintEvent(QPaintEvent *) override {
		auto p = QPainter(this);
		_paint(p);
	}

	Fn<void(QPainter&)> _paint;

};

InputAnimation::InputAnimation(not_null<InputField*> field)
: InputAnimation(field, field->rawTextEdit()) {
}

InputAnimation::InputAnimation(QWidget *field, QTextEdit *edit, QLineEdit *line)
: QObject(field)
, _field(field)
, _edit(edit)
, _line(line)
, _originalCursorWidth(edit ? edit->cursorWidth() : 1) {
	_clock.start();
	_observedText = plainText();
	editor()->installEventFilter(this);
	viewport()->installEventFilter(this);
	_field->installEventFilter(this);
	_timer.setInterval(16);
	connect(&_timer, &QTimer::timeout, this, [=] { frame(); });
	const auto cursorChanged = [=] {
		_cursorMoved = _clock.elapsed();
		if (_timer.interval() != 16) {
			_lastFrame = _clock.elapsed();
			_timer.setInterval(16);
		}
		if (!_handling) finishAppearance();
	};
	const auto textChanged = [=] {
		if (!_handling && editorAlive()) {
			const auto text = plainText();
			if (_observedText != text) {
				_observedText = text;
				refresh();
			}
		}
	};
	if (_edit) {
		connect(_edit, &QTextEdit::cursorPositionChanged, this, cursorChanged);
		connect(_edit->document(), &QTextDocument::contentsChanged, this, textChanged);
		connect(_edit->verticalScrollBar(), &QScrollBar::valueChanged,
			this, [=] { if (!_handling) finishAppearance(); });
		connect(_edit->horizontalScrollBar(), &QScrollBar::valueChanged,
			this, [=] { if (!_handling) finishAppearance(); });
	} else {
		connect(_line, &QLineEdit::cursorPositionChanged, this, cursorChanged);
		connect(_line, &QLineEdit::textChanged, this, textChanged);
	}
	connect(editor(), &QObject::destroyed, this, [=] {
		_timer.stop();
		_particles.clear();
		_originalSelections.clear();
		_hiding = false;
		if (_overlay) _overlay->hide();
	});
	Changes() | rpl::on_next([=] { refresh(); }, _lifetime);
	refresh();
}
InputAnimation::~InputAnimation() {
	clear();
	delete _overlay.data();
}

bool InputAnimation::editorAlive() const {
	return (_edit && dynamic_cast<QTextEdit*>(static_cast<QObject*>(_edit.data())))
		|| (_line && dynamic_cast<QLineEdit*>(static_cast<QObject*>(_line.data())));
}

QWidget *InputAnimation::editor() const {
	return _edit ? static_cast<QWidget*>(_edit.data()) : _line.data();
}

QWidget *InputAnimation::viewport() const {
	return _edit ? _edit->viewport() : static_cast<QWidget*>(_line.data());
}

QString InputAnimation::plainText() const {
	return _edit ? _edit->toPlainText() : _line->text();
}

std::pair<int, int> InputAnimation::selection() const {
	if (_edit) {
		const auto cursor = _edit->textCursor();
		return { cursor.selectionStart(), cursor.selectionEnd() };
	}
	return _line->hasSelectedText()
		? std::pair(_line->selectionStart(), _line->selectionEnd())
		: std::pair(_line->cursorPosition(), _line->cursorPosition());
}

QRectF InputAnimation::positionRect(int position) const {
	if (_edit) {
		auto cursor = QTextCursor(_edit->document());
		cursor.setPosition(position);
		return _edit->cursorRect(cursor);
	}
	const auto data = static_cast<QLineEditPrivate*>(QObjectPrivate::get(_line));
	auto result = QRectF(data->adjustedControlRect(data->control->rectForPos(position)));
	result.setLeft(result.left() + 5);
	result.setWidth(style::ConvertScale(2));
	return result;
}

void InputAnimation::setCursorWidth(int width) {
	if (_edit) {
		_edit->setCursorWidth(width);
	} else {
		const auto data = static_cast<QLineEditPrivate*>(QObjectPrivate::get(_line));
		data->cursorVisible = editor()->hasFocus() && width > 0;
		_line->update();
	}
}

void InputAnimation::setLineFormats(const QList<QTextLayout::FormatRange> &formats) {
	const auto data = static_cast<QLineEditPrivate*>(QObjectPrivate::get(_line));
	const auto layout = data->control->textLayout();
	const auto old = layout->lineCount() ? layout->lineAt(0) : QTextLine();
	const auto width = old.isValid() ? old.width() : _line->width();
	const auto position = old.isValid() ? old.position() : QPointF();
	layout->setFormats(formats);
	layout->beginLayout();
	auto line = layout->createLine();
	if (line.isValid()) {
		line.setLineWidth(width);
		line.setPosition(position);
	}
	layout->endLayout();
	_line->update();
}

QPointF InputAnimation::origin() const {
	return _overlay
		? QPointF(_overlay->mapFromGlobal(viewport()->mapToGlobal(QPoint())))
		: QPointF();
}

QRectF InputAnimation::cursorRect() const {
	auto result = positionRect(_edit
		? _edit->textCursor().position() : _line->cursorPosition());
	result.setWidth(std::max(1, style::ConvertScale(2)));
	return result.translated(origin());
}

void InputAnimation::refresh() {
	if (!editorAlive()) return;
	clear();
	_enabled = Enabled();
	_mode = CurrentMode();
	_fall = Fall();
	_ignoreFloor = IgnoreFloor();
	for (auto i = 0; i < int(_values.size()); ++i) _values[i] = Value(Parameter(i));
	for (const auto mode : { Mode::Blur, Mode::Explosion, Mode::Slide }) {
		for (const auto phase : { Phase::Appearance, Phase::Removal, Phase::BlurAppearance }) {
			_disabled[int(mode)][int(phase)] = Disabled(mode, phase);
		}
	}
	_smoothCursor = _enabled && value(Parameter::Cursor) > 0;
	setCursorWidth(_smoothCursor && editor()->hasFocus()
		? 0 : _originalCursorWidth);
	if (_enabled && editor()->hasFocus() && _field->isVisible()) {
		if (!_overlay || _overlay->parentWidget() != _field->window()) {
			delete _overlay.data();
			_overlay = new Overlay(_field->window(), [=](QPainter &p) { paint(p); });
		}
		_overlay->setGeometry(_field->window()->rect());
		_overlay->show();
		_overlay->raise();
		_cursor = cursorRect();
		_lastFrame = _clock.elapsed();
		_timer.setInterval(16);
		if (_smoothCursor) _timer.start();
	}
}

void InputAnimation::clear() {
	_timer.stop();
	if (_overlay) _overlay->hide();
	_particles.clear();
	if (!editorAlive()) {
		_originalSelections.clear();
		_hiding = false;
		return;
	}
	selections();
	setCursorWidth(_originalCursorWidth);
}

bool InputAnimation::eventFilter(QObject *object, QEvent *event) {
	if (!editorAlive()) return false;
	if (_handling) return false;
	const auto type = event->type();
	if (_line && object == editor() && type == QEvent::Paint && _enabled) {
		const auto data = static_cast<QLineEditPrivate*>(QObjectPrivate::get(_line));
		data->cursorVisible = editor()->hasFocus() && (!_smoothCursor || _preedit);
	}
	if (object == editor() && type == QEvent::FocusIn) {
		QTimer::singleShot(0, this, [=] { refresh(); });
	} else if (type == QEvent::Hide
		|| (object == editor() && type == QEvent::FocusOut)) {
		clear();
		setCursorWidth(_originalCursorWidth);
		_preedit = false;
	} else if (object == editor() && _enabled
			&& (type == QEvent::KeyPress || type == QEvent::InputMethod)) {
		if ((_edit ? _edit->isReadOnly() : _line->isReadOnly()) || !editor()->hasFocus()) return false;
		if (type == QEvent::KeyPress) {
			const auto key = static_cast<QKeyEvent*>(event);
			if (key->key() == Qt::Key_Escape) return false;
		}
		auto composing = _preedit;
		if (type == QEvent::InputMethod) {
			const auto input = static_cast<QInputMethodEvent*>(event);
			_preedit = !input->preeditString().isEmpty();
			composing = composing || _preedit;
		}
		const auto before = _observedText;
		auto [start, end] = selection();
		auto bulkInput = false;
		if (type == QEvent::InputMethod) {
			bulkInput = static_cast<QInputMethodEvent*>(event)->commitString().size()
				> kMaxAnimatedEdit;
		}
		if (type == QEvent::KeyPress) {
			const auto key = static_cast<QKeyEvent*>(event);
			bulkInput = key->matches(QKeySequence::Paste);
			if (start == end) {
				auto boundary = _edit ? _edit->textCursor() : QTextCursor();
				if (key->key() == Qt::Key_Backspace) {
					boundary.movePosition(key->modifiers() & Qt::ControlModifier
						? QTextCursor::PreviousWord : QTextCursor::PreviousCharacter);
					start = _edit ? boundary.position()
						: static_cast<QLineEditPrivate*>(QObjectPrivate::get(_line))->control->textLayout()
							->previousCursorPosition(start, key->modifiers() & Qt::ControlModifier
								? QTextLayout::SkipWords : QTextLayout::SkipCharacters);
				} else if (key->key() == Qt::Key_Delete) {
					boundary.movePosition(key->modifiers() & Qt::ControlModifier
						? QTextCursor::NextWord : QTextCursor::NextCharacter);
					end = _edit ? boundary.position()
						: static_cast<QLineEditPrivate*>(QObjectPrivate::get(_line))->control->textLayout()
							->nextCursorPosition(end, key->modifiers() & Qt::ControlModifier
								? QTextLayout::SkipWords : QTextLayout::SkipCharacters);
				}
			}
		}
		auto removed = (!composing && !bulkInput && end - start <= kMaxAnimatedEdit
			&& !disabled(_mode, Phase::Removal))
			? capture(start, end) : std::vector<Glyph>();
		struct AppearanceRange {
			int index = 0;
			int from = 0;
			int till = 0;
		};
		auto ranges = std::vector<AppearanceRange>();
		for (auto i = 0; i < int(_particles.size()); ++i) {
			const auto &particle = _particles[i];
			if (particle.appearance) {
				ranges.push_back({ i, _line ? particle.from : particle.cursor.selectionStart(),
					_line ? particle.till : particle.cursor.selectionEnd() });
			}
		}
		const auto guard = QPointer<InputAnimation>(this);
		_handling = true;
		QCoreApplication::sendEvent(object, event);
		if (!guard) return true;
		_handling = false;
		if (!editorAlive() || !_field->isVisible() || !editor()->hasFocus()) {
			clear();
			return true;
		}
		const auto after = plainText();
		_observedText = after;
		if (before == after) return true;
		if (composing) {
			refresh();
			return true;
		}
		if (!_overlay || !_overlay->isVisible()) refresh();
		auto prefix = start;
		auto oldEnd = end;
		auto newEnd = end + int(after.size() - before.size());
		const auto oldView = QStringView(before);
		const auto newView = QStringView(after);
		if (newEnd < prefix || newEnd > after.size()
			|| oldView.first(prefix) != newView.first(prefix)
			|| oldView.sliced(oldEnd) != newView.sliced(newEnd)) {
			prefix = 0;
			while (prefix < before.size() && prefix < after.size()
				&& before[prefix] == after[prefix]) ++prefix;
			oldEnd = int(before.size());
			newEnd = int(after.size());
			while (oldEnd > prefix && newEnd > prefix
				&& before[oldEnd - 1] == after[newEnd - 1]) {
				--oldEnd;
				--newEnd;
			}
		}
		const auto inserted = newView.sliced(prefix, newEnd - prefix);
		if (inserted.contains(' ')) _spaceTime = _clock.elapsed();
		if (inserted.contains('\n')) _enterTime = _clock.elapsed();
		if (oldEnd - prefix > newEnd - prefix) _deleteTime = _clock.elapsed();
		for (auto i = int(ranges.size()); i > 0;) {
			const auto range = ranges[--i];
			auto &particle = _particles[range.index];
			auto from = range.from;
			auto till = range.till;
			if (from < oldEnd && till > prefix) {
				_particles.erase(_particles.begin() + range.index);
				continue;
			}
			if (from >= oldEnd) {
				from += newEnd - oldEnd;
				till += newEnd - oldEnd;
			}
			if (_edit) {
				particle.cursor.setPosition(from);
				particle.cursor.setPosition(till, QTextCursor::KeepAnchor);
			}
			particle.from = from;
			particle.till = till;
		}
		const auto animateEdit = oldEnd - prefix <= kMaxAnimatedEdit
			&& newEnd - prefix <= kMaxAnimatedEdit;
		if (animateEdit) {
			for (const auto &glyph : removed) {
				if (glyph.start >= prefix && glyph.start < oldEnd) add(glyph, false);
			}
		}
		const auto mode = _mode;
		const auto appearance = !disabled(mode, Phase::Appearance)
			|| (mode == Mode::Slide && !disabled(mode, Phase::BlurAppearance));
		if (appearance && animateEdit) {
			for (const auto &glyph : capture(prefix, newEnd)) add(glyph, true);
		}
		if (_hiding || (appearance && animateEdit && newEnd > prefix)) selections();
		if (!_timer.isActive() && (!_particles.empty() || _smoothCursor)) {
			_lastFrame = _clock.elapsed();
			_timer.start(16);
		} else if (_timer.interval() != 16) {
			_timer.setInterval(16);
		}
		return true;
	}
	return false;
}

std::vector<Glyph> InputAnimation::capture(int start, int end) {
	auto result = std::vector<Glyph>();
	if (_line) {
		const auto data = static_cast<QLineEditPrivate*>(QObjectPrivate::get(_line));
		const auto native = data->control->textLayout();
		auto display = _line->displayText();
		if (_line->echoMode() == QLineEdit::NoEcho || !native->lineCount()) return result;
		if (_line->echoMode() != QLineEdit::Normal) {
			display.fill(data->control->passwordCharacter());
		}
		if (start < 0 || end <= start || end > display.size() || !_overlay) return result;
		auto layout = QTextLayout(display, _line->font());
		layout.setTextOption(native->textOption());
		layout.beginLayout();
		auto line = layout.createLine();
		line.setLineWidth(native->lineAt(0).width());
		line.setPosition(native->lineAt(0).position());
		layout.endLayout();
		auto boundary = QTextBoundaryFinder(QTextBoundaryFinder::Grapheme, display);
		boundary.setPosition(start);
		if (!boundary.isAtBoundary()) start = std::max(0, int(boundary.toPreviousBoundary()));
		for (auto position = start; position < end && result.size() < 50;) {
			boundary.setPosition(position);
			const auto next = int(boundary.toNextBoundary());
			if (next <= position) break;
			const auto left = positionRect(position);
			const auto right = positionRect(next);
			const auto rect = QRectF(std::min(left.x(), right.x()), left.y(),
				std::abs(right.x() - left.x()), left.height()).toAlignedRect();
			if (!rect.isEmpty() && _line->rect().contains(rect)
				&& !display.mid(position, next - position).trimmed().isEmpty()) {
				const auto dpr = _line->devicePixelRatioF();
				auto image = QImage((QSizeF(rect.size()) * dpr).toSize(),
					QImage::Format_ARGB32_Premultiplied);
				image.setDevicePixelRatio(dpr);
				image.fill(Qt::transparent);
				auto p = QPainter(&image);
				p.setPen(_line->palette().color(QPalette::Text));
				line.draw(&p, left.topLeft() - QPointF(line.cursorToX(position), line.y())
					- rect.topLeft());
				p.end();
				result.push_back({ std::move(image), QRectF(rect).translated(origin()), position, next });
			}
			position = next;
		}
		return result;
	}
	const auto &text = _observedText;
	if (end <= start || start < 0 || end > text.size() || !_overlay) return result;
	const auto viewportRect = viewport()->rect();
	const auto topLeft = _edit->cursorForPosition(viewportRect.topLeft()).position();
	const auto topRight = _edit->cursorForPosition(viewportRect.topRight()).position();
	const auto bottomLeft = _edit->cursorForPosition(viewportRect.bottomLeft()).position();
	const auto bottomRight = _edit->cursorForPosition(viewportRect.bottomRight()).position();
	start = std::max(start, std::min(topLeft, topRight));
	end = std::min(end, std::max(bottomLeft, bottomRight));
	if (end <= start) return result;
	auto boundary = QTextBoundaryFinder(QTextBoundaryFinder::Grapheme, text);
	boundary.setPosition(start);
	if (!boundary.isAtBoundary()) {
		start = std::max(0, int(boundary.toPreviousBoundary()));
	}
	for (auto position = start, examined = 0;
		position < end && result.size() < 50 && examined < 128; ++examined) {
		boundary.setPosition(position);
		const auto next = int(boundary.toNextBoundary());
		if (next <= position) break;
		auto cursor = QTextCursor(_edit->document());
		cursor.setPosition(position);
		const auto left = _edit->cursorRect(cursor);
		cursor.setPosition(next);
		const auto right = _edit->cursorRect(cursor);
		const auto cluster = text.mid(position, next - position);
		if (!cluster.trimmed().isEmpty()
			&& !cluster.contains(QChar::ObjectReplacementCharacter)
			&& left.top() == right.top()) {
			const auto rect = QRect(std::min(left.x(), right.x()), left.y(),
				std::abs(right.x() - left.x()), left.height());
			if (!rect.isEmpty() && viewport()->rect().contains(rect)) {
				const auto dpr = _edit->devicePixelRatioF();
				auto image = QImage((QSizeF(rect.size()) * dpr).toSize(),
					QImage::Format_ARGB32_Premultiplied);
				image.setDevicePixelRatio(dpr);
				image.fill(Qt::transparent);
				auto p = QPainter(&image);
				const auto block = _edit->document()->findBlock(position);
				const auto layout = block.layout();
				if (layout) {
					const auto line = layout->lineForTextPosition(position - block.position());
					const auto offset = QPointF(left.x() - line.cursorToX(position - block.position()),
						left.y() - line.y());
					p.setPen(_edit->palette().color(QPalette::Text));
					line.draw(&p, offset - rect.topLeft());
				}
				p.end();
				result.push_back({ std::move(image), QRectF(rect).translated(origin()), position, next });
			}
		}
		position = next;
	}
	return result;
}

void InputAnimation::add(const Glyph &glyph, bool appearance) {
	const auto mode = _mode;
	auto particle = Particle();
	particle.image = glyph.image;
	particle.position = glyph.rect.topLeft();
	particle.mode = appearance && mode == Mode::Explosion ? Mode::Blur : mode;
	particle.appearance = appearance;
	particle.born = _clock.elapsed();
	particle.fade = value(Parameter::Fade);
	particle.blur = style::ConvertScale(value(Parameter::Blur));
	particle.blurDuration = value(Parameter::BlurDuration);
	particle.slideDuration = value(Parameter::SlideDuration);
	particle.duration = appearance
		? std::max(particle.fade, particle.blurDuration) : particle.fade;
	if (mode == Mode::Slide) {
		const auto angle = value(Parameter::SlideAngle) * std::numbers::pi / 180.;
		const auto distance = style::ConvertScale(value(Parameter::DistanceMin)
			+ int(Random() * (value(Parameter::DistanceMax) - value(Parameter::DistanceMin) + 1)));
		if (!appearance || !disabled(mode, Phase::Appearance)) {
			particle.displacement = QPointF(std::cos(angle), std::sin(angle)) * distance;
			particle.duration = std::max(particle.duration, particle.slideDuration);
		}
		if (appearance && disabled(mode, Phase::BlurAppearance)) {
			particle.fade = 0;
			particle.blur = 0.;
			particle.duration = particle.slideDuration;
		} else if (!appearance) {
			particle.blur = 0.;
			particle.fade = particle.duration = particle.slideDuration;
		}
	}
	particle.from = glyph.start;
	particle.till = glyph.end;
	if (appearance && _edit) {
		particle.cursor = QTextCursor(_edit->document());
		particle.cursor.setPosition(glyph.start);
		particle.cursor.setPosition(glyph.end, QTextCursor::KeepAnchor);
	}
	if (mode != Mode::Explosion || appearance) {
		if (particle.blur > 0.) {
			std::erase_if(_rasters, [](const auto &weak) { return weak.expired(); });
			for (const auto &weak : _rasters) {
				const auto raster = weak.lock();
				if (raster && raster->source == particle.image) {
					particle.raster = raster;
					break;
				}
			}
			if (!particle.raster) {
				particle.raster = std::make_shared<GlyphRaster>();
				particle.raster->source = particle.image;
				_rasters.push_back(particle.raster);
			}
		}
		if (_particles.size() == kMaxParticles) _particles.pop_front();
		_particles.push_back(std::move(particle));
		return;
	}
	const auto image = glyph.image;
	const auto width = image.width();
	const auto height = image.height();
	auto ink = std::vector<QPoint>();
	for (auto y = 0; y < height; ++y) {
		const auto pixels = reinterpret_cast<const QRgb*>(image.constScanLine(y));
		for (auto x = 0; x < width; ++x) {
			if (qAlpha(pixels[x])) ink.emplace_back(x, y);
		}
	}
	if (ink.empty()) return;
	const auto count = std::min(value(Parameter::Shards), int(ink.size()));
	if (_particles.size() + count > kMaxParticles) {
		const auto excess = int(_particles.size()) + count - kMaxParticles;
		for (auto i = 0; i < excess; ++i) _particles.pop_front();
	}
	auto assignment = std::vector<int>(ink.size(), 0);
	auto distances = std::vector<int>(ink.size(), std::numeric_limits<int>::max());
	auto seed = ink[int(Random() * ink.size())];
	for (auto i = 0; i < count; ++i) {
		auto farthest = -1;
		const auto center = seed;
		for (auto j = 0; j < int(ink.size()); ++j) {
			const auto delta = ink[j] - center;
			const auto distance = delta.x() * delta.x() + delta.y() * delta.y();
			if (distance < distances[j]) {
				distances[j] = distance;
				assignment[j] = i;
			}
			if (distances[j] > farthest) {
				farthest = distances[j];
				seed = ink[j];
			}
		}
	}
	auto pieces = std::vector<QImage>(count);
	for (auto &piece : pieces) {
		piece = QImage(image.size(), image.format());
		piece.setDevicePixelRatio(image.devicePixelRatio());
		piece.fill(Qt::transparent);
	}
	auto bounds = std::vector<QRect>(count);
	for (auto j = 0; j < int(ink.size()); ++j) {
		const auto point = ink[j];
		const auto index = assignment[j];
		pieces[index].setPixel(point, image.pixel(point));
		bounds[index] |= QRect(point, QSize(1, 1));
	}
	const auto scale = image.devicePixelRatio();
	for (auto i = 0; i < count; ++i) {
		if (bounds[i].isEmpty()) continue;
		auto shard = particle;
		shard.image = pieces[i].copy(bounds[i]);
		shard.position += QPointF(bounds[i].topLeft()) / scale;
		const auto radial = QPointF(bounds[i].center()) - QPointF(image.rect().center());
		const auto angle = std::atan2(radial.y(), radial.x()) + (Random() - .5) * .5;
		const auto speed = style::ConvertScale(value(Parameter::Force)) * (.65 + .55 * Random());
		shard.velocity = QPointF(std::cos(angle), std::sin(angle)) * speed;
		shard.rotationStrength = value(Parameter::Spin);
		shard.spin = shard.rotationStrength
			* std::min(1., speed / style::ConvertScale(340))
			* (.5 + .5 * Random()) * (Random() < .5 ? -1 : 1);
		shard.gravity = style::ConvertScale(value(Parameter::Gravity));
		shard.direction = value(Parameter::Direction) * std::numbers::pi / 180.;
		shard.acceleration = QPointF(std::sin(shard.direction),
			std::cos(shard.direction));
		shard.bounce = value(Parameter::Bounce) / 100.;
		shard.duration = value(Parameter::LifetimeMin)
			+ int(Random() * (value(Parameter::LifetimeMax) - value(Parameter::LifetimeMin) + 1));
		shard.fall = _fall;
		shard.ignoreFloor = _ignoreFloor;
		shard.blur = 0.;
		_particles.push_back(std::move(shard));
	}
}

int InputAnimation::value(Parameter parameter) const {
	return _values[int(parameter)];
}

bool InputAnimation::disabled(Mode mode, Phase phase) const {
	return _disabled[int(mode)][int(phase)];
}

void InputAnimation::finishAppearance() {
	const auto removed = std::erase_if(_particles, [](const Particle &particle) {
		return particle.appearance;
	});
	if (removed && _hiding) selections();
}

void InputAnimation::selections() {
	if (!editorAlive()) return;
	if (!_hiding && std::ranges::none_of(_particles, [](const Particle &particle) {
		return particle.appearance;
	})) return;
	if (_line) {
		const auto data = static_cast<QLineEditPrivate*>(QObjectPrivate::get(_line));
		auto formats = data->control->textLayout()->formats();
		formats.erase(std::remove_if(formats.begin(), formats.end(), [](const auto &range) {
			return range.format.property(kHiddenGlyphFormat).toBool();
		}), formats.end());
		const auto original = formats.size();
		for (const auto &particle : _particles) {
			if (!particle.appearance || particle.till <= particle.from) continue;
			auto range = QTextLayout::FormatRange();
			range.start = particle.from;
			range.length = particle.till - particle.from;
			range.format.setForeground(Qt::transparent);
			range.format.setProperty(kHiddenGlyphFormat, true);
			formats.push_back(std::move(range));
		}
		_hiding = formats.size() != original;
		setLineFormats(formats);
		return;
	}
	auto selections = _hiding ? _originalSelections : _edit->extraSelections();
	const auto original = selections;
	for (const auto &particle : _particles) {
		if (!particle.appearance || !particle.cursor.hasSelection()) continue;
		auto selection = QTextEdit::ExtraSelection();
		selection.cursor = particle.cursor;
		selection.format.setForeground(Qt::transparent);
		selections.push_back(selection);
	}
	_originalSelections = original;
	_hiding = selections.size() != original.size();
	_edit->setExtraSelections(selections);
}

void InputAnimation::frame() {
	if (!editorAlive() || !_enabled || !_field->isVisible() || !editor()->hasFocus() || !_overlay) {
		clear();
		return;
	}
	const auto now = _clock.elapsed();
	const auto dt = std::clamp((now - _lastFrame) / 1000., 0., .048);
	_lastFrame = now;
	_overlay->setGeometry(_field->window()->rect());
	const auto target = cursorRect();
	_cursorDelta = target.topLeft() - _cursor.topLeft();
	const auto factor = std::min(1., value(Parameter::Cursor) / 100. * dt / .016 * .22);
	_cursor.translate((target.topLeft() - _cursor.topLeft()) * factor);
	_cursor.setHeight(_cursor.height() + (target.height() - _cursor.height()) * factor);
	const auto floor = _overlay->height();
	const auto width = _overlay->width();
	for (auto &particle : _particles) {
		if (particle.mode != Mode::Explosion || particle.appearance) continue;
		const auto age = now - particle.born;
		auto remaining = dt;
		while (remaining > 0.) {
			const auto step = std::min(.008, remaining);
			remaining -= step;
			const auto dropping = particle.fall && age >= particle.duration;
			const auto gravity = dropping
				? std::max(particle.gravity, double(style::ConvertScale(1250)))
				: particle.gravity;
			particle.velocity += particle.acceleration * gravity * step;
			particle.position += particle.velocity * step;
			particle.angle += particle.spin * step;
			particle.spin *= 1. - 6. * step;
			if (dropping && (particle.ignoreFloor
				|| (particle.impact && now - particle.impact >= 120))) {
				particle.released = true;
			}
			if (particle.ignoreFloor) continue;
			const auto size = QSizeF(particle.image.size()) / particle.image.devicePixelRatio();
			const auto radians = particle.angle * std::numbers::pi / 180.;
			const auto sine = std::abs(std::sin(radians));
			const auto cosine = std::abs(std::cos(radians));
			const auto half = QPointF(size.width() / 2., size.height() / 2.);
			auto center = particle.position + half;
			const auto extentX = std::min(width / 2., cosine * half.x() + sine * half.y());
			const auto extentY = std::min(floor / 2., sine * half.x() + cosine * half.y());
			const auto wall = [&](double position, double min, double max, double direction) {
				return position <= min && (!particle.released || direction >= 0.) ? -1
					: position >= max && (!particle.released || direction <= 0.) ? 1 : 0;
			};
			const auto rebound = [&](double velocity, double tangent) {
				const auto speed = std::abs(velocity) * particle.bounce;
				if (speed < style::ConvertScale(22)) {
					particle.spin = 0.;
					return 0.;
				}
				const auto sign = std::abs(tangent) > style::ConvertScale(8)
					? std::copysign(1., tangent) : std::copysign(1., particle.spin);
				particle.spin = sign * particle.rotationStrength
					* std::min(1., speed / style::ConvertScale(340));
				return -std::copysign(speed, velocity);
			};
			const auto wallX = wall(center.x(), extentX, width - extentX, particle.acceleration.x());
			if (wallX) {
				center.setX(wallX < 0 ? extentX : width - extentX);
				if (particle.velocity.x() * wallX > 0.) {
					if (wallX * particle.acceleration.x() > 0. && !particle.impact) particle.impact = now;
					particle.velocity.setX(rebound(particle.velocity.x(), particle.velocity.y()));
					if (particle.velocity.x() != 0.) particle.velocity.ry() *= .9;
				}
			}
			const auto wallY = wall(center.y(), extentY, floor - extentY, particle.acceleration.y());
			if (wallY) {
				center.setY(wallY < 0 ? extentY : floor - extentY);
				if (particle.velocity.y() * wallY > 0.) {
					if (wallY * particle.acceleration.y() > 0. && !particle.impact) particle.impact = now;
					particle.velocity.setY(rebound(particle.velocity.y(), particle.velocity.x()));
					if (particle.velocity.y() != 0.) particle.velocity.rx() *= .9;
					else if (std::abs(particle.acceleration.x()) < .001) particle.velocity.setX(0.);
				}
			}
			particle.position = center - half;
		}
	}
	const auto removed = std::erase_if(_particles, [&](const Particle &particle) {
		if (now - particle.born >= particle.duration + (particle.fall ? 2500 : 0)) return true;
		if (!particle.released && !particle.ignoreFloor) return false;
		const auto size = QSizeF(particle.image.size()) / particle.image.devicePixelRatio();
		const auto radius = std::hypot(size.width(), size.height());
		const auto position = particle.position;
		return (particle.acceleration.x() < -.001 && position.x() + radius < 0.)
			|| (particle.acceleration.x() > .001 && position.x() - radius > width)
			|| (particle.acceleration.y() < -.001 && position.y() + radius < 0.)
			|| (particle.acceleration.y() > .001 && position.y() - radius > floor);
	});
	if (removed) selections();
	auto bounds = _cursor.adjusted(-30, -30, 30, 30);
	const auto viewportBounds = QRectF(viewport()->rect()).translated(origin());
	for (const auto &particle : _particles) {
		if (particle.appearance || particle.mode == Mode::Blur) {
			bounds = bounds.united(viewportBounds);
		} else {
			const auto size = QSizeF(particle.image.size()) / particle.image.devicePixelRatio();
			const auto radius = std::hypot(size.width(), size.height());
			const auto position = particle.position + particle.displacement;
			bounds = bounds.united(QRectF(particle.position, size)
				.united(QRectF(position, size))
				.adjusted(-radius, -radius, radius, radius));
		}
	}
	const auto painted = QRegion(bounds.toAlignedRect().intersected(_overlay->rect()));
	_overlay->update(painted.united(_painted));
	_painted = std::move(painted);
	if (_particles.empty() && !_smoothCursor) _timer.stop();
	else if (_particles.empty() && std::abs(_cursorDelta.x()) < .02
		&& std::abs(_cursorDelta.y()) < .02 && now - _spaceTime >= 300
		&& now - _enterTime >= 400 && now - _deleteTime >= 300) {
		const auto flash = QApplication::cursorFlashTime();
		_timer.setInterval(flash > 0 ? std::max(16, flash / 2) : 500);
	}
}

void InputAnimation::paint(QPainter &p) {
	if (!editorAlive() || !editor()->hasFocus() || !_enabled) return;
	const auto now = _clock.elapsed();
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	for (auto &particle : _particles) {
		const auto age = double(now - particle.born);
		auto position = particle.position;
		auto opacity = 1.;
		auto blur = 0.;
		if (particle.appearance) {
			if (_edit && !particle.cursor.hasSelection()) continue;
			const auto rect = positionRect(_line ? particle.till : particle.cursor.selectionEnd());
			const auto left = positionRect(_line ? particle.from : particle.cursor.selectionStart());
			position = QPointF(std::min(left.x(), rect.x()), left.y()) + origin();
			position -= particle.displacement * (1. - Ease(age / particle.slideDuration));
			opacity = particle.fade ? std::clamp(age / particle.fade, 0., 1.) : 1.;
			blur = particle.blur * (1. - std::clamp(age / particle.blurDuration, 0., 1.));
		} else if (particle.mode == Mode::Slide) {
			const auto t = Ease(age / particle.duration);
			position += particle.displacement * t;
			opacity = 1. - t;
		} else if (particle.mode == Mode::Blur) {
			opacity = 1. - std::clamp(age / particle.fade, 0., 1.);
			blur = particle.blur * std::clamp(age / particle.blurDuration, 0., 1.);
		} else if (!particle.fall) {
			opacity = std::clamp((particle.duration - age) / (particle.duration * .45), 0., 1.);
		}
		p.save();
		if (particle.appearance || particle.mode == Mode::Blur) {
			p.setClipRect(QRectF(viewport()->rect()).translated(origin()));
		}
		const auto size = QSizeF(particle.image.size()) / particle.image.devicePixelRatio();
		p.translate(position + QPointF(size.width() / 2., size.height() / 2.));
		p.rotate(particle.angle);
		const auto target = QPointF(-size.width() / 2., -size.height() / 2.);
		p.setOpacity(opacity);
		const auto radius = int(std::round(blur * particle.image.devicePixelRatio() * .25));
		if (radius > 0) {
			auto &cache = particle.raster->blurred;
			auto i = cache.constFind(radius);
			if (i == cache.cend()) {
				cache.insert(radius, BlurGlyph(particle.image, radius));
				i = cache.constFind(radius);
			}
			const auto padding = radius * 3. / particle.image.devicePixelRatio();
			p.drawImage(target - QPointF(padding, padding), i.value());
		} else {
			p.drawImage(target, particle.image);
		}
		p.restore();
	}
	if (_smoothCursor && selection().first == selection().second && !_preedit) {
		const auto flash = QApplication::cursorFlashTime();
		const auto visible = flash <= 0 || (now - _cursorMoved) % flash < flash / 2;
		if (visible) {
			p.setClipRect(QRectF(viewport()->rect()).translated(origin()));
			const auto pulse = [&](qint64 start, int duration) {
				const auto age = now - start;
				return age >= 0 && age < duration
					? std::sin(age * std::numbers::pi / duration) : 0.;
			};
			const auto stretchX = std::min(std::abs(_cursorDelta.x()) * .25,
				double(style::ConvertScale(12)));
			const auto stretchY = std::min(std::abs(_cursorDelta.y()) * .2,
				_cursor.height() * .3);
			auto rect = _cursor;
			if (_cursorDelta.x() > 0.) rect.setLeft(rect.left() - stretchX);
			else rect.setRight(rect.right() + stretchX);
			if (_cursorDelta.y() > 0.) rect.setTop(rect.top() - stretchY);
			else rect.setBottom(rect.bottom() + stretchY);
			const auto horizontal = pulse(_spaceTime, 300) * style::ConvertScale(8)
				- pulse(_deleteTime, 300) * style::ConvertScale(1);
			const auto vertical = pulse(_deleteTime, 300) * style::ConvertScale(2)
				- stretchX * .2;
			rect.adjust(-horizontal / 2., -vertical, horizontal / 2., vertical);
			rect.translate(0., pulse(_enterTime, 400) * style::ConvertScale(12));
			p.setPen(Qt::NoPen);
			p.setBrush(editor()->palette().color(QPalette::Text));
			p.drawRoundedRect(rect, rect.width() / 2., rect.width() / 2.);
		}
	}
}

}

ParameterSpec Spec(Parameter parameter) {
	switch (parameter) {
	case Parameter::Cursor: return { "cursor", 0, 100, 1, 80, QString() };
	case Parameter::Fade: return { "fade", 50, 800, 5, 300, u"ms"_q };
	case Parameter::Blur: return { "blur", 0, 30, 1, 20, QString() };
	case Parameter::BlurDuration: return { "blur-duration", 50, 1000, 5, 350, u"ms"_q };
	case Parameter::Force: return { "force", 0, 1200, 10, 340, u"dp/s"_q };
	case Parameter::Gravity: return { "gravity", 0, 4000, 50, 1300, u"dp/s²"_q };
	case Parameter::Bounce: return { "bounce", 0, 100, 1, 40, u"%"_q };
	case Parameter::Spin: return { "spin", 0, 1440, 30, 540, u"°/s"_q };
	case Parameter::Shards: return { "shards", 2, 24, 1, 9, QString() };
	case Parameter::LifetimeMin: return { "lifetime-min", 100, 3000, 25, 500, u"ms"_q };
	case Parameter::LifetimeMax: return { "lifetime-max", 100, 3000, 25, 850, u"ms"_q };
	case Parameter::Direction: return { "direction", 0, 360, 5, 0, u"°"_q };
	case Parameter::SlideAngle: return { "slide-angle", 0, 360, 5, 0, u"°"_q };
	case Parameter::DistanceMin: return { "distance-min", 0, 120, 1, 16, u"dp"_q };
	case Parameter::DistanceMax: return { "distance-max", 0, 120, 1, 36, u"dp"_q };
	case Parameter::SlideDuration: return { "slide-duration", 50, 1500, 25, 250, u"ms"_q };
	}
	Unexpected("Text animation parameter.");
}

QString Title(Parameter parameter) {
	switch (parameter) {
	case Parameter::Cursor: return tr::lng_xenon_text_cursor(tr::now);
	case Parameter::Fade: return tr::lng_xenon_text_fade(tr::now);
	case Parameter::Blur: return tr::lng_xenon_text_blur_strength(tr::now);
	case Parameter::BlurDuration: return tr::lng_xenon_text_blur_duration(tr::now);
	case Parameter::Force: return tr::lng_xenon_text_force(tr::now);
	case Parameter::Gravity: return tr::lng_xenon_text_gravity(tr::now);
	case Parameter::Bounce: return tr::lng_xenon_text_bounce(tr::now);
	case Parameter::Spin: return tr::lng_xenon_text_spin(tr::now);
	case Parameter::Shards: return tr::lng_xenon_text_shards(tr::now);
	case Parameter::LifetimeMin: return tr::lng_xenon_text_lifetime_min(tr::now);
	case Parameter::LifetimeMax: return tr::lng_xenon_text_lifetime_max(tr::now);
	case Parameter::Direction: return tr::lng_xenon_text_direction(tr::now);
	case Parameter::SlideAngle: return tr::lng_xenon_text_slide_angle(tr::now);
	case Parameter::DistanceMin: return tr::lng_xenon_text_distance_min(tr::now);
	case Parameter::DistanceMax: return tr::lng_xenon_text_distance_max(tr::now);
	case Parameter::SlideDuration: return tr::lng_xenon_text_slide_duration(tr::now);
	}
	Unexpected("Text animation parameter.");
}

bool Enabled() { return Read("enabled") != 0; }
void SetEnabled(bool enabled) { Write("enabled", enabled); }
Mode CurrentMode() { return Mode(std::clamp(Read("mode"), 0, 2)); }
void SetMode(Mode mode) { Write("mode", int(mode)); }
int Value(Parameter parameter) {
	const auto spec = Spec(parameter);
	return std::clamp(Read(spec.key, spec.initial), spec.min, spec.max);
}
void SetValue(Parameter parameter, int value) {
	const auto spec = Spec(parameter);
	value = std::clamp(value, spec.min, spec.max);
	const auto other = parameter == Parameter::LifetimeMin ? Parameter::LifetimeMax
		: parameter == Parameter::LifetimeMax ? Parameter::LifetimeMin
		: parameter == Parameter::DistanceMin ? Parameter::DistanceMax
		: parameter == Parameter::DistanceMax ? Parameter::DistanceMin : parameter;
	const auto minimum = parameter == Parameter::LifetimeMin || parameter == Parameter::DistanceMin;
	if (other != parameter && (minimum ? value > Value(other) : value < Value(other))) {
		Core::App().settings().writePref<int>(Key(Spec(other).key), value);
	}
	Write(spec.key, value);
}
bool Disabled(Mode mode, Phase phase) {
	return Core::App().settings().readPref<bool>(PhaseKey(mode, phase));
}
void SetDisabled(Mode mode, Phase phase, bool disabled) {
	Core::App().settings().writePref<bool>(PhaseKey(mode, phase), disabled);
	Core::App().saveSettingsDelayed();
	Changed.fire({});
}
bool Fall() { return Read("fall") != 0; }
void SetFall(bool enabled) { Write("fall", enabled); }
bool IgnoreFloor() { return Read("ignore-floor") != 0; }
void SetIgnoreFloor(bool enabled) { Write("ignore-floor", enabled); }
rpl::producer<> Changes() { return Changed.events(); }
void Attach(not_null<InputField*> field) {
	if (field->property("xenon-text-animation-attached").toBool()) return;
	field->setProperty("xenon-text-animation-attached", true);
	field->rawTextEdit()->setProperty("xenon-text-animation-attached", true);
	const auto animation = new InputAnimation(field);
	field->lifetime().add([weak = QPointer<InputAnimation>(animation)] {
		delete weak.data();
	});
}


void Install() {
	static auto installed = false;
	if (installed) return;
	installed = true;
	const auto attach = [](QWidget *widget) {
		if (!Core::IsAppLaunched() || !widget
			|| widget->property("xenon-text-animation-attached").toBool()) return;
		if (const auto edit = qobject_cast<QTextEdit*>(widget)) {
			if (edit->isReadOnly()) return;
			for (auto parent = edit->parentWidget(); parent; parent = parent->parentWidget()) {
				const auto field = dynamic_cast<InputField*>(parent);
				if (field && field->rawTextEdit() == edit) {
					Attach(field);
					return;
				}
			}
			edit->setProperty("xenon-text-animation-attached", true);
			new InputAnimation(edit, edit);
		} else if (const auto line = qobject_cast<QLineEdit*>(widget)) {
			if (line->isReadOnly()) return;
			line->setProperty("xenon-text-animation-attached", true);
			new InputAnimation(line, nullptr, line);
		}
	};
	QObject::connect(qApp, &QApplication::focusChanged, qApp,
		[attach](QWidget *, QWidget *now) { attach(now); });
	attach(QApplication::focusWidget());
}

}
