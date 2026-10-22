/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mainwindow.h"
#include "history/view/history_view_top_bar_blur.h"
#include "history/view/history_view_blur_transition.h"
#include "history/view/history_view_top_bar_widget.h"

#include "data/data_document.h"
#include "data/data_session.h"
#include "data/data_document_media.h"
#include "dialogs/ui/dialogs_layout.h"
#include "history/history.h"
#include "history/history_drag_area.h"
#include "ui/effects/animations.h"
#include "crl/crl_on_main.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/shadow.h"
#include "ui/widgets/tooltip.h"
#include "ui/effects/layer_backdrop.h"
#include "ui/emoji_config.h"
#include "ui/ui_utility.h"
#include "ui/painter.h"
#include "lang/lang_cloud_manager.h"
#include "lang/lang_instance.h"
#include "core/sandbox.h"
#include "core/application.h"
#include "export/export_manager.h"
#include "inline_bots/bot_attach_web_view.h" // AttachWebView::cancel.
#include "intro/intro_widget.h"
#include "main/main_session.h"
#include "main/main_account.h" // Account::sessionValue.
#include "main/main_domain.h"
#include "mainwidget.h"
#include "ui/boxes/confirm_box.h"
#include "boxes/connection_box.h"
#include "storage/storage_account.h"
#include "storage/localstorage.h"
#include "apiwrap.h"
#include "api/api_updates.h"
#include "settings/settings_intro.h"
#include "base/options.h"
#include "wallet/wallet_panel.h"
#include "window/notifications_manager.h"
#include "window/themes/window_theme.h"
#include "window/themes/window_theme_warning.h"
#include "window/window_main_menu.h"
#include "window/window_controller.h" // App::wnd.
#include "window/window_session_controller.h"
#include "window/window_setup_email.h"
#include "window/window_media_preview.h"
#include "styles/style_layers.h"
#include "styles/style_window.h"

#include <QtGui/QWindow>

namespace {

class LayerBlur final {
public:
	LayerBlur(
		not_null<Ui::LayerStackWidget*> layer,
		QImage backdrop,
		Fn<void()> closed);

private:
	void update();
	void finish();
	void refreshBackdrop();

	not_null<Ui::LayerStackWidget*> _layer;
	Ui::RpWidget _widget;
	QImage _backdrop;
	HistoryView::BlurTransition _transition;
	HistoryView::BlurBackground _blur;
	Fn<void()> _closed;
	Ui::Animations::Basic _animation;
	bool _wasAnimating = false;
	bool _finished = false;
	rpl::lifetime _lifetime;

};

LayerBlur::LayerBlur(
		not_null<Ui::LayerStackWidget*> layer,
		QImage backdrop,
		Fn<void()> closed)
: _layer(layer)
, _widget(layer->parentWidget())
, _backdrop(std::move(backdrop))
, _transition(st::boxDuration)
, _blur(&_widget, [=] {
	return HistoryView::PrepareBlurImage(_backdrop, _widget.size(), true);
}, [] { return QColor(Qt::transparent); }, 0.f, [=] {
	return float(_transition.value(crl::now()));
})
, _closed(std::move(closed))
, _animation([=](crl::time now) {
	update();
	if (base::options::value<bool>(HistoryView::kOptionRealtimeBlur)
		&& base::options::value<bool>(HistoryView::kOptionBlurEffects)
		&& (_transition.shown() || _transition.animating(now))) {
		refreshBackdrop();
		return true;
	}
	return _transition.animating(now);
}) {
	_widget.setAttribute(Qt::WA_TransparentForMouseEvents);
	_widget.setFocusPolicy(Qt::NoFocus);
	_widget.hide();
	_layer->paintRequest() | rpl::on_next([=] { update(); }, _lifetime);
	_layer->events() | rpl::on_next([=](not_null<QEvent*> event) {
		if (event->type() == QEvent::Show) {
			update();
		} else if (event->type() == QEvent::ChildAdded
			|| event->type() == QEvent::ChildRemoved) {
			crl::on_main(_layer, [=] { update(); });
		}
	}, _lifetime);
	_layer->sizeValue() | rpl::on_next([=] {
		_widget.setGeometry(_layer->geometry());
	}, _lifetime);
	rpl::merge(
		base::options::lookup<bool>(HistoryView::kOptionBlurEffects).changes(),
		base::options::lookup<bool>(HistoryView::kOptionRealtimeBlur).changes()
	) | rpl::on_next([=] {
		update();
		if (base::options::value<bool>(HistoryView::kOptionRealtimeBlur)) {
			_animation.start();
		}
	}, _lifetime);
	_layer->hideFinishEvents() | rpl::on_next([=] {
		_finished = true;
		update();
	}, _lifetime);
}

void LayerBlur::update() {
	const auto now = crl::now();
	const auto shown = _layer->canSetFocus();
	const auto enabled = base::options::value<bool>(HistoryView::kOptionBlurEffects);
	if (enabled || _layer->window()->property("xenonWindowsBackdrop").toBool()) {
		_layer->removeBodyCache();
	}
	const auto changed = (_transition.shown() != shown);
	_transition.setShown(shown, now);
	if (changed) {
		if (shown) {
			_finished = false;
		}
		_widget.stackUnder(_layer);
		_widget.show();
		_animation.start();
	}
	const auto animating = _transition.animating(now);
	if (changed || animating || _wasAnimating) {
		_widget.update();
	}
	_wasAnimating = animating;
	finish();
}

void LayerBlur::refreshBackdrop() {
	const auto parent = _layer->parentWidget();
	const auto ratio = parent->devicePixelRatioF();
	auto image = QImage(parent->size() * ratio, QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(ratio);
	image.fill(Qt::transparent);
	{
		const auto snapshot = HistoryView::BlurSnapshot(parent);
		Painter p(&image);
		Ui::RenderWidget(p, parent, QPoint(), parent->rect(), QWidget::DrawWindowBackground);
		for (const auto child : parent->findChildren<QWidget*>(
			QString(), Qt::FindDirectChildrenOnly)) {
			if (child == _layer || child == &_widget
				|| child->isHidden() || child->isWindow()) {
				continue;
			}
			p.save();
			p.setClipRect(child->geometry());
			Ui::RenderWidget(p, child, child->pos(), child->rect());
			p.restore();
		}
	}
	_backdrop = std::move(image);
	_blur.invalidate();
}

void LayerBlur::finish() {
	if (_finished && !_transition.shown() && !_transition.animating(crl::now())) {
		_finished = false;
		crl::on_main(_layer, _closed);
	}
}

// Code for testing languages is F7-F6-F7-F8
void FeedLangTestingKey(int key) {
	static auto codeState = 0;
	if ((codeState == 0 && key == Qt::Key_F7)
		|| (codeState == 1 && key == Qt::Key_F6)
		|| (codeState == 2 && key == Qt::Key_F7)
		|| (codeState == 3 && key == Qt::Key_F8)) {
		++codeState;
	} else {
		codeState = 0;
	}
	if (codeState == 4) {
		codeState = 0;
		Lang::CurrentCloudManager().switchToTestLanguage();
	}
}

base::options::toggle AutoScrollInactiveChat({
	.id = kOptionAutoScrollInactiveChat,
	.name = "Mark as read of inactive chat",
	.description = "Mark new messages as read and scroll the chat "
		"even when the window is not in focus.",
});

} // namespace

const char kOptionAutoScrollInactiveChat[]
	= "auto-scroll-inactive-chat";

MainWindow::MainWindow(not_null<Window::Controller*> controller)
: Platform::MainWindow(controller) {
	resize(st::windowDefaultWidth, st::windowDefaultHeight);

	setLocale(QLocale(QLocale::English, QLocale::UnitedStates));

	using Window::Theme::BackgroundUpdate;
	Window::Theme::Background()->updates(
	) | rpl::on_next([=](const BackgroundUpdate &data) {
		themeUpdated(data);
	}, lifetime());

	Core::App().passcodeLockChanges(
	) | rpl::on_next([=] {
		updateGlobalMenu();
	}, lifetime());

	Ui::Emoji::Updated(
	) | rpl::on_next([=] {
		Ui::ForceFullRepaint(this);
	}, lifetime());

	setAttribute(Qt::WA_OpaquePaintEvent, !Window::UseWindowsAcrylic());
}

void MainWindow::initHook() {
	Platform::MainWindow::initHook();
	QCoreApplication::instance()->installEventFilter(this);
}

void MainWindow::applyInitialWorkMode() {
	const auto workMode = Core::App().settings().workMode();
	workmodeUpdated(workMode);

	if (controller().isPrimary()) {
		if (Core::App().settings().windowPosition().maximized) {
			DEBUG_LOG(("Window Pos: First show, setting maximized."));
			setWindowState(Qt::WindowMaximized);
		}
		if (cStartInTray()
			|| (cLaunchMode() == LaunchModeAutoStart
				&& cStartMinimized()
				&& !Core::App().passcodeLocked())) {
			DEBUG_LOG(("Window Pos: First show, setting minimized after."));
			if (workMode == Core::Settings::WorkMode::TrayOnly
				|| workMode == Core::Settings::WorkMode::WindowAndTray) {
				hide();
			} else {
				setWindowState(windowState() | Qt::WindowMinimized);
			}
		}
	}
	setPositionInited();
}

void MainWindow::finishFirstShow() {
	applyInitialWorkMode();
	createGlobalMenu();

	windowActiveValue(
	) | rpl::skip(1) | rpl::filter(
		!rpl::mappers::_1
	) | rpl::on_next([=] {
		Ui::Tooltip::Hide();
	}, lifetime());

	setAttribute(Qt::WA_NoSystemBackground);

	if (!_passcodeLock && !_setupEmailLock && _main) {
		_main->activate();
	} else if (!_passcodeLock && !_setupEmailLock && _intro) {
		_intro->setInnerFocus();
	}
}

void MainWindow::clearWidgetsHook() {
	_mediaPreview.destroy();
	_main.destroy();
	_intro.destroy();
	if (!Core::App().passcodeLocked()) {
		_passcodeLock.destroy();
	}
	_setupEmailLock.destroy();
}

QPixmap MainWindow::grabForSlideAnimation() {
	const auto body = bodyWidget();
	return body->size().isEmpty()
		? QPixmap()
		: HistoryView::GrabBlurSnapshot(body);
}

void MainWindow::preventOrInvoke(Fn<void()> callback) {
	if (_main && _main->preventsCloseSection(callback)) {
		return;
	}
	callback();
}

void MainWindow::setupPasscodeLock() {
	auto animated = (_main || _intro);
	auto oldContentCache = animated ? grabForSlideAnimation() : QPixmap();
	_passcodeLock.create(bodyWidget(), &controller());
	updateControlsGeometry();

	ui_hideSettingsAndLayer(anim::type::instant);
	if (_main) {
		_main->hide();
	}
	if (_intro) {
		_intro->hide();
	}
	if (animated) {
		_passcodeLock->showAnimated(std::move(oldContentCache));
	} else {
		_passcodeLock->showFinished();
		setInnerFocus();
	}
	if (const auto sessionController = controller().sessionController()) {
		sessionController->session().attachWebView().closeAll();
	}
}

void MainWindow::setupSetupEmailLock() {
	auto animated = (_main || _intro || _passcodeLock);
	auto oldContentCache = animated ? grabForSlideAnimation() : QPixmap();
	_setupEmailLock.create(bodyWidget(), &controller());
	updateControlsGeometry();

	ui_hideSettingsAndLayer(anim::type::instant);
	if (_main) {
		_main->hide();
	}
	if (_intro) {
		_intro->hide();
	}
	if (_passcodeLock) {
		_passcodeLock->hide();
	}
	if (animated) {
		_setupEmailLock->showAnimated(std::move(oldContentCache));
	} else {
		_setupEmailLock->showFinished();
		setInnerFocus();
	}
	if (const auto sessionController = controller().sessionController()) {
		sessionController->session().attachWebView().closeAll();
		Wallet::CloseWallet(&sessionController->session());
	}
}

void MainWindow::clearSetupEmailLock() {
	if (!_setupEmailLock) {
		return;
	}

	auto oldContentCache = grabForSlideAnimation();
	_setupEmailLock.destroy();
	if (_passcodeLock) {
		_passcodeLock->show();
		updateControlsGeometry();
		_passcodeLock->showAnimated(std::move(oldContentCache));
	} else if (_intro) {
		_intro->show();
		updateControlsGeometry();
		_intro->showAnimated(std::move(oldContentCache), true);
	} else if (_main) {
		_main->show();
		updateControlsGeometry();
		_main->showAnimated(std::move(oldContentCache), true);
	}
}

void MainWindow::clearPasscodeLock() {
	Expects(_intro || _main);

	if (!_passcodeLock) {
		return;
	}

	auto oldContentCache = grabForSlideAnimation();
	_passcodeLock.destroy();
	if (_setupEmailLock) {
		_setupEmailLock->show();
		updateControlsGeometry();
		_setupEmailLock->showAnimated(std::move(oldContentCache));
	} else if (_intro) {
		_intro->show();
		updateControlsGeometry();
		_intro->showAnimated(std::move(oldContentCache), true);
	} else if (_main) {
		_main->show();
		updateControlsGeometry();
		_main->showAnimated(std::move(oldContentCache), true);
		Core::App().checkStartUrls();
	}
}

void MainWindow::setupIntro(
		Intro::EnterPoint point,
		Main::Account *accountBeforeIntro,
		QPixmap oldContentCache) {
	auto animated = (_main || _passcodeLock || _setupEmailLock);

	destroyLayer();
	auto created = object_ptr<Intro::Widget>(
		bodyWidget(),
		&controller(),
		&account(),
		point,
		accountBeforeIntro);
	created->showSettingsRequested(
	) | rpl::on_next([=] {
		showSettings();
	}, created->lifetime());

	clearWidgets();
	_intro = std::move(created);
	DragArea::SetupProxyDropArea(_intro.data(), [](const QString &localUrl) {
		Core::App().openLocalUrl(localUrl, {});
	});
	if (_passcodeLock || _setupEmailLock) {
		_intro->hide();
	} else {
		_intro->show();
		updateControlsGeometry();
		if (animated) {
			_intro->showAnimated(std::move(oldContentCache));
		} else {
			setInnerFocus();
		}
	}
	fixOrder();
}

void MainWindow::setupMain(
		MsgId singlePeerShowAtMsgId,
		QPixmap oldContentCache) {
	Expects(account().sessionExists());

	const auto animated = _intro
		|| (_passcodeLock && !Core::App().passcodeLocked())
		|| _setupEmailLock;
	const auto weakAnimatedLayer = (_main
			&& _layer
			&& !_passcodeLock
			&& !_setupEmailLock)
		? base::make_weak(_layer.get())
		: nullptr;
	if (weakAnimatedLayer) {
		Assert(!animated);
		_layer->hideAllAnimatedPrepare();
	} else {
		destroyLayer();
	}
	auto created = object_ptr<MainWidget>(bodyWidget(), sessionController());
	clearWidgets();
	_main = std::move(created);
	updateControlsGeometry();
	Ui::SendPendingMoveResizeEvents(_main);
	_main->controller()->showByInitialId(
		Window::SectionShow::Way::ClearStack,
		singlePeerShowAtMsgId);
	if (_passcodeLock || _setupEmailLock) {
		_main->hide();
	} else {
		_main->show();
		updateControlsGeometry();
		if (animated) {
			_main->showAnimated(std::move(oldContentCache));
		} else {
			_main->activate();
		}
		Core::App().checkStartUrls();
	}
	fixOrder();
	if (const auto strong = weakAnimatedLayer.get()) {
		strong->hideAllAnimatedRun();
	}
}

void MainWindow::showSettings() {
	if (_passcodeLock || _setupEmailLock) {
		return;
	}

	if (const auto session = sessionController()) {
		session->showSettings();
	} else {
		controller().hideLayer(anim::type::instant);
		showSpecialLayer(
			Box<Settings::LayerWidget>(&controller()),
			anim::type::normal);
	}
}

void MainWindow::showSpecialLayer(
		object_ptr<Ui::LayerWidget> layer,
		anim::type animated) {
	if (_passcodeLock || _setupEmailLock) {
		return;
	}

	if (layer) {
		ensureLayerCreated();
		_layer->showSpecialLayer(std::move(layer), animated);
	} else if (_layer) {
		_layer->hideSpecialLayer(animated);
	}
}

bool MainWindow::showSectionInExistingLayer(
		not_null<Window::SectionMemento*> memento,
		const Window::SectionShow &params) {
	if (_layer) {
		return _layer->showSectionInternal(memento, params);
	}
	return false;
}

void MainWindow::showMainMenu() {
	if (_passcodeLock || _setupEmailLock) return;

	if (isHidden()) showFromTray();

	ensureLayerCreated();
	_layer->showMainMenu(
		object_ptr<Window::MainMenu>(body(), sessionController()),
		anim::type::normal);
}

void MainWindow::ensureLayerCreated() {
	if (_layer) {
		return;
	}
	const auto backdrop = HistoryView::GrabBlurSnapshot(bodyWidget()).toImage();
	_layer = base::make_unique_q<Ui::LayerStackWidget>(
		bodyWidget(),
		crl::guard(this, [=] { return controller().uiShow(); }));
	const auto layer = _layer.get();
	_layer->lifetime().make_state<LayerBlur>(layer, backdrop, [=] {
		if (_layer.get() == layer && !layer->canSetFocus()) {
			destroyLayer();
		}
	});

	_layer->boxShownValue(
	) | rpl::on_next([=](bool shown) {
		_boxShown = shown;
	}, _layer->lifetime());

	rpl::merge(
		rpl::single(rpl::empty_value()),
		base::options::lookup<bool>(HistoryView::kOptionBlurEffects).changes(),
		base::options::lookup<bool>(HistoryView::kOptionRealtimeBlur).changes()
	) | rpl::on_next([=] {
		if (const auto controller = sessionController()) {
			if (base::options::value<bool>(HistoryView::kOptionBlurEffects)
				&& base::options::value<bool>(HistoryView::kOptionRealtimeBlur)) {
				controller->disableGifPauseReason(Window::GifPauseReason::Layer);
			} else {
				controller->enableGifPauseReason(Window::GifPauseReason::Layer);
			}
		}
	}, _layer->lifetime());
}

void MainWindow::destroyLayer() {
	if (!_layer) {
		return;
	}

	auto layer = base::take(_layer);
	_boxShown = false;
	const auto resetFocus = Ui::InFocusChain(layer);
	if (resetFocus) {
		setFocus();
	}
	layer = nullptr;

	if (const auto controller = sessionController()) {
		controller->disableGifPauseReason(Window::GifPauseReason::Layer);
	}
	if (resetFocus) {
		setInnerFocus();
	}
	InvokeQueued(this, [=] {
		checkActivation();
	});
}

void MainWindow::ui_hideSettingsAndLayer(anim::type animated) {
	if (animated == anim::type::instant) {
		destroyLayer();
	} else if (_layer) {
		_layer->hideAll(animated);
	}
}

void MainWindow::ui_removeLayerBlackout() {
	if (_layer) {
		_layer->removeBodyCache();
	}
}

MainWidget *MainWindow::sessionContent() const {
	return _main.data();
}

void MainWindow::showOrHideBoxOrLayer(
		std::variant<
			v::null_t,
			object_ptr<Ui::BoxContent>,
			std::unique_ptr<Ui::LayerWidget>> &&layer,
		Ui::LayerOptions options,
		anim::type animated) {
	using UniqueLayer = std::unique_ptr<Ui::LayerWidget>;
	using ObjectBox = object_ptr<Ui::BoxContent>;
	if (auto layerWidget = std::get_if<UniqueLayer>(&layer)) {
		ensureLayerCreated();
		_layer->showLayer(std::move(*layerWidget), options, animated);
	} else if (auto box = std::get_if<ObjectBox>(&layer)) {
		ensureLayerCreated();
		_layer->showBox(std::move(*box), options, animated);
	} else {
		if (_layer) {
			_layer->hideTopLayer(animated);
			if ((animated == anim::type::instant)
				&& _layer
				&& !_layer->layerShown()) {
				destroyLayer();
			}
		}
		Core::App().hideMediaView();
	}
}

bool MainWindow::ui_isLayerShown() const {
	return _layer != nullptr;
}

rpl::producer<bool> MainWindow::ui_boxShownValue() const {
	return _boxShown.value();
}

bool MainWindow::closeLayerByBackButton() {
	return _layer && _layer->closeCurrentByBackButton();
}

bool MainWindow::showMediaPreview(
		Data::FileOrigin origin,
		not_null<DocumentData*> document) {
	const auto media = document->activeMediaView();
	const auto preview = Data::VideoPreviewState(media.get());
	if (!document->sticker()
		&& (!document->isAnimation() || !preview.loaded())) {
		return false;
	}
	if (!_mediaPreview) {
		_mediaPreview.create(bodyWidget(), sessionController());
		updateControlsGeometry();
	}
	if (_mediaPreview->isHidden()) {
		raiseMediaPreview();
	}
	_mediaPreview->showPreview(origin, document);
	return true;
}

bool MainWindow::showMediaPreview(
		Data::FileOrigin origin,
		not_null<PhotoData*> photo) {
	if (!_mediaPreview) {
		_mediaPreview.create(bodyWidget(), sessionController());
		updateControlsGeometry();
	}
	if (_mediaPreview->isHidden()) {
		raiseMediaPreview();
	}
	_mediaPreview->showPreview(origin, photo);
	return true;
}

void MainWindow::hideMediaPreview() {
	if (!_mediaPreview) {
		return;
	}
	_mediaPreview->hidePreview();
}

void MainWindow::themeUpdated(const Window::Theme::BackgroundUpdate &data) {
	using Type = Window::Theme::BackgroundUpdate::Type;

	// We delay animating theme warning because we want all other
	// subscribers to receive palette changed notification before any
	// animations (that include pixmap caches with old palette values).
	if (data.type == Type::TestingTheme) {
		if (!_testingThemeWarning) {
			_testingThemeWarning.create(bodyWidget());
			_testingThemeWarning->hide();
			_testingThemeWarning->setGeometry(rect());
			_testingThemeWarning->setHiddenCallback([this] { _testingThemeWarning.destroyDelayed(); });
		}
		crl::on_main(this, [=] {
			if (_testingThemeWarning) {
				_testingThemeWarning->showAnimated();
			}
		});
	} else if (data.type == Type::RevertingTheme || data.type == Type::ApplyingTheme) {
		if (_testingThemeWarning) {
			if (_testingThemeWarning->isHidden()) {
				_testingThemeWarning.destroy();
			} else {
				crl::on_main(this, [=] {
					if (_testingThemeWarning) {
						_testingThemeWarning->hideAnimated();
						_testingThemeWarning = nullptr;
					}
					setInnerFocus();
				});
			}
		}
	}
}

bool MainWindow::markingAsRead() const {
	return _main
		&& !_main->isHidden()
		&& !_main->animatingShow()
		&& !_layer
		&& !isHidden()
		&& !isMinimized()
		&& windowHandle()->isExposed()
		&& (AutoScrollInactiveChat.value()
			|| (isActive() && !_main->session().updates().isIdle()));
}

void MainWindow::checkActivation() {
	updateIsActive();
	if (_main) {
		_main->checkActivation();
	}
}

bool MainWindow::contentOverlapped(const QRect &globalRect) {
	return (_main && _main->contentOverlapped(globalRect))
		|| (_layer
			&& !Ui::CapturingUnderLayer()
			&& _layer->contentOverlapped(globalRect));
}

void MainWindow::setInnerFocus() {
	if (_testingThemeWarning) {
		_testingThemeWarning->setFocus();
	} else if (_layer && _layer->canSetFocus()) {
		_layer->setInnerFocus();
	} else if (_passcodeLock) {
		_passcodeLock->setInnerFocus();
	} else if (_setupEmailLock) {
		_setupEmailLock->setInnerFocus();
	} else if (_main) {
		_main->setInnerFocus();
	} else if (_intro) {
		_intro->setInnerFocus();
	}
}

bool MainWindow::eventFilter(QObject *object, QEvent *e) {
	switch (e->type()) {
	case QEvent::KeyPress: {
		if (Logs::DebugEnabled()
			&& object == windowHandle()) {
			const auto key = static_cast<QKeyEvent*>(e)->key();
			FeedLangTestingKey(key);
		}
#ifdef _DEBUG
		if (static_cast<QKeyEvent*>(e)->modifiers().testFlag(
				Qt::ControlModifier)) {
			switch (static_cast<QKeyEvent*>(e)->key()) {
			case Qt::Key_F11:
				anim::SetSlowMultiplier((anim::SlowMultiplier() == 10)
					? 1
					: 10);
				return true;
			case Qt::Key_F12:
				anim::SetSlowMultiplier((anim::SlowMultiplier() == 50)
					? 1
					: 50);
				return true;
			}
		}
#endif
	} break;

	case QEvent::MouseMove: {
		const auto position = static_cast<QMouseEvent*>(e)->globalPos();
		if (_lastMousePosition != position) {
			if (const auto controller = sessionController()) {
				if (controller->session().updates().isIdle()) {
					Core::App().updateNonIdle();
				}
			}
		}
		_lastMousePosition = position;
	} break;

	case QEvent::MouseButtonRelease: {
		hideMediaPreview();
	} break;

	case QEvent::ApplicationActivate: {
		if (object == QCoreApplication::instance()) {
			InvokeQueued(this, [=] {
				handleActiveChanged(isActiveWindow());
			});
		}
	} break;

	case QEvent::WindowStateChange: {
		if (object == this) {
			auto state = (windowState() & Qt::WindowMinimized) ? Qt::WindowMinimized :
				((windowState() & Qt::WindowMaximized) ? Qt::WindowMaximized :
				((windowState() & Qt::WindowFullScreen) ? Qt::WindowFullScreen : Qt::WindowNoState));
			handleStateChanged(state);
		}
	} break;

	case QEvent::Move:
	case QEvent::Resize: {
		if (object == this) {
			positionUpdated();
		}
	} break;
	}

	return Platform::MainWindow::eventFilter(object, e);
}

bool MainWindow::takeThirdSectionFromLayer() {
	return _layer ? _layer->takeToThirdSection() : false;
}

void MainWindow::fixOrder() {
	if (_setupEmailLock) _setupEmailLock->raise();
	if (_passcodeLock) _passcodeLock->raise();
	if (_layer) _layer->raise();
	raiseMediaPreview();
}

void MainWindow::raiseMediaPreview() {
	if (_mediaPreview) _mediaPreview->raise();
	if (_testingThemeWarning) _testingThemeWarning->raise();
}

void MainWindow::closeEvent(QCloseEvent *e) {
	if (Core::Sandbox::Instance().isSavingSession() || Core::Quitting()) {
		e->accept();
		Core::Quit();
		return;
	} else if (Core::App().closeNonLastAsync(&controller())) {
		e->accept();
		return;
	}
	e->ignore();
	const auto hasAuth = [&] {
		if (!Core::App().domain().started()) {
			return false;
		}
		for (const auto &[_, account] : Core::App().domain().accounts()) {
			if (account->sessionExists()) {
				return true;
			}
		}
		return false;
	}();
	if (!hasAuth || !hideNoQuit()) {
		Core::Quit();
	}
}

void MainWindow::updateControlsGeometry() {
	Platform::MainWindow::updateControlsGeometry();

	auto body = bodyWidget()->rect();
	if (_passcodeLock) _passcodeLock->setGeometry(body);
	if (_setupEmailLock) _setupEmailLock->setGeometry(body);
	auto mainLeft = 0;
	auto mainWidth = body.width();
	if (const auto session = sessionController()) {
		if (const auto skip = session->filtersWidth()) {
			mainLeft += skip;
			mainWidth -= skip;
		}
	}
	if (_main) {
		_main->setGeometry({
			body.x() + mainLeft,
			body.y(),
			mainWidth,
			body.height() });
	}
	if (_intro) _intro->setGeometry(body);
	if (_layer) _layer->setGeometry(body);
	if (_mediaPreview) _mediaPreview->setGeometry(body);
	if (_testingThemeWarning) _testingThemeWarning->setGeometry(body);

	if (_main) _main->checkMainSectionToLayer();
}

void MainWindow::handleStartFiles(QStringList paths) {
	if (controller().locked()) {
		return;
	}
	Core::App().hideMediaView();
	ui_hideSettingsAndLayer(anim::type::instant);
	if (_main) {
		_main->handleStartFiles(std::move(paths));
	}
}

MainWindow::~MainWindow() = default;
