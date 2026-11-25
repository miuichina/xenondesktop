// Message Shot is based on AyuGram Desktop, copyright Radolyn, 2026.
// Distributed under the GNU General Public License, version 3.
#include "history/view/history_view_message_shot.h"

#include "base/algorithm.h"
#include "base/flat_map.h"
#include "base/timer.h"
#include "mtproto/sender.h"
#include "core/file_utilities.h"
#include "core/file_location.h"
#include "data/data_cloud_themes.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_forum.h"
#include "data/data_groups.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "dialogs/ui/dialogs_video_userpic.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/view/history_view_element.h"
#include "history/view/media/history_view_media.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "settings/settings_common.h"
#include "storage/storage_account.h"
#include "styles/style_chat.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "ui/chat/chat_style.h"
#include "ui/chat/chat_theme.h"
#include "ui/effects/path_shift_gradient.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/userpic_view.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/themes/window_theme_preview.h"
#include "window/themes/window_themes_embedded.h"
#include "window/window_session_controller.h"

#include <QtCore/QDataStream>
#include <QtCore/QIODevice>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QColorDialog>

#include <cmath>

namespace HistoryView::MessageShot {
namespace {

constexpr auto kEnabledPref = "messageShotEnabled";
constexpr auto kOptionsPref = "messageShotOptions";
constexpr auto kShotPadding = 4;
constexpr auto kPreviewHeight = 300;
constexpr auto kMaxPixels = 64'000'000.;
constexpr auto kMediaTimeout = crl::time(3000);

using EmbeddedType = Window::Theme::EmbeddedType;

struct Options {
	bool background = true;
	bool date = false;
	bool reactions = false;
	bool decorations = true;
	bool colorfulReplies = true;
	bool revealSpoilers = true;
	int embedded = -1;
	QColor accent;
	Data::CloudTheme cloud;
};

const Options *ActiveOptions = nullptr;
rpl::event_stream<Main::Session*> EnabledUpdates;

Options ReadOptions(not_null<Main::Session*> session) {
	const auto bytes = session->local().readPref<QByteArray>(
		kOptionsPref,
		QByteArray());
	if (bytes.isEmpty()) {
		return {};
	}
	auto stream = QDataStream(bytes);
	stream.setVersion(QDataStream::Qt_5_1);
	auto result = Options();
	auto embedded = qint32();
	auto id = quint64();
	auto hash = quint64();
	auto document = quint64();
	stream >> result.background >> result.date >> result.reactions
		>> result.decorations >> result.colorfulReplies >> result.revealSpoilers
		>> embedded >> result.accent >> id >> hash >> document
		>> result.cloud.title;
	if (stream.status() != QDataStream::Ok) {
		return {};
	}
	if (embedded >= int(EmbeddedType::DayBlue)
		&& embedded <= int(EmbeddedType::NightGreen)) {
		result.embedded = embedded;
	}
	result.cloud.id = id;
	result.cloud.accessHash = hash;
	result.cloud.documentId = document;
	return result;
}

void WriteOptions(not_null<Main::Session*> session, const Options &options) {
	auto bytes = QByteArray();
	auto stream = QDataStream(&bytes, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << options.background << options.date << options.reactions
		<< options.decorations << options.colorfulReplies << options.revealSpoilers
		<< qint32(options.embedded) << options.accent
		<< quint64(options.cloud.id) << quint64(options.cloud.accessHash)
		<< quint64(options.cloud.documentId) << options.cloud.title;
	session->local().writePref<QByteArray>(kOptionsPref, std::move(bytes));
}

HistoryItemsList Resolve(
		not_null<Main::Session*> session,
		const MessageIdsList &ids) {
	auto result = HistoryItemsList();
	auto seen = base::flat_set<FullMsgId>();
	result.reserve(ids.size());
	for (const auto id : ids) {
		auto item = session->data().message(id);
		if (!item) {
			continue;
		}
		if (const auto group = session->data().groups().find(item)) {
			item = group->items.front();
		}
		if (seen.insert(item->fullId()).second) {
			result.push_back(item);
		}
	}
	return result;
}

class ShotDelegate final : public DefaultElementDelegate {
public:
	explicit ShotDelegate(not_null<const Ui::ChatStyle*> style)
	: _gradient(MakePathShiftGradient(style, [] {})) {
	}

	bool elementAnimationsPaused() override {
		return true;
	}
	not_null<Ui::PathShiftGradient*> elementPathShiftGradient() override {
		return _gradient.get();
	}
	Context elementContext() override {
		return Context::AdminLog;
	}
	bool elementHideReply(not_null<const Element*> view) override {
		const auto history = view->data()->history();
		const auto reply = view->data()->Get<HistoryMessageReply>();
		if (!reply
			|| reply->fields().manualQuote
			|| (reply->externalPeerId()
				&& reply->externalPeerId() != history->peer->id)) {
			return false;
		}
		const auto forum = history->asForum();
		return forum && forum->topicFor(reply->messageId());
	}
	ElementChatMode elementChatMode() override {
		return ElementChatMode::Wide;
	}

private:
	const std::unique_ptr<Ui::PathShiftGradient> _gradient;

};

QRect ContentBounds(const QImage &image) {
	auto left = image.width();
	auto top = image.height();
	auto right = -1;
	auto bottom = -1;
	for (auto y = 0; y < image.height(); ++y) {
		const auto line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
		for (auto x = 0; x < image.width(); ++x) {
			if (qAlpha(line[x])) {
				left = std::min(left, x);
				top = std::min(top, y);
				right = std::max(right, x);
				bottom = y;
			}
		}
	}
	return (right >= left)
		? QRect(QPoint(left, top), QPoint(right, bottom))
		: QRect();
}

QImage Render(
		not_null<Window::SessionController*> controller,
		const MessageIdsList &ids,
		const Options &options,
		const style::palette *palette) {
	const auto messages = Resolve(&controller->session(), ids);
	if (messages.empty()) {
		return {};
	}
	const auto previous = std::exchange(ActiveOptions, &options);
	const auto reset = gsl::finally([&] { ActiveOptions = previous; });
	const auto isolated = palette
		? std::make_unique<Ui::ChatStyle>(palette)
		: nullptr;
	const auto style = isolated
		? not_null<const Ui::ChatStyle*>(isolated.get())
		: controller->chatStyle();
	auto delegate = ShotDelegate(style);
	auto views = std::vector<std::unique_ptr<Element>>();
	views.reserve(messages.size());

	const auto width = st::msgMaxWidth
		+ st::boxPadding.left()
		+ st::boxPadding.right();
	auto prior = (Element*)nullptr;
	for (const auto item : messages) {
		auto view = item->createView(&delegate);
		if (view->isHidden()) {
			continue;
		}
		const auto sameHistory = prior && prior->data()->history() == item->history();
		const auto sameDate = prior
			&& view->dateTime().date() == prior->dateTime().date();
		view->setDisplayDate(options.date && prior && !sameDate);
		if (sameHistory && sameDate && view->computeIsAttachToPrevious(prior)) {
			view->setAttachToPrevious(true, prior);
			prior->setAttachToNext(true, view.get());
		}
		prior = view.get();
		views.push_back(std::move(view));
	}
	auto height = 0;
	for (const auto &view : views) {
		height += view->resizeGetHeight(width);
		if (options.revealSpoilers) {
			view->revealSpoilers();
		}
	}
	if (height <= 0) {
		return {};
	}
	const auto pixelRatio = std::min(
		qreal(::style::DevicePixelRatio()),
		std::sqrt(kMaxPixels / (double(width) * height)));
	auto image = QImage(
		QSize(int(std::ceil(width * pixelRatio)), int(std::ceil(height * pixelRatio))),
		QImage::Format_ARGB32_Premultiplied);
	if (image.isNull()) {
		return {};
	}
	image.setDevicePixelRatio(pixelRatio);
	image.fill(Qt::transparent);

	{
		auto painter = Painter(&image);
		auto userpics = base::flat_map<not_null<PeerData*>, Ui::PeerUserpicView>();
		auto y = 0;
		for (const auto &view : views) {
			const auto rect = QRect(0, 0, width, view->height());
			const auto viewport = QRect(0, -y, width, height);
			auto context = controller->defaultChatTheme()->preparePaintContext(
				style,
				viewport,
				rect,
				rect,
				true);
			if (isolated) {
				context.bubblesPattern = nullptr;
				context.bubblesPatternAverage = {};
				context.backgroundAverage = {};
			}
			painter.translate(0, y);
			view->draw(painter, context);
			const auto message = view->data();
			if (view->displayFromPhoto() || message->isPost()) {
				const auto photoX = st::msgMargin.left();
				const auto photoY = view->height()
					- view->marginBottom()
					- st::msgPhotoSize;
				if (const auto from = message->displayFrom()) {
					Dialogs::Ui::PaintUserpic(
						painter, from, nullptr, userpics[from],
						photoX, photoY, width, st::msgPhotoSize, true);
				} else if (const auto info = message->displayHiddenSenderInfo()) {
					info->emptyUserpic.paintCircle(
						painter, photoX, photoY, width, st::msgPhotoSize);
				}
			}
			painter.translate(0, -y);
			y += view->height();
		}
	}
	const auto bounds = ContentBounds(image);
	if (bounds.isEmpty()) {
		return {};
	}
	auto cropped = image.copy(bounds);
	image = QImage();
	cropped.setDevicePixelRatio(pixelRatio);
	const auto padding = std::max(1, ::style::ConvertScale(kShotPadding));
	const auto physicalPadding = int(std::ceil(padding * pixelRatio));
	auto result = QImage(
		cropped.size() + QSize(2 * physicalPadding, 2 * physicalPadding),
		QImage::Format_ARGB32_Premultiplied);
	if (result.isNull()) {
		return {};
	}
	result.setDevicePixelRatio(pixelRatio);
	const auto background = style->boxBg()->c;
	result.fill(options.background
		? (style->dark() ? background.lighter(175) : background.darker(110))
		: QColor(Qt::transparent));
	{
		auto painter = QPainter(&result);
		painter.drawImage(QPointF(physicalPadding / pixelRatio,
			physicalPadding / pixelRatio), cropped);
	}
	return result;
}

class ShotBox final : public Ui::BoxContent {
public:
	ShotBox(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		MessageIdsList ids,
		Fn<void()> clearSelected);

private:
	void prepare() override;
	void preloadMedia();
	void updatePreview();
	void updatePalette();
	void loadCloudPalette(const Data::CloudTheme &cloud);
	void chooseTheme();
	void save();
	void copy();
	[[nodiscard]] QString themeName() const;

	const not_null<Window::SessionController*> _controller;
	const MessageIdsList _ids;
	MTP::Sender _api;
	mtpRequestId _themeRequest = 0;
	Fn<void()> _clearSelected;
	Options _options;
	QImage _image;
	Ui::RpWidget *_preview = nullptr;
	std::unique_ptr<style::palette> _palette;
	rpl::variable<QString> _themeName;
	rpl::lifetime _themeLoading;
	rpl::lifetime _mediaLoading;
	std::vector<std::shared_ptr<Data::PhotoMedia>> _photos;
	std::vector<std::shared_ptr<Data::DocumentMedia>> _documents;
	base::Timer _previewTimer;
	base::Timer _mediaTimer;
	bool _exported = false;

};

ShotBox::ShotBox(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	MessageIdsList ids,
	Fn<void()> clearSelected)
: _controller(controller)
, _ids(std::move(ids))
, _api(&controller->session().mtp())
, _clearSelected(std::move(clearSelected))
, _options(ReadOptions(&controller->session()))
, _previewTimer([=] { updatePreview(); })
, _mediaTimer([=] {
	_mediaLoading.destroy();
	updatePreview();
}) {
}

void ShotBox::prepare() {
	setTitle(tr::lng_message_shot());
	auto wrap = object_ptr<Ui::VerticalLayout>(this);
	const auto content = wrap.data();
	setInnerWidget(object_ptr<Ui::OverrideMargins>(this, std::move(wrap)));
	using namespace Settings;
	AddSubsectionTitle(content, tr::lng_message_shot_preview());
	_preview = content->add(
		object_ptr<Ui::RpWidget>(content),
		st::boxPadding);
	_preview->resize(0, ::style::ConvertScale(kPreviewHeight));
	_preview->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(_preview);
		if (_image.isNull()) {
			p.setPen(st::windowSubTextFg->c);
			p.drawText(_preview->rect(), Qt::AlignCenter,
				tr::lng_message_shot_empty(tr::now));
			return;
		}
		auto size = _image.size() / _image.devicePixelRatio();
		size = size.boundedTo(size.scaled(_preview->size(), Qt::KeepAspectRatio));
		const auto target = QRect(
			(_preview->width() - size.width()) / 2,
			(_preview->height() - size.height()) / 2,
			size.width(),
			size.height());
		p.fillRect(target, st::windowBgOver->c);
		if (!_options.background) {
			const auto tile = std::max(1, ::style::ConvertScale(8));
			p.save();
			p.setClipRect(target);
			for (auto y = 0; y * tile < target.height(); ++y) {
				for (auto x = y % 2; x * tile < target.width(); x += 2) {
					p.fillRect(target.x() + x * tile, target.y() + y * tile,
						tile, tile, st::boxBg->c);
				}
			}
			p.restore();
		}
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		p.drawImage(target, _image);
	}, _preview->lifetime());
	AddDivider(content);
	AddSkip(content);
	AddSubsectionTitle(content, tr::lng_message_shot_preferences());

	_themeName = themeName();
	AddButtonWithLabel(
		content,
		tr::lng_message_shot_theme(),
		_themeName.value(),
		st::settingsButtonNoIcon
	)->addClickHandler([=] { chooseTheme(); });

	const auto add = [&](rpl::producer<QString> title, bool Options::*field) {
		AddButtonWithIcon(content, std::move(title), st::settingsButtonNoIcon
		)->toggleOn(rpl::single(_options.*field))->toggledValue(
		) | rpl::skip(1) | rpl::on_next([=](bool enabled) {
			_options.*field = enabled;
			WriteOptions(&_controller->session(), _options);
			_previewTimer.callOnce(0);
		}, content->lifetime());
	};
	add(tr::lng_message_shot_background(), &Options::background);
	add(tr::lng_message_shot_date(), &Options::date);
	add(tr::lng_message_shot_reactions(), &Options::reactions);
	add(tr::lng_message_shot_decorations(), &Options::decorations);
	add(tr::lng_message_shot_replies(), &Options::colorfulReplies);
	add(tr::lng_message_shot_spoilers(), &Options::revealSpoilers);
	AddSkip(content);
	addButton(tr::lng_settings_save(), [=] { save(); });
	addButton(tr::lng_group_invite_context_copy(), [=] { copy(); });
	setDimensionsToContent(st::boxWideWidth, content);
	boxClosing() | rpl::on_next([=] {
		if (_exported) {
			_clearSelected();
		}
	}, lifetime());

	preloadMedia();
	updatePalette();
}

void ShotBox::preloadMedia() {
	const auto session = &_controller->session();
	auto items = Resolve(session, _ids);
	const auto size = items.size();
	for (auto i = size_t(0); i < size; ++i) {
		const auto item = items[i];
		if (const auto group = session->data().groups().find(item)) {
			for (auto j = size_t(1); j < group->items.size(); ++j) {
				items.push_back(group->items[j]);
			}
		}
	}
	for (const auto message : items) {
		const auto media = message->media();
		if (!media) {
			continue;
		}
		const auto origin = Data::FileOrigin(message->fullId());
		if (const auto photo = media->photo()) {
			const auto view = photo->createMediaView();
			if (!view->loaded()) {
				photo->load(origin, LoadFromCloudOrLocal, false);
			}
			_photos.push_back(view);
		} else if (const auto document = media->document(); document && document->hasThumbnail()) {
			const auto view = document->createMediaView();
			if (!view->thumbnail()) {
				document->loadThumbnail(origin);
			}
			_documents.push_back(view);
		}
	}
	if (!_photos.empty() || !_documents.empty()) {
		session->downloaderTaskFinished() | rpl::on_next([=] {
			_previewTimer.callOnce(50);
		}, _mediaLoading);
		_mediaTimer.callOnce(kMediaTimeout);
	}
}

void ShotBox::updatePreview() {
	_image = Render(_controller, _ids, _options, _palette.get());
	_preview->update();
}

void ShotBox::updatePalette() {
	using namespace Window::Theme;
	if (const auto request = base::take(_themeRequest)) {
		_api.request(request).cancel();
	}
	_themeLoading.destroy();
	_palette = nullptr;
	_themeName = themeName();
	if (_options.cloud.documentId) {
		const auto session = &_controller->session();
		const auto cloud = _options.cloud;
		const auto document = session->data().document(cloud.documentId);
		if (document->hasRemoteLocation()) {
			loadCloudPalette(cloud);
		} else if (cloud.id) {
			_themeRequest = _api.request(MTPaccount_GetTheme(
				MTP_string(Data::CloudThemes::Format()),
				MTP_inputTheme(MTP_long(cloud.id), MTP_long(cloud.accessHash))
			)).done([=](const MTPTheme &result) {
				_themeRequest = 0;
				const auto loaded = Data::CloudTheme::Parse(session, result);
				if (loaded.documentId) {
					loadCloudPalette(loaded);
				}
			}).fail([=] {
				_themeRequest = 0;
			}).send();
		}
	} else if (_options.embedded >= 0) {
		_palette = PrepareEmbeddedPalette(
			EmbeddedType(_options.embedded),
			_options.accent.isValid()
				? std::optional<QColor>(_options.accent)
				: std::nullopt);
	}
	updatePreview();
}

void ShotBox::loadCloudPalette(const Data::CloudTheme &cloud) {
	const auto session = &_controller->session();
	const auto document = session->data().document(cloud.documentId);
	const auto media = document->createMediaView();
	const auto apply = [=] {
		const auto preview = Window::Theme::PreviewFromFile(
			media->bytes(),
			document->location().name(),
			cloud);
		if (preview) {
			_palette = std::make_unique<style::palette>(
				preview->instance.palette);
			_previewTimer.callOnce(0);
		}
	};
	if (media->loaded()) {
		apply();
	} else {
		session->downloaderTaskFinished() | rpl::filter([=] {
			return media->loaded();
		}) | rpl::take(1) | rpl::on_next(apply, _themeLoading);
		document->save(
			Data::FileOriginTheme(cloud.id, cloud.accessHash),
			QString());
	}
}

QString ShotBox::themeName() const {
	if (_options.cloud.documentId && !_options.cloud.title.isEmpty()) {
		return _options.cloud.title;
	}
	for (auto &theme : Window::Theme::EmbeddedThemes()) {
		if (int(theme.type) == _options.embedded) {
			auto result = QString();
			auto lifetime = rpl::lifetime();
			std::move(theme.name) | rpl::take(1) | rpl::on_next(
				[&](QString text) { result = std::move(text); },
				lifetime);
			return result;
		}
	}
	return tr::lng_message_shot_theme_default(tr::now);
}

void ShotBox::chooseTheme() {
	using namespace Settings;
	using namespace Window::Theme;
	const auto choose = crl::guard(this, [=](
			int embedded, QColor color, Data::CloudTheme cloud) {
		_options.embedded = embedded;
		_options.accent = std::move(color);
		_options.cloud = std::move(cloud);
		WriteOptions(&_controller->session(), _options);
		updatePalette();
	});
	const auto accent = _options.accent;
	const auto embedded = _options.embedded;
	const auto clouds = &_controller->session().data().cloudThemes();
	_controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_message_shot_theme());
		const auto content = box->verticalLayout();
		AddButtonWithIcon(
			content,
			tr::lng_message_shot_theme_default(),
			st::settingsButtonNoIcon
		)->addClickHandler([=] {
			choose(-1, QColor(), Data::CloudTheme());
			box->closeBox();
		});
		for (auto &theme : EmbeddedThemes()) {
			const auto type = int(theme.type);
			const auto color = (type == embedded) ? accent : QColor();
			AddButtonWithIcon(
				content,
				std::move(theme.name),
				st::settingsButtonNoIcon
			)->addClickHandler([=] {
				choose(type, color, Data::CloudTheme());
				box->closeBox();
			});
		}
		if (embedded >= 0) {
			AddButtonWithIcon(
				content,
				tr::lng_message_shot_theme_accent(),
				st::settingsButtonNoIcon
			)->addClickHandler([=] {
				const auto color = QColorDialog::getColor(
					accent,
					box,
					tr::lng_message_shot_theme_accent(tr::now));
				if (color.isValid()) {
					choose(embedded, color, Data::CloudTheme());
					box->closeBox();
				}
			});
		}
		auto cloudRows = content->add(object_ptr<Ui::VerticalLayout>(content));
		const auto refreshClouds = [=] {
			cloudRows->clear();
			for (const auto &theme : clouds->list()) {
				if (!theme.documentId) {
					continue;
				}
				AddButtonWithIcon(
					cloudRows,
					rpl::single(theme.title),
					st::settingsButtonNoIcon
				)->addClickHandler([=] {
					choose(-1, QColor(), theme);
					box->closeBox();
				});
			}
		};
		clouds->updated() | rpl::on_next(refreshClouds, cloudRows->lifetime());
		refreshClouds();
		clouds->refresh();
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}), Ui::LayerOption::KeepOther);
}

void ShotBox::save() {
	if (_image.isNull()) {
		return;
	}
	FileDialog::GetWritePath(
		this,
		tr::lng_save_file(tr::now),
		u"PNG (*.png)"_q,
		u"shot.png"_q,
		crl::guard(this, [=, image = _image](QString &&path) {
			if (path.isEmpty()) {
				return;
			}
			if (!image.save(path, "PNG")) {
				_controller->showToast(tr::lng_message_shot_save_failed(tr::now));
				return;
			}
			_exported = true;
			closeBox();
		}));
}

void ShotBox::copy() {
	if (!_image.isNull()) {
		QGuiApplication::clipboard()->setImage(_image);
		_exported = true;
		closeBox();
	}
}

} // namespace

bool Rendering() {
	return ActiveOptions != nullptr;
}

bool Hide(Part part) {
	if (!ActiveOptions) {
		return false;
	}
	switch (part) {
	case Part::Date: return !ActiveOptions->date;
	case Part::Reactions: return !ActiveOptions->reactions;
	case Part::HeaderDecorations: return !ActiveOptions->decorations;
	case Part::ColorfulReplies: return !ActiveOptions->colorfulReplies;
	}
	return false;
}

bool Enabled(not_null<Main::Session*> session) {
	return session->local().readPref<bool>(kEnabledPref, true);
}

void SetEnabled(not_null<Main::Session*> session, bool enabled) {
	if (Enabled(session) != enabled) {
		session->local().writePref<bool>(kEnabledPref, enabled);
		EnabledUpdates.fire_copy(session.get());
	}
}

rpl::producer<> EnabledChanges(not_null<Main::Session*> session) {
	return EnabledUpdates.events() | rpl::filter([=](Main::Session *updated) {
		return updated == session.get();
	}) | rpl::to_empty;
}

void Show(
		not_null<Window::SessionController*> controller,
		MessageIdsList ids,
		Fn<void()> clearSelected) {
	if (!Resolve(&controller->session(), ids).empty()) {
		controller->show(Box<ShotBox>(
			controller,
			std::move(ids),
			std::move(clearSelected)));
	}
}

} // namespace HistoryView::MessageShot
