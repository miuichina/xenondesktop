/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_helpers/gifs_list_widget.h"

#include "api/api_toggling_media.h" // Api::ToggleSavedGif
#include "base/const_string.h"
#include "base/qt/qt_key_modifiers.h"
#include "chat_helpers/stickers_list_footer.h"
#include "data/data_photo.h"
#include "data/data_document.h"
#include "data/stickers/data_custom_emoji.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/data_file_origin.h"
#include "data/data_photo_media.h"
#include "data/data_document_media.h"
#include "data/stickers/data_stickers.h"
#include "menu/menu_send.h" // SendMenu::FillSendMenu
#include "mtproto/mtproto_config.h"
#include "core/click_handler_types.h"
#include "ui/boxes/confirm_box.h"
#include "ui/controls/tabbed_search.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "ui/text/text_utilities.h"
#include "ui/effects/ripple_animation.h"
#include "ui/image/image.h"
#include "ui/painter.h"
#include "boxes/stickers_box.h"
#include "inline_bots/inline_bot_result.h"
#include "storage/localstorage.h"
#include "lang/lang_keys.h"
#include "layout/layout_position.h"
#include "mainwindow.h"
#include "main/main_session.h"
#include "main/session/send_as_peers.h"
#include "window/window_session_controller.h"
#include "history/view/history_view_cursor_state.h"
#include "storage/storage_account.h" // Account::writeSavedGifs
#include "styles/style_chat_helpers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_media_player.h"
#include "styles/style_widgets.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>

#include <QtWidgets/QApplication>

namespace ChatHelpers {
namespace {

constexpr auto kSearchRequestDelay = 400;
constexpr auto kMinRepaintDelay = crl::time(33);
constexpr auto kMinAfterScrollDelay = crl::time(33);
constexpr auto kGifCollectionNameLimit = 64;
constexpr auto kCollectionScrollInterval = 16;

class GifCollectionRow final : public Ui::RpWidget {
public:
	GifCollectionRow(
		not_null<Ui::ScrollArea*> bar,
		Fn<void(uint32, uint32)> moved)
	: RpWidget(bar)
	, _bar(bar)
	, _moved(std::move(moved))
	, _scrollTimer([=] { scrollDuringDrag(); }) {
	}

	void addButton(uint32 id, not_null<Ui::RoundButton*> button) {
		const auto left = _buttons.empty()
			? 0
			: (_buttons.back().left
				+ _buttons.back().button->width()
				+ st::gifCollectionButtonSkip);
		_buttons.push_back({ button, id, left });
		button->move(left, 0);
		button->show();
		if (id) {
			button->installEventFilter(this);
		}
		resize(left + button->width(), button->height());
	}

protected:
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	struct Entry {
		not_null<Ui::RoundButton*> button;
		uint32 id = 0;
		int left = 0;
	};

	void updateDrag();
	void finishDrag(bool apply);
	void scrollDuringDrag();

	const not_null<Ui::ScrollArea*> _bar;
	Fn<void(uint32, uint32)> _moved;
	std::vector<Entry> _buttons;
	base::Timer _scrollTimer;
	QPoint _pointer;
	int _pressX = 0;
	int _grabOffset = 0;
	int _source = -1;
	int _target = -1;
	bool _dragging = false;

};

bool GifCollectionRow::eventFilter(QObject *object, QEvent *event) {
	if (event->type() == QEvent::MouseButtonPress) {
		const auto mouse = static_cast<QMouseEvent*>(event);
		if (mouse->button() == Qt::LeftButton && _buttons.size() > 2) {
			const auto i = ranges::find(_buttons, object, [](const Entry &e) {
				return static_cast<QObject*>(e.button.get());
			});
			if (i != end(_buttons)) {
				_source = _target = int(i - begin(_buttons));
				_pressX = mouse->globalPosition().toPoint().x();
				_grabOffset = mouse->position().toPoint().x();
			}
		}
	} else if (_source >= 0 && object == _buttons[_source].button.get()) {
		if (event->type() == QEvent::MouseMove) {
			const auto mouse = static_cast<QMouseEvent*>(event);
			if (!(mouse->buttons() & Qt::LeftButton)) {
				finishDrag(false);
				return false;
			}
			_pointer = mouse->globalPosition().toPoint();
			if (!_dragging && std::abs(_pointer.x() - _pressX)
				>= QApplication::startDragDistance()) {
				_dragging = true;
				_buttons[_source].button->setSynteticOver(false);
				_buttons[_source].button->clearState();
				_buttons[_source].button->raise();
				_buttons.front().button->raise();
				if (_bar->scrollLeftMax()) {
					_scrollTimer.callEach(kCollectionScrollInterval);
				}
			}
			if (_dragging) {
				updateDrag();
				return true;
			}
		} else if (event->type() == QEvent::MouseButtonRelease) {
			const auto mouse = static_cast<QMouseEvent*>(event);
			if (mouse->button() == Qt::LeftButton) {
				const auto dragged = _dragging;
				if (dragged) {
					_pointer = mouse->globalPosition().toPoint();
					updateDrag();
				}
				finishDrag(dragged);
				return dragged;
			}
		} else if (event->type() == QEvent::UngrabMouse
			|| event->type() == QEvent::Hide) {
			finishDrag(false);
		}
	}
	return false;
}

void GifCollectionRow::updateDrag() {
	const auto &dragged = _buttons[_source];
	const auto buttonWidth = dragged.button->width();
	const auto pointerX = mapFromGlobal(_pointer).x();
	const auto left = std::clamp(
		pointerX - _grabOffset,
		_buttons[1].left,
		width() - buttonWidth);
	const auto midpoint = [](const Entry &entry) {
		return entry.left + entry.button->width() / 2;
	};
	const auto count = int(_buttons.size());
	_target = _source;
	while (_target > 1 && pointerX < midpoint(_buttons[_target - 1])) {
		--_target;
	}
	while (_target + 1 < count && pointerX > midpoint(_buttons[_target + 1])) {
		++_target;
	}
	const auto shift = buttonWidth + st::gifCollectionButtonSkip;
	for (auto i = 1; i < count; ++i) {
		const auto &entry = _buttons[i];
		const auto x = (i == _source)
			? left
			: (i >= _target && i < _source)
			? (entry.left + shift)
			: (i > _source && i <= _target)
			? (entry.left - shift)
			: entry.left;
		entry.button->move(x, 0);
	}
}

void GifCollectionRow::finishDrag(bool apply) {
	_scrollTimer.cancel();
	if (_dragging) {
		for (const auto &entry : _buttons) {
			entry.button->move(entry.left, 0);
		}
	}
	const auto from = std::exchange(_source, -1);
	const auto to = std::exchange(_target, -1);
	_dragging = false;
	if (apply && from != to) {
		_moved(_buttons[from].id, _buttons[to].id);
	}
}

void GifCollectionRow::scrollDuringDrag() {
	const auto viewport = _bar->viewport();
	const auto x = viewport->mapFromGlobal(_pointer).x();
	const auto edge = std::min(height(), viewport->width() / 3);
	const auto delta = (x < edge)
		? (x - edge)
		: std::max(x - viewport->width() + edge, 0);
	if (delta) {
		const auto old = _bar->scrollLeft();
		const auto direction = (_bar->layoutDirection() == Qt::RightToLeft)
			? -1
			: 1;
		const auto speed = std::clamp(delta, -edge, edge) / 4;
		_bar->scrollToX(old + direction * (speed + (delta > 0 ? 1 : -1)));
		if (old != _bar->scrollLeft()) {
			updateDrag();
		}
	}
}

} // namespace

void AddGifAction(
		Fn<void(QString, Fn<void()> &&, const style::icon*)> callback,
		std::shared_ptr<Show> show,
		not_null<DocumentData*> document,
		const style::ComposeIcons *iconsOverride) {
	if (!document->isGifv()) {
		return;
	}
	auto &data = document->owner();
	const auto index = data.stickers().savedGifs().indexOf(document);
	const auto saved = (index >= 0);
	const auto text = (saved
		? tr::lng_context_delete_gif
		: tr::lng_context_save_gif)(tr::now);
	const auto &icons = iconsOverride
		? *iconsOverride
		: st::defaultComposeIcons;
	callback(text, [=] {
		Api::ToggleSavedGif(
			show,
			document,
			Data::FileOriginSavedGifs(),
			!saved);

		const auto &data = document->owner();
		if (saved) {
			data.stickers().savedGifsRef().remove(index);
			document->session().local().writeSavedGifs();
		}
		data.stickers().notifySavedGifsUpdated();
	}, saved ? &icons.menuGifRemove : &icons.menuGifAdd);
}

GifsListWidget::GifsListWidget(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	PauseReason level)
: GifsListWidget(parent, {
	.show = controller->uiShow(),
	.paused = Window::PausedIn(controller, level),
}) {
}

GifsListWidget::GifsListWidget(
	QWidget *parent,
	GifsListDescriptor &&descriptor)
: Inner(
	parent,
	descriptor.st ? *descriptor.st : st::defaultEmojiPan,
	descriptor.show,
	descriptor.paused)
, _show(std::move(descriptor.show))
, _chooseBeforeLoad(descriptor.chooseBeforeLoad)
, _api(&session().mtp())
, _section(Section::Gifs)
, _updateInlineItems([=] { updateInlineItems(); })
, _mosaic(st::emojiPanWidth - st::inlineResultsLeft)
, _previewTimer([=] { showPreview(); }) {
	setInlineQueryPeer(descriptor.inlineQueryPeer);
	setMouseTracking(true);
	setAttribute(Qt::WA_OpaquePaintEvent);

	setupSearch();

	_inlineRequestTimer.setSingleShot(true);
	connect(
		&_inlineRequestTimer,
		&QTimer::timeout,
		this,
		[=] { sendInlineRequest(); });

	session().data().stickers().savedGifsUpdated(
	) | rpl::on_next([=] {
		refreshSavedGifs();
	}, lifetime());

	session().downloaderTaskFinished(
	) | rpl::on_next([=] {
		updateInlineItems();
	}, lifetime());

	_show->pauseChanged(
	) | rpl::on_next([=] {
		if (!paused()) {
			updateInlineItems();
		}
	}, lifetime());

	sizeValue(
	) | rpl::on_next([=](const QSize &s) {
		_mosaic.setFullWidth(s.width());
	}, lifetime());

	_mosaic.setRightSkip(st::inlineResultsSkip);
	setupCollections();
}

rpl::producer<FileChosen> GifsListWidget::fileChosen() const {
	return _fileChosen.events();
}

rpl::producer<PhotoChosen> GifsListWidget::photoChosen() const {
	return _photoChosen.events();
}

auto GifsListWidget::inlineResultChosen() const
-> rpl::producer<InlineChosen> {
	return _inlineResultChosen.events();
}

object_ptr<TabbedSelector::InnerFooter> GifsListWidget::createFooter() {
	Expects(_footer == nullptr);

	using FooterDescriptor = StickersListFooter::Descriptor;
	auto result = object_ptr<StickersListFooter>(FooterDescriptor{
		.session = &session(),
		.paused = pausedMethod(),
		.parent = this,
		.st = &st(),
		.features = { .stickersSettings = false },
	});
	_footer = result;
	_chosenSetId = Data::Stickers::RecentSetId;

	GifSectionsValue(
		&session()
	) | rpl::on_next([=](std::vector<GifSection> &&list) {
		_sections = std::move(list);
		refreshIcons();
	}, _footer->lifetime());

	_footer->setChosen(
	) | rpl::on_next([=](uint64 setId) {
		_search->cancel();
		_collectionId = 0;
		refreshCollectionButtons();
		_chosenSetId = setId;
		refreshIcons();
		const auto i = ranges::find(_sections, setId, [](GifSection value) {
			return value.document->id;
		});
		searchForGifs((i != end(_sections)) ? i->emoji->text() : QString());
	}, _footer->lifetime());

	return result;
}

void GifsListWidget::refreshIcons() {
	if (_footer) {
		_footer->refreshIcons(
			fillIcons(),
			_chosenSetId,
			nullptr,
			ValidateIconAnimations::None);
	}
}

std::vector<StickerIcon> GifsListWidget::fillIcons() {
	auto result = std::vector<StickerIcon>();
	result.reserve(_sections.size() + 1);
	result.emplace_back(Data::Stickers::RecentSetId);
	const auto side = StickersListFooter::IconFrameSize();
	for (const auto &section : _sections) {
		const auto s = section.document;
		const auto id = s->id;
		const auto size = s->hasThumbnail()
			? QSize(
				s->thumbnailLocation().width(),
				s->thumbnailLocation().height())
			: QSize();
		const auto pix = size.scaled(side, side, Qt::KeepAspectRatio);
		const auto owner = &s->owner();
		const auto already = _fakeSets.find(id);
		const auto set = (already != end(_fakeSets))
			? already
			: _fakeSets.emplace(
				id,
				std::make_unique<Data::StickersSet>(
					owner,
					id,
					0,
					0,
					QString(),
					QString(),
					0,
					Data::StickersSetFlag::Special,
					0)).first;
		result.emplace_back(set->second.get(), s, pix.width(), pix.height());
	}
	return result;
}

void GifsListWidget::visibleTopBottomUpdated(
		int visibleTop,
		int visibleBottom) {
	const auto top = getVisibleTop();
	Inner::visibleTopBottomUpdated(visibleTop, visibleBottom);
	if (top != getVisibleTop()) {
		_lastScrolledAt = crl::now();
		update();
	}
	checkLoadMore();
}

void GifsListWidget::checkLoadMore() {
	auto visibleHeight = (getVisibleBottom() - getVisibleTop());
	if (getVisibleBottom() + visibleHeight > height()) {
		sendInlineRequest();
	}
}

int GifsListWidget::countDesiredHeight(int newWidth) {
	return _mosaic.countDesiredHeight(newWidth);
}

GifsListWidget::~GifsListWidget() {
	clearInlineRows(true);
	deleteUnusedGifLayouts();
	deleteUnusedInlineLayouts();
}

void GifsListWidget::cancelGifsSearch() {
	_search->setLoading(false);
	if (_inlineRequestId) {
		_api.request(_inlineRequestId).cancel();
		_inlineRequestId = 0;
	}
	_inlineRequestTimer.stop();
	_inlineQuery = _inlineNextQuery = _inlineNextOffset = QString();
	_inlineCache.clear();
	refreshInlineRows(nullptr, true);
}

void GifsListWidget::inlineResultsDone(const MTPmessages_BotResults &result) {
	_search->setLoading(false);
	_inlineRequestId = 0;

	auto it = _inlineCache.find(_inlineQuery);
	auto adding = (it != _inlineCache.cend());
	if (result.type() == mtpc_messages_botResults) {
		auto &d = result.c_messages_botResults();
		session().data().processUsers(d.vusers());

		auto &v = d.vresults().v;
		auto queryId = d.vquery_id().v;

		if (it == _inlineCache.cend()) {
			it = _inlineCache.emplace(
				_inlineQuery,
				std::make_unique<InlineCacheEntry>()).first;
		}
		const auto entry = it->second.get();
		entry->nextOffset = qs(d.vnext_offset().value_or_empty());
		if (const auto count = v.size()) {
			entry->results.reserve(entry->results.size() + count);
		}
		auto added = 0;
		for (const auto &res : v) {
			const auto widget = res.match([](const auto &data) {
				return (data.vsend_message().type()
					== mtpc_botInlineMessageWidget);
			});
			if (widget) {
				// GIF selection needs a media rect that widget rows lack.
				continue;
			}
			auto result = InlineBots::Result::Create(
				&session(),
				queryId,
				res);
			if (result) {
				++added;
				entry->results.push_back(std::move(result));
			}
		}

		if (!added) {
			entry->nextOffset = QString();
		}
	} else if (adding) {
		it->second->nextOffset = QString();
	}

	if (!showInlineRows(!adding)) {
		it->second->nextOffset = QString();
	}
	checkLoadMore();
}

void GifsListWidget::paintEvent(QPaintEvent *e) {
	Painter p(this);
	auto clip = e->rect();
	p.fillRect(clip, st().bg);

	paintInlineItems(p, clip);
}

void GifsListWidget::paintInlineItems(Painter &p, QRect clip) {
	if (_mosaic.empty()) {
		p.setFont(st::normalFont);
		p.setPen(st::noContactsColor);
		const auto text = _collectionId
			? tr::lng_gif_collections_empty(tr::now)
			: _inlineQuery.isEmpty()
			? tr::lng_gifs_no_saved(tr::now)
			: tr::lng_inline_bot_no_results(tr::now);
		p.drawText(QRect(0, 0, width(), (height() / 3) * 2 + st::normalFont->height), text, style::al_center);
		return;
	}
	const auto gifPaused = paused();
	using namespace InlineBots::Layout;
	PaintContext context(crl::now(), false, gifPaused, false);

	auto paintItem = [&](not_null<const ItemBase*> item, QPoint point) {
		p.translate(point.x(), point.y());
		item->paint(
			p,
			clip.translated(-point),
			&context);
		p.translate(-point.x(), -point.y());
	};
	_mosaic.paint(std::move(paintItem), clip);
}

void GifsListWidget::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_lastMousePos = e->globalPos();
	updateSelected();

	_pressed = _selected;
	ClickHandler::pressed();
	_previewTimer.callOnce(QApplication::startDragTime());
}

base::unique_qptr<Ui::PopupMenu> GifsListWidget::fillContextMenu(
		const SendMenu::Details &details) {
	if (_selected < 0 || _pressed >= 0) {
		return nullptr;
	}

	auto menu = base::make_unique_q<Ui::PopupMenu>(this, st().menu);
	const auto selected = _selected;
	const auto send = crl::guard(this, [=](Api::SendOptions options) {
		selectInlineResult(selected, options, true);
	});
	const auto item = _mosaic.maybeItemAt(_selected);
	const auto isInlineResult = !item->getPhoto()
		&& !item->getDocument()
		&& item->getResult();
	const auto icons = &st().icons;
	auto copyDetails = details;
	if (isInlineResult) {
		// inline results don't have effects
		copyDetails.effectAllowed = false;
	}

	// In case we're adding items after FillSendMenu we have
	// to pass nullptr for showForEffect and attach selector later.
	// Otherwise added items widths won't be respected in menu geometry.
	SendMenu::FillSendMenu(
		menu,
		nullptr, // showForMenu
		copyDetails,
		SendMenu::DefaultCallback(_show, send),
		icons);

	if (!isInlineResult && _inlineQueryPeer) {
		menu->addAction(
			tr::lng_send_gif_with_caption(tr::now),
			crl::guard(this, [=] {
				selectInlineResult(selected, {}, true, true);
			}),
			&icons->menuGifCaption);
	}

	if (const auto item = _mosaic.maybeItemAt(_selected)) {
		const auto document = item->getDocument()
			? item->getDocument() // Saved GIF.
			: item->getPreviewDocument(); // Searched GIF.
		if (document) {
			auto callback = [&](
					const QString &text,
					Fn<void()> &&done,
					const style::icon *icon) {
				menu->addAction(text, std::move(done), icon);
			};
			AddGifAction(std::move(callback), _show, document, icons);
			if (document->isGifv() && document->hasRemoteLocation()) {
				addCollectionMenu(menu.get(), document);
			}
		}
	}

	SendMenu::AttachSendMenuEffect(
		menu,
		_show,
		copyDetails,
		SendMenu::DefaultCallback(_show, send));

	return menu;
}

void GifsListWidget::mouseReleaseEvent(QMouseEvent *e) {
	_previewTimer.cancel();

	auto pressed = std::exchange(_pressed, -1);
	auto activated = ClickHandler::unpressed();

	if (_previewShown) {
		_previewShown = false;
		return;
	}

	_lastMousePos = e->globalPos();
	updateSelected();

	if (_selected < 0 || _selected != pressed || !activated) {
		return;
	}

	if (dynamic_cast<InlineBots::Layout::SendClickHandler*>(activated.get())) {
		selectInlineResult(_selected, {});
	} else {
		ActivateClickHandler(window(), activated, {
			e->button(),
			QVariant::fromValue(ClickHandlerContext{
				.show = _show,
			})
		});
	}
}

void GifsListWidget::selectInlineResult(
		int index,
		Api::SendOptions options,
		bool forceSend,
		bool needsCaption) {
	const auto item = _mosaic.maybeItemAt(index);
	if (!item) {
		return;
	}

	const auto messageSendingFrom = [&] {
		if (options.scheduled) {
			return Ui::MessageSendingAnimationFrom();
		}
		const auto rect = item->innerContentRect().translated(
			_mosaic.findRect(index).topLeft());
		return Ui::MessageSendingAnimationFrom{
			.type = Ui::MessageSendingAnimationFrom::Type::Gif,
			.localId = session().data().nextLocalMessageId(),
			.globalStartGeometry = mapToGlobal(rect),
			.crop = true,
		};
	};

	forceSend |= _chooseBeforeLoad || base::IsCtrlPressed();
	if (const auto photo = item->getPhoto()) {
		using Data::PhotoSize;
		const auto media = photo->activeMediaView();
		if (forceSend
			|| (media && media->image(PhotoSize::Thumbnail))
			|| (media && media->image(PhotoSize::Large))) {
			_photoChosen.fire({
				.photo = photo,
				.options = options
			});
		} else if (!photo->loading(PhotoSize::Thumbnail)) {
			photo->load(PhotoSize::Thumbnail, Data::FileOrigin());
		}
	} else if (const auto document = item->getDocument()) {
		const auto media = document->activeMediaView();
		const auto preview = Data::VideoPreviewState(media.get());
		if (forceSend || (media && preview.loaded())) {
			_fileChosen.fire({
				.document = document,
				.options = options,
				.messageSendingFrom = messageSendingFrom(),
				.needsCaption = needsCaption,
			});
		} else if (!preview.usingThumbnail()) {
			if (preview.loading()) {
				document->cancel();
			} else {
				document->save(
					document->stickerOrGifOrigin(),
					QString());
			}
		}
	} else if (const auto inlineResult = item->getResult()) {
		if (_chooseBeforeLoad || inlineResult->onChoose(item)) {
			options.hideViaBot = true;
			_inlineResultChosen.fire({
				.result = inlineResult,
				.bot = _searchBot,
				.options = options,
				.messageSendingFrom = messageSendingFrom(),
			});
		}
	}
}

void GifsListWidget::mouseMoveEvent(QMouseEvent *e) {
	_lastMousePos = e->globalPos();
	updateSelected();
}

void GifsListWidget::leaveEventHook(QEvent *e) {
	clearSelection();
}

void GifsListWidget::leaveToChildEvent(QEvent *e, QWidget *child) {
	clearSelection();
}

void GifsListWidget::enterFromChildEvent(QEvent *e, QWidget *child) {
	_lastMousePos = QCursor::pos();
	updateSelected();
}

void GifsListWidget::clearSelection() {
	if (_selected >= 0) {
		ClickHandler::clearActive(_mosaic.itemAt(_selected));
		setCursor(style::cur_default);
	}
	_selected = _pressed = -1;
	repaintItems();
}

TabbedSelector::InnerFooter *GifsListWidget::getFooter() const {
	return _footer;
}

void GifsListWidget::processHideFinished() {
	clearSelection();
	clearHeavyData();
	if (_footer) {
		_footer->clearHeavyData();
	}
}

void GifsListWidget::processPanelHideFinished() {
	clearHeavyData();
	if (_footer) {
		_footer->clearHeavyData();
	}
}

void GifsListWidget::clearHeavyData() {
	// Preserve panel state through visibility toggles.
	//clearInlineRows(false);
	for (const auto &[document, layout] : _gifLayouts) {
		layout->unloadHeavyPart();
	}
	for (const auto &[document, layout] : _inlineLayouts) {
		layout->unloadHeavyPart();
	}
}

void GifsListWidget::refreshSavedGifs() {
	if (_section == Section::Gifs) {
		clearInlineRows(false);

		auto &stickers = session().data().stickers();
		const auto addGifs = [&](const auto &gifs) {
			if (gifs.empty()) {
				return;
			}
			const auto layouts = gifs
				| ranges::views::transform([&](not_null<DocumentData*> gif) {
					return layoutPrepareSavedGif(gif);
				}) | ranges::views::filter([](const LayoutItem *item) {
					return item != nullptr;
				}) | ranges::to<std::vector<not_null<LayoutItem*>>>;
			_mosaic.addItems(layouts);
		};
		if (const auto collection = stickers.gifCollection(_collectionId)) {
			addGifs(collection->gifs);
		} else {
			addGifs(stickers.savedGifs());
		}
		deleteUnusedGifLayouts();

		resizeToWidth(width());
		repaintItems();
	}

	if (isVisible()) {
		updateSelected();
	} else {
		preloadImages();
	}
}

void GifsListWidget::clearInlineRows(bool resultsDeleted) {
	if (resultsDeleted) {
		_selected = _pressed = -1;
	} else {
		clearSelection();
	}
	_mosaic.clearRows(resultsDeleted);
}

GifsListWidget::LayoutItem *GifsListWidget::layoutPrepareSavedGif(
		not_null<DocumentData*> document) {
	auto it = _gifLayouts.find(document);
	if (it == _gifLayouts.cend()) {
		if (auto layout = LayoutItem::createLayoutGif(this, document)) {
			it = _gifLayouts.emplace(document, std::move(layout)).first;
			it->second->initDimensions();
		} else {
			return nullptr;
		}
	}
	if (!it->second->maxWidth()) return nullptr;

	return it->second.get();
}

GifsListWidget::LayoutItem *GifsListWidget::layoutPrepareInlineResult(
		std::shared_ptr<InlineResult> result) {
	const auto raw = result.get();
	auto it = _inlineLayouts.find(raw);
	if (it == _inlineLayouts.cend()) {
		if (auto layout = LayoutItem::createLayout(
				this,
				std::move(result),
				_inlineWithThumb)) {
			it = _inlineLayouts.emplace(raw, std::move(layout)).first;
			it->second->initDimensions();
		} else {
			return nullptr;
		}
	}
	if (!it->second->maxWidth()) return nullptr;

	return it->second.get();
}

void GifsListWidget::deleteUnusedGifLayouts() {
	if (_mosaic.empty() || _section != Section::Gifs) { // delete all
		_gifLayouts.clear();
	} else {
		for (auto i = _gifLayouts.begin(); i != _gifLayouts.cend();) {
			if (i->second->position() < 0) {
				i = _gifLayouts.erase(i);
			} else {
				++i;
			}
		}
	}
}

void GifsListWidget::deleteUnusedInlineLayouts() {
	if (_mosaic.empty() || _section == Section::Gifs) { // delete all
		_inlineLayouts.clear();
	} else {
		for (auto i = _inlineLayouts.begin(); i != _inlineLayouts.cend();) {
			if (i->second->position() < 0) {
				i = _inlineLayouts.erase(i);
			} else {
				++i;
			}
		}
	}
}

void GifsListWidget::preloadImages() {
	_mosaic.forEach([](not_null<const LayoutItem*> item) {
		item->preload();
	});
}

void GifsListWidget::switchToSavedGifs() {
	clearInlineRows(false);
	_section = Section::Gifs;
	refreshSavedGifs();
	scrollTo(0);
}

int GifsListWidget::refreshInlineRows(const InlineCacheEntry *entry, bool resultsDeleted) {
	if (!entry) {
		if (resultsDeleted) {
			clearInlineRows(_section == Section::Inlines);
			deleteUnusedInlineLayouts();
		}
		switchToSavedGifs();
		return 0;
	}

	clearSelection();

	_section = Section::Inlines;
	const auto count = int(entry->results.size());
	const auto from = validateExistingInlineRows(entry->results);
	auto added = 0;
	if (count) {
		const auto resultLayouts = entry->results | ranges::views::slice(
			from,
			count
		) | ranges::views::transform([&](
				const std::shared_ptr<InlineBots::Result> &r) {
			return layoutPrepareInlineResult(r);
		}) | ranges::views::filter([](const LayoutItem *item) {
			return item != nullptr;
		}) | ranges::to<std::vector<not_null<LayoutItem*>>>;

		_mosaic.addItems(resultLayouts);
		added = resultLayouts.size();
		preloadImages();
	}

	resizeToWidth(width());
	repaintItems();

	_lastMousePos = QCursor::pos();
	updateSelected();

	return added;
}

int GifsListWidget::validateExistingInlineRows(const InlineResults &results) {
	const auto until = _mosaic.validateExistingRows([&](
			not_null<const LayoutItem*> item,
			int untilIndex) {
		return item->getResult().get() != results[untilIndex].get();
	}, results.size());

	if (_mosaic.empty()) {
		_inlineWithThumb = false;
		for (int i = until; i < results.size(); ++i) {
			if (results.at(i)->hasThumbDisplay()) {
				_inlineWithThumb = true;
				break;
			}
		}
	}
	return until;
}

void GifsListWidget::inlineItemLayoutChanged(const InlineBots::Layout::ItemBase *layout) {
	if (_selected < 0 || !isVisible()) {
		return;
	}

	if (const auto item = _mosaic.maybeItemAt(_selected)) {
		if (layout == item) {
			updateSelected();
		}
	}
}

void GifsListWidget::inlineItemRepaint(
		const InlineBots::Layout::ItemBase *layout) {
	updateInlineItems();
}

bool GifsListWidget::inlineItemVisible(
		const InlineBots::Layout::ItemBase *layout) {
	const auto position = layout->position();
	if (position < 0 || !isVisible()) {
		return false;
	}

	const auto &[row, column] = Layout::IndexToPosition(position);
	if (_mosaic.maybeItemAt(row, column) != layout) {
		return false;
	}

	auto top = 0;
	for (auto i = 0; i != row; ++i) {
		top += _mosaic.rowHeightAt(i);
	}

	return (top < getVisibleBottom())
		&& (top + layout->height() > getVisibleTop());
}

Data::FileOrigin GifsListWidget::inlineItemFileOrigin() {
	return _inlineQuery.isEmpty()
		? Data::FileOriginSavedGifs()
		: Data::FileOrigin();
}

void GifsListWidget::afterShown() {
	if (_search) {
		_search->stealFocus();
	}
}

void GifsListWidget::beforeHiding() {
	if (_search) {
		_search->returnFocus();
	}
}

bool GifsListWidget::refreshInlineRows(int32 *added) {
	auto it = _inlineCache.find(_inlineQuery);
	auto entry = (const InlineCacheEntry*)nullptr;
	if (it != _inlineCache.cend()) {
		entry = it->second.get();
		_inlineNextOffset = it->second->nextOffset;
	}
	auto result = refreshInlineRows(entry, false);
	if (added) *added = result;
	return (entry != nullptr);
}

void GifsListWidget::setupSearch() {
	const auto session = &_show->session();
	_search = MakeSearch(this, st(), [=](std::vector<QString> &&query) {
		const auto accumulated = ranges::accumulate(query, QString(), [](
				QString a,
				QString b) {
			return a.isEmpty() ? b : (a + ' ' + b);
		});
		if (!accumulated.isEmpty() && _collectionId) {
			_collectionId = 0;
			refreshCollectionButtons();
		}
		_chosenSetId = accumulated.isEmpty()
			? Data::Stickers::RecentSetId
			: SearchEmojiSectionSetId();
		refreshIcons();
		searchForGifs(accumulated);
	}, session, TabbedSearchType::Emoji);
}

void GifsListWidget::setupCollections() {
	const auto bar = _collectionBar = Ui::CreateChild<Ui::ScrollArea>(
		this,
		st::gifCollectionScroll);
	bar->setCustomWheelProcess([=](not_null<QWheelEvent*> e) {
		const auto pixel = e->pixelDelta();
		const auto angle = e->angleDelta();
		if (pixel.x() || angle.x() || !bar->scrollLeftMax()) {
			return false;
		}
		bar->scrollToX(bar->scrollLeft() - (pixel.y() ? pixel.y() : angle.y()));
		return true;
	});
	widthValue() | rpl::on_next([=](int width) {
		const auto &margin = st().searchMargin;
		bar->setGeometry(
			margin.left(),
			_search->height() + margin.top(),
			std::max(width - margin.left() - margin.right(), 0),
			st().search.height);
	}, lifetime());

	session().data().stickers().gifCollectionsChanged(
	) | rpl::on_next([=] {
		if (_collectionId
			&& !session().data().stickers().gifCollection(_collectionId)) {
			selectCollection(0);
			return;
		}
		refreshCollectionButtons();
		if (_section == Section::Gifs) {
			refreshSavedGifs();
		} else {
			resizeToWidth(width());
			update();
		}
	}, lifetime());
	refreshCollectionButtons();
}

void GifsListWidget::refreshCollectionButtons() {
	const auto &collections = session().data().stickers().gifCollections();
	const auto height = _search->height();
	_mosaic.setPadding(st::gifsPadding + QMargins(
		-st::emojiPanRadius,
		height * (collections.empty() ? 1 : 2),
		0,
		0));
	_collectionBar->setVisible(!collections.empty());
	if (collections.empty()) {
		return;
	}

	const auto left = _collectionBar->scrollLeft();
	auto row = object_ptr<GifCollectionRow>(
		_collectionBar,
		[=](uint32 id, uint32 targetId) {
			InvokeQueued(this, [=] {
				session().data().stickers().moveGifCollection(id, targetId);
			});
		});
	const auto add = [&](uint32 id, QString name) {
		const auto button = Ui::CreateChild<Ui::RoundButton>(
			row.data(),
			rpl::single(std::move(name)),
			(id == _collectionId)
				? st::gifCollectionButtonActive
				: st::gifCollectionButton);
		button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		button->setFullWidth(std::min(
			button->width(),
			st::gifCollectionButtonMaxWidth));
		button->setFullRadius(true);
		button->setClickedCallback([=] {
			InvokeQueued(this, [=] {
				if (!id || session().data().stickers().gifCollection(id)) {
					selectCollection(id);
				}
			});
		});
		if (id) {
			button->setContextMenuPolicy(Qt::CustomContextMenu);
			connect(
				button,
				&QWidget::customContextMenuRequested,
				this,
				[=](QPoint position) {
					showCollectionMenu(id, button->mapToGlobal(position));
				});
		}
		row->addButton(id, button);
	};
	add(0, tr::lng_gif_collections_all(tr::now));
	for (const auto &collection : collections) {
		add(collection.id, collection.name);
	}
	_collectionBar->setOwnedWidget(std::move(row));
	_collectionBar->scrollToX(left);
}

void GifsListWidget::selectCollection(uint32 id) {
	_search->cancel();
	_collectionId = id;
	_chosenSetId = Data::Stickers::RecentSetId;
	refreshIcons();
	refreshCollectionButtons();
	cancelGifsSearch();
	scrollTo(0);
}

bool GifsListWidget::hasCollectionMenu() const {
	return _collectionMenu && _collectionMenu->isVisible();
}

void GifsListWidget::showCollectionMenu(uint32 id, QPoint position) {
	const auto collection = session().data().stickers().gifCollection(id);
	if (!collection) {
		return;
	}
	_collectionMenu = base::make_unique_q<Ui::PopupMenu>(this, st().menu);
	_collectionMenu->addAction(
		tr::lng_gif_collections_rename(tr::now),
		[=] { editCollection(id); },
		&st::menuIconEdit);
	_collectionMenu->addAction(
		tr::lng_gif_collections_delete(tr::now),
		[=, name = collection->name] {
			showBoxPreventHide(Ui::MakeConfirmBox({
				.text = tr::lng_gif_collections_confirm_delete(
					lt_title,
					rpl::single(name)),
				.confirmed = crl::guard(this, [=](Fn<void()> &&close) {
					session().data().stickers().deleteGifCollection(id);
					close();
				}),
				.confirmText = tr::lng_box_delete(),
				.confirmStyle = &st::attentionBoxButton,
			}));
		},
		&st::menuIconDeleteAttention);
	_collectionMenu->popup(position);
}

void GifsListWidget::editCollection(uint32 id, DocumentData *gif) {
	const auto collection = session().data().stickers().gifCollection(id);
	if (id && !collection) {
		return;
	}
	showBoxPreventHide(Box([=, text = collection ? collection->name : QString()](
			not_null<Ui::GenericBox*> box) {
		box->setTitle(id
			? tr::lng_gif_collections_rename()
			: tr::lng_gif_collections_create());
		const auto input = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			tr::lng_gif_collections_name(),
			text));
		input->setMaxLength(kGifCollectionNameLimit);
		box->setFocusCallback([=] { input->setFocusFast(); });

		const auto done = crl::guard(this, [=] {
			auto name = input->getLastText().trimmed();
			if (name.isEmpty()) {
				input->showError();
				return;
			}
			auto &stickers = session().data().stickers();
			if (id) {
				stickers.renameGifCollection(id, std::move(name));
			} else {
				stickers.createGifCollection(std::move(name), gif);
			}
			box->closeBox();
		});
		box->addButton(
			id ? tr::lng_settings_save() : tr::lng_filters_create_button(),
			done);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		input->submits() | rpl::on_next([=] { done(); }, input->lifetime());
	}));
}

void GifsListWidget::addCollectionMenu(
		not_null<Ui::PopupMenu*> menu,
		not_null<DocumentData*> gif) {
	const auto action = menu->addAction(
		tr::lng_gif_collections_add(tr::now),
		Fn<void()>(),
		&st::menuIconAddToFolder);
	const auto submenu = menu->ensureSubmenu(action, st::foldersMenu);
	for (const auto &collection : session().data().stickers().gifCollections()) {
		const auto id = collection.id;
		const auto contains = ranges::contains(collection.gifs, gif);
		const auto check = contains ? &st::mediaPlayerMenuCheck : nullptr;
		submenu->addAction(
			Ui::Text::FixAmpersandInAction(collection.name),
			crl::guard(this, [=] {
				session().data().stickers().toggleGifCollectionItem(id, gif);
			}),
			check,
			check);
	}
	submenu->addAction(
		tr::lng_gif_collections_create(tr::now),
		crl::guard(this, [=] { editCollection(0, gif); }),
		&st::menuIconShowInFolder);
}

int32 GifsListWidget::showInlineRows(bool newResults) {
	auto added = 0;
	refreshInlineRows(&added);
	if (newResults) {
		scrollTo(0);
	}
	return added;
}

void GifsListWidget::searchForGifs(const QString &query) {
	if (query.isEmpty()) {
		cancelGifsSearch();
		return;
	}

	if (_inlineQuery != query) {
		_search->setLoading(false);
		if (_inlineRequestId) {
			_api.request(_inlineRequestId).cancel();
			_inlineRequestId = 0;
		}
		if (_inlineCache.find(query) != _inlineCache.cend()) {
			_inlineRequestTimer.stop();
			_inlineQuery = _inlineNextQuery = query;
			showInlineRows(true);
		} else {
			_inlineNextQuery = query;
			_inlineRequestTimer.start(kSearchRequestDelay);
		}
	}

	if (!_searchBot && !_searchBotRequestId) {
		const auto username = session().serverConfig().gifSearchUsername;
		_searchBotRequestId = _api.request(MTPcontacts_ResolveUsername(
			MTP_flags(0),
			MTP_string(username),
			MTP_string()
		)).done([=](const MTPcontacts_ResolvedPeer &result) {
			auto &data = result.data();
			session().data().processUsers(data.vusers());
			session().data().processChats(data.vchats());
			const auto peer = session().data().peerLoaded(
				peerFromMTP(data.vpeer()));
			if (const auto user = peer ? peer->asUser() : nullptr) {
				_searchBot = user;
			}
		}).send();
	}
}

void GifsListWidget::cancelled() {
	_cancelled.fire({});
}

rpl::producer<> GifsListWidget::cancelRequests() const {
	return _cancelled.events();
}

void GifsListWidget::sendInlineRequest() {
	if (_inlineRequestId || !_inlineQueryPeer || _inlineNextQuery.isEmpty()) {
		return;
	}

	if (!_searchBot) {
		// Wait for the bot being resolved.
		_search->setLoading(true);
		_inlineRequestTimer.start(kSearchRequestDelay);
		return;
	}
	_inlineRequestTimer.stop();
	_inlineQuery = _inlineNextQuery;

	auto nextOffset = QString();
	auto it = _inlineCache.find(_inlineQuery);
	if (it != _inlineCache.cend()) {
		nextOffset = it->second->nextOffset;
		if (nextOffset.isEmpty()) {
			_search->setLoading(false);
			return;
		}
	}

	_search->setLoading(true);
	_inlineRequestId = _api.request(MTPmessages_GetInlineBotResults(
		MTP_flags(0),
		_searchBot->inputUser(),
		_inlineQueryPeer->session().sendAsPeers().chosenAccount(_inlineQueryPeer)
			? MTP_inputPeerSelf()
			: _inlineQueryPeer->input(),
		MTPInputGeoPoint(),
		MTP_string(_inlineQuery),
		MTP_string(nextOffset)
	)).done([this](const MTPmessages_BotResults &result) {
		inlineResultsDone(result);
	}).fail([this] {
		// show error?
		_search->setLoading(false);
		_inlineRequestId = 0;
	}).handleAllErrors().send();
}

void GifsListWidget::refreshRecent() {
	if (_section == Section::Gifs) {
		refreshSavedGifs();
	}
}

void GifsListWidget::updateSelected() {
	if (_pressed >= 0 && !_previewShown) {
		return;
	}

	const auto p = mapFromGlobal(_lastMousePos);
	const auto sx = rtl() ? (width() - p.x()) : p.x();
	const auto sy = p.y();
	const auto &[index, exact, relative] = _mosaic.findByPoint({ sx, sy });
	const auto selected = exact ? index : -1;
	const auto item = exact ? _mosaic.itemAt(selected).get() : nullptr;
	const auto link = exact ? item->getState(relative, {}).link : nullptr;

	if (_selected != selected) {
		if (const auto s = _mosaic.maybeItemAt(_selected)) {
			s->update();
		}
		_selected = selected;
		if (item) {
			item->update();
		}
		if (_previewShown && _selected >= 0 && _pressed != _selected) {
			_pressed = _selected;
			if (item) {
				if (const auto preview = item->getPreviewDocument()) {
					_show->showMediaPreview(
						Data::FileOriginSavedGifs(),
						preview);
				} else if (const auto preview = item->getPreviewPhoto()) {
					_show->showMediaPreview(Data::FileOrigin(), preview);
				}
			}
		}
	}
	if (ClickHandler::setActive(link, item)) {
		setCursor(link ? style::cur_pointer : style::cur_default);
	}
}

void GifsListWidget::showPreview() {
	if (_pressed < 0) {
		return;
	}
	if (const auto layout = _mosaic.maybeItemAt(_pressed)) {
		if (const auto previewDocument = layout->getPreviewDocument()) {
			_previewShown = _show->showMediaPreview(
				Data::FileOriginSavedGifs(),
				previewDocument);
		} else if (const auto previewPhoto = layout->getPreviewPhoto()) {
			_previewShown = _show->showMediaPreview(
				Data::FileOrigin(),
				previewPhoto);
		}
	}
}

void GifsListWidget::updateInlineItems() {
	const auto now = crl::now();

	const auto delay = std::max(
		_lastScrolledAt + kMinAfterScrollDelay - now,
		_lastUpdatedAt + kMinRepaintDelay - now);
	if (delay <= 0) {
		repaintItems(now);
	} else if (!_updateInlineItems.isActive()
		|| _updateInlineItems.remainingTime() > kMinRepaintDelay) {
		_updateInlineItems.callOnce(std::max(delay, kMinRepaintDelay));
	}
}

void GifsListWidget::repaintItems(crl::time now) {
	_lastUpdatedAt = now ? now : crl::now();
	update();
}

} // namespace ChatHelpers
