/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/win/main_window_win.h"

#include "platform/platform_specific.h"
#include "platform/platform_notifications_manager.h"
#include "platform/win/tray_win.h"
#include "platform/win/windows_dlls.h"
#include "platform/win/integration_win.h"
#include "window/notifications_manager.h"
#include "window/window_session_controller.h"
#include "mainwindow.h"
#include "main/main_session.h"
#include "base/crc32hash.h"
#include "base/options.h"
#include "ui/style/style_core_palette.h"
#include "ui/window_palette.h"
#include "ui/painter.h"
#include "ui/image/image_prepare.h"
#include <QtCore/QTimer>
#include <map>
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_dialogs.h"
#include "styles/style_window.h"
#include "base/platform/win/base_windows_wrl.h"
#include "base/platform/base_platform_info.h"
#include "core/application.h"
#include "core/sandbox.h"
#include "lang/lang_keys.h"
#include "storage/localstorage.h"
#include "ui/widgets/popup_menu.h"
#include "ui/ui_utility.h"
#include "window/themes/window_theme.h"
#include "window/window_controller.h"
#include "history/history.h"

#include <QtWidgets/QStyleFactory>
#include <QtWidgets/QApplication>
#include <QtGui/QWindow>
#include <QtGui/QScreen>
#include <QtGui/qscreen_platform.h>
#include <QtCore/QOperatingSystemVersion>

#include <Shobjidl.h>
#include <shellapi.h>
#include <WtsApi32.h>
#include <dwmapi.h>

#include <windows.ui.viewmanagement.h>
#include <UIViewSettingsInterop.h>

#include <Windowsx.h>
#include <VersionHelpers.h>

// Taken from qtbase/src/gui/image/qpixmap_win.cpp
HICON qt_pixmapToWinHICON(const QPixmap &);
HBITMAP qt_imageToWinHBITMAP(const QImage &, int hbitmapFormat);

namespace ViewManagement = ABI::Windows::UI::ViewManagement;

namespace Platform {
namespace {

// Mouse down on tray icon deactivates the application.
// So there is no way to know for sure if the tray icon was clicked from
// active application or from inactive application. So we assume that
// if the application was deactivated less than 0.5s ago, then the tray
// icon click (both left or right button) was made from the active app.
constexpr auto kKeepActiveForTrayIcon = crl::time(500);

constexpr auto kWallpaperRefreshDelay = crl::time(80);

rpl::event_stream<> WallpaperPositionChanged;

thread_local QWidget *WallpaperPaintWidget = nullptr;
thread_local auto WallpaperRasterPaint = false;
thread_local auto WallpaperRasterTransform = QTransform();

// QWidget geometry can lag behind WM_MOVE during the native move loop.
QPoint WallpaperGlobalPosition(QWidget *widget) {
	const auto top = widget->window();
	// Hidden popup snapshots use the requested Qt position before the native
	// popup has moved there. Only main-window dragging needs native coordinates.
	if (!dynamic_cast<MainWindow*>(top)) {
		return widget->mapToGlobal(QPoint());
	}
	auto point = POINT{};
	// Never force native creation from WM_MOVE: Windows sends it while
	// Qt is still constructing the platform window.
	const auto hwnd = reinterpret_cast<HWND>(top->internalWinId());
	const auto screen = top->screen();
	const auto native = screen
		? screen->nativeInterface<QNativeInterface::QWindowsScreen>() : nullptr;
	auto monitor = MONITORINFO{ .cbSize = sizeof(MONITORINFO) };
	if (!hwnd || !ClientToScreen(hwnd, &point) || !native
		|| !GetMonitorInfo(native->handle(), &monitor)) {
		return widget->mapToGlobal(QPoint());
	}
	const auto ratio = top->devicePixelRatioF();
	return screen->geometry().topLeft() + QPoint(
		qRound((point.x - monitor.rcMonitor.left) / ratio),
		qRound((point.y - monitor.rcMonitor.top) / ratio))
		+ widget->mapTo(top, QPoint());
}

class WallpaperBackground final {
public:
	void invalidate() {
		_loaded = false;
		_image = QImage();
		_tints.clear();
	}

	QBrush brush(QWidget *widget, QColor color, int minimumOpacity) {
		if (base::options::value<int>(Window::kOptionWallpaperTintOpacity) >= 100) {
			return QBrush(color);
		}
		if (!_loaded) {
			rebuild();
		}
		if (_image.isNull()) {
			return QBrush(color);
		}
		const auto opacity = std::max(minimumOpacity, std::clamp(
			base::options::value<int>(Window::kOptionWallpaperTintOpacity), 0, 100));
		const auto key = std::pair(color.rgba(), opacity);
		auto i = _tints.find(key);
		if (i == _tints.end()) {
			auto image = _image.copy();
			QPainter p(&image);
			color.setAlphaF(opacity / 100.);
			p.fillRect(image.rect(), color);
			p.end();
			i = _tints.emplace(key, QPixmap::fromImage(image)).first;
		}
		auto result = QBrush(i->second);
		const auto origin = _geometry.topLeft() - WallpaperGlobalPosition(widget);
		auto transform = QTransform();
		transform.translate(origin.x(), origin.y());
		transform.scale(
			_geometry.width() / double(_image.width()),
			_geometry.height() / double(_image.height()));
		result.setTransform(transform);
		return result;
	}

private:
	void rebuild() {
		_loaded = true;
		Microsoft::WRL::ComPtr<IDesktopWallpaper> desktop;
		if (FAILED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr,
				CLSCTX_ALL, IID_PPV_ARGS(&desktop)))) {
			return;
		}
		_geometry = QRect();
		for (const auto screen : QGuiApplication::screens()) {
			_geometry = _geometry.united(screen->geometry());
		}
		if (_geometry.isEmpty()) {
			return;
		}
		const auto blur = std::clamp(base::options::value<int>(
			Window::kOptionWallpaperBlur), 0, 100);
		// Small radii need the original detail; a quarter-size texture would
		// impose four-pixel blocks even when almost no blur was requested.
		const auto scale = (blur >= 16) ? 0.25 : (blur >= 8) ? 0.5 : 1.;
		_image = QImage(QSize(
			std::max(1, int(std::ceil(_geometry.width() * scale))),
			std::max(1, int(std::ceil(_geometry.height() * scale)))),
			QImage::Format_RGB32);
		auto background = COLORREF(0);
		desktop->GetBackgroundColor(&background);
		_image.fill(QColor(GetRValue(background), GetGValue(background),
			GetBValue(background)));
		auto position = DWPOS_FILL;
		desktop->GetPosition(&position);
		auto count = UINT(0);
		desktop->GetMonitorDevicePathCount(&count);
		auto painted = false;
		QPainter p(&_image);
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		p.scale(scale, scale);
		p.translate(-_geometry.topLeft());
		for (const auto screen : QGuiApplication::screens()) {
			const auto native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
			auto monitor = MONITORINFO{ .cbSize = sizeof(MONITORINFO) };
			if (!native || !GetMonitorInfo(native->handle(), &monitor)) {
				continue;
			}
			for (auto index = UINT(0); index != count; ++index) {
				auto path = LPWSTR(nullptr);
				if (FAILED(desktop->GetMonitorDevicePathAt(index, &path))) {
					continue;
				}
				auto rect = RECT{};
				const auto bounds = desktop->GetMonitorRECT(path, &rect);
				auto wallpaper = LPWSTR(nullptr);
				if (SUCCEEDED(bounds) && EqualRect(&rect, &monitor.rcMonitor)) {
					desktop->GetWallpaper(path, &wallpaper);
				}
				CoTaskMemFree(path);
				if (!wallpaper) {
					continue;
				}
				const auto source = QImage(QString::fromWCharArray(wallpaper));
				CoTaskMemFree(wallpaper);
				if (source.isNull()) {
					break;
				}
				painted = true;
				const auto area = QRectF(screen->geometry());
				const auto target = (position == DWPOS_SPAN)
					? QRectF(_geometry) : area;
				p.save();
				p.setClipRect(area);
				if (position == DWPOS_TILE) {
					const auto tile = QSizeF(source.size()) / screen->devicePixelRatio();
					for (auto y = area.top(); y < area.bottom(); y += tile.height()) {
						for (auto x = area.left(); x < area.right(); x += tile.width()) {
							p.drawImage(QRectF(QPointF(x, y), tile), source);
						}
					}
				} else {
					auto size = QSizeF(source.size()) / screen->devicePixelRatio();
					if (position == DWPOS_STRETCH) {
						size = target.size();
					} else if (position != DWPOS_CENTER) {
						size.scale(target.size(), position == DWPOS_FIT
							? Qt::KeepAspectRatio : Qt::KeepAspectRatioByExpanding);
					}
					p.drawImage(QRectF(target.center() - QPointF(
						size.width() / 2, size.height() / 2), size), source);
				}
				p.restore();
				break;
			}
		}
		p.end();
		if (!painted) {
			_image = QImage();
			return;
		}
		if (blur) {
			_image = Images::BlurLargeImage(std::move(_image),
				std::max(1, int(std::round(blur * scale))));
		}
		if (_image.size() != _geometry.size()) {
			// Reconstruct once, rather than letting texture brushes enlarge it
			// with nearest-neighbour sampling on every widget.
			_image = _image.scaled(_geometry.size(), Qt::IgnoreAspectRatio,
				Qt::SmoothTransformation);
		}
	}

	bool _loaded = false;
	QImage _image;
	QRect _geometry;
	std::map<std::pair<QRgb, int>, QPixmap> _tints;

};

using namespace Microsoft::WRL;

// Taken from qtbase/src/gui/image/qpixmap_win.cpp
enum HBitmapFormat {
	HBitmapNoAlpha,
	HBitmapPremultipliedAlpha,
	HBitmapAlpha
};

class EventFilter final : public QAbstractNativeEventFilter {
public:
	explicit EventFilter(not_null<MainWindow*> window);

private:
	bool nativeEventFilter(
		const QByteArray &eventType,
		void *message,
		native_event_filter_result *result) override;

	bool mainWindowEvent(
		HWND hWnd,
		UINT msg,
		WPARAM wParam,
		LPARAM lParam,
		LRESULT *result);

	const not_null<MainWindow*> _window;

};


[[nodiscard]] HICON NativeIcon(const QIcon &icon, QSize size) {
	if (!icon.isNull()) {
		const auto pixmap = icon.pixmap(icon.actualSize(size));
		if (!pixmap.isNull()) {
			return qt_pixmapToWinHICON(pixmap);
		}
	}
	return nullptr;
}

struct RealSize {
	QSize value;
	bool maximized = false;
};
[[nodiscard]] RealSize DetectRealSize(HWND hwnd) {
	auto result = RECT();
	auto placement = WINDOWPLACEMENT();
	if (!GetWindowPlacement(hwnd, &placement)) {
		return {};
	} else if (placement.flags & WPF_RESTORETOMAXIMIZED) {
		const auto monitor = MonitorFromRect(
			&placement.rcNormalPosition,
			MONITOR_DEFAULTTONULL);
		if (!monitor) {
			return {};
		}
		auto info = MONITORINFO{ .cbSize = sizeof(MONITORINFO) };
		if (!GetMonitorInfo(monitor, &info)) {
			return {};
		}
		result = info.rcWork;
	} else {
		CopyRect(&result, &placement.rcNormalPosition);
	}
	return {
		{ int(result.right - result.left), int(result.bottom - result.top) },
		((placement.flags & WPF_RESTORETOMAXIMIZED) != 0)
	};
}

[[nodiscard]] QImage PrepareLogoPreview(
		QSize size,
		QImage::Format format,
		int radius = 0) {
	auto result = QImage(size, QImage::Format_RGB32);
	result.fill(st::windowBg->c);

	const auto logo = Window::Logo();
	const auto width = size.width();
	const auto height = size.height();
	const auto side = logo.width();
	const auto skip = width / 8;
	const auto use = std::min({ width - skip, height - skip, side });
	auto p = QPainter(&result);
	if (use == side) {
		p.drawImage((width - side) / 2, (height - side) / 2, logo);
	} else {
		const auto scaled = logo.scaled(
			use,
			use,
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
		p.drawImage((width - use) / 2, (height - use) / 2, scaled);
	}
	p.end();

	return radius
		? Images::Round(std::move(result), Images::CornersMask(radius))
		: result;
}

EventFilter::EventFilter(not_null<MainWindow*> window) : _window(window) {
}

bool EventFilter::nativeEventFilter(
		const QByteArray &eventType,
		void *message,
		native_event_filter_result *result) {
	return Core::Sandbox::Instance().customEnterFromEventLoop([&] {
		const auto msg = static_cast<MSG*>(message);
		if (msg->hwnd == _window->psHwnd()
			|| msg->hwnd && !_window->psHwnd()) {
			return mainWindowEvent(
				msg->hwnd,
				msg->message,
				msg->wParam,
				msg->lParam,
				(LRESULT*)result);
		}
		return false;
	});
}

bool EventFilter::mainWindowEvent(
		HWND hWnd,
		UINT msg,
		WPARAM wParam,
		LPARAM lParam,
		LRESULT *result) {
	switch (msg) {

	case WM_DESTROY: {
		_window->destroyedFromSystem();
	} return false;

	case WM_ACTIVATE: {
		if (LOWORD(wParam) != WA_INACTIVE) {
			_window->shadowsActivate();
		} else {
			_window->shadowsDeactivate();
		}
	} return false;

	case WM_SIZE: {
		if (wParam == SIZE_MAXIMIZED || wParam == SIZE_RESTORED || wParam == SIZE_MINIMIZED) {
			if (wParam == SIZE_RESTORED && _window->windowState() == Qt::WindowNoState) {
				_window->positionUpdated();
			}
		}
	} return false;

	case WM_SETTINGCHANGE:
	case WM_DISPLAYCHANGE: {
		_window->refreshWallpaper();
	} return false;

	case WM_ENTERSIZEMOVE: {
		_window->repaintWallpaperPosition();
	} return false;

	case WM_MOVE: {
		_window->positionUpdated();
		if (Window::UseWallpaperBackdrop()) {
			_window->repaintWallpaperPosition();
		}
	} return false;

	case WM_DWMSENDICONICTHUMBNAIL: {
		if (!Core::App().passcodeLocked()) {
			return false;
		}
		const auto size = QSize(int(HIWORD(lParam)), int(LOWORD(lParam)));
		return _window->setDwmThumbnail(size);
	}

	case WM_DWMSENDICONICLIVEPREVIEWBITMAP: {
		if (!Core::App().passcodeLocked()) {
			return false;
		}
		const auto size = DetectRealSize(hWnd);
		const auto radius = size.maximized ? 0 : style::ConvertScale(8);
		return _window->setDwmPreview(size.value, radius);
	}

	}
	return false;
}

[[nodiscard]] QString HumanReadableDisplayName(HMONITOR hMonitor) {
	// https://github.com/qt/qtbase/commit/6136b92f540c15835e0c7eb7e01ab7b58fbea685
	auto monitorInfo = MONITORINFOEX{};
	monitorInfo.cbSize = sizeof(MONITORINFOEX);
	if (!GetMonitorInfo(hMonitor, &monitorInfo)) {
		return QString();
	}

	// Try Display Configuration API first (Windows 7+).
	auto numPathArrayElements = UINT32(0);
	auto numModeInfoArrayElements = UINT32(0);
	if (GetDisplayConfigBufferSizes(
			QDC_ONLY_ACTIVE_PATHS,
			&numPathArrayElements,
			&numModeInfoArrayElements) != ERROR_SUCCESS) {
		return QString();
	}

	auto pathInfos = std::vector<DISPLAYCONFIG_PATH_INFO>(
		numPathArrayElements);
	auto modeInfos = std::vector<DISPLAYCONFIG_MODE_INFO>(
		numModeInfoArrayElements);

	if (QueryDisplayConfig(
			QDC_ONLY_ACTIVE_PATHS,
			&numPathArrayElements,
			pathInfos.data(),
			&numModeInfoArrayElements,
			modeInfos.data(),
			nullptr) != ERROR_SUCCESS) {
		return QString();
	}

	// Find matching path.
	for (const auto &path : pathInfos) {
		auto deviceName = DISPLAYCONFIG_SOURCE_DEVICE_NAME{};
		deviceName.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(
			DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME);
		deviceName.header.size = sizeof(DISPLAYCONFIG_SOURCE_DEVICE_NAME);
		deviceName.header.adapterId = path.sourceInfo.adapterId;
		deviceName.header.id = path.sourceInfo.id;

		if (DisplayConfigGetDeviceInfo(&deviceName.header) != ERROR_SUCCESS
			|| wcscmp(
				monitorInfo.szDevice,
				deviceName.viewGdiDeviceName) != 0) {
			continue;
		}

		auto targetName = DISPLAYCONFIG_TARGET_DEVICE_NAME{};
		targetName.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(
			DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME);
		targetName.header.size = sizeof(DISPLAYCONFIG_TARGET_DEVICE_NAME);
		targetName.header.adapterId = path.targetInfo.adapterId;
		targetName.header.id = path.targetInfo.id;

		if (DisplayConfigGetDeviceInfo(&targetName.header) == ERROR_SUCCESS) {
			const auto friendlyName = QString::fromWCharArray(
				targetName.monitorFriendlyDeviceName);
			if (!friendlyName.isEmpty()) {
				return friendlyName;
			}
		}
	}

	// Fallback to legacy method.
	auto displayDevice = DISPLAY_DEVICE{};
	displayDevice.cb = sizeof(DISPLAY_DEVICE);
	for (auto deviceIndex = DWORD(0);
			EnumDisplayDevices(
				monitorInfo.szDevice,
				deviceIndex,
				&displayDevice,
				0);
			deviceIndex++) {
		if (!(displayDevice.StateFlags & DISPLAY_DEVICE_ACTIVE)) {
			continue;
		}
		const auto deviceName = QString::fromWCharArray(
			displayDevice.DeviceString);
		if (!deviceName.isEmpty()
			&& deviceName != u"Generic PnP Monitor"_q) {
			return deviceName;
		}
		break;
	}

	return QString();
}

} // namespace

struct MainWindow::Private {
	explicit Private(not_null<MainWindow*> window) : filter(window) {
	}

	EventFilter filter;
	WallpaperBackground wallpaper;
	QTimer wallpaperRefresh;
	bool wallpaperRepainting = false;
	QPoint wallpaperPosition;
	ComPtr<ViewManagement::IUIViewSettings> viewSettings;
};

MainWindow::BitmapPointer::BitmapPointer(HBITMAP value) : _value(value) {
}

MainWindow::BitmapPointer::BitmapPointer(BitmapPointer &&other)
: _value(base::take(other._value)) {
}

MainWindow::BitmapPointer &MainWindow::BitmapPointer::operator=(
		BitmapPointer &&other) {
	if (_value != other._value) {
		reset();
		_value = base::take(other._value);
	}
	return *this;
}

MainWindow::BitmapPointer::~BitmapPointer() {
	reset();
}

HBITMAP MainWindow::BitmapPointer::get() const {
	return _value;
}

MainWindow::BitmapPointer::operator bool() const {
	return _value != nullptr;
}

void MainWindow::BitmapPointer::release() {
	_value = nullptr;
}

void MainWindow::BitmapPointer::reset(HBITMAP value) {
	if (_value != value) {
		if (const auto old = std::exchange(_value, value)) {
			DeleteObject(old);
		}
	}
}

MainWindow::MainWindow(not_null<Window::Controller*> controller)
: Window::MainWindow(controller)
, _private(std::make_unique<Private>(this))
, _taskbarHiderWindow(std::make_unique<QWindow>()) {
	qApp->installNativeEventFilter(&_private->filter);

	setupNativeWindowFrame();

	SetWindowPriority(this, controller->isPrimary() ? 2 : 1);

	using namespace rpl::mappers;
	Core::App().appDeactivatedValue(
	) | rpl::distinct_until_changed(
	) | rpl::filter(_1) | rpl::on_next([=] {
		_lastDeactivateTime = crl::now();
	}, lifetime());

	setupPreviewPasscodeLock();
	_private->wallpaperRefresh.setSingleShot(true);
	_private->wallpaperRefresh.setInterval(kWallpaperRefreshDelay);
	QObject::connect(&_private->wallpaperRefresh, &QTimer::timeout, this, [=] {
		_private->wallpaper.invalidate();
		Ui::ForceFullRepaint(this);
	});
	rpl::merge(
		base::options::lookup<bool>(Window::kOptionWallpaperBackdrop).changes(),
		base::options::lookup<int>(Window::kOptionWallpaperBlur).changes(),
		base::options::lookup<int>(Window::kOptionWallpaperTintOpacity).changes(),
		style::PaletteChanged()
	) | rpl::on_next([=] {
		_private->wallpaperRefresh.start();
	}, lifetime());
	QObject::connect(qApp, &QGuiApplication::screenAdded, this, [=] {
		refreshWallpaper();
	});
	QObject::connect(qApp, &QGuiApplication::screenRemoved, this, [=] {
		refreshWallpaper();
	});

}

void MainWindow::setupPreviewPasscodeLock() {
	Core::App().passcodeLockValue(
	) | rpl::on_next([=](bool locked) {
		// Use iconic bitmap instead of the window content if passcoded.
		BOOL fForceIconic = locked ? TRUE : FALSE;
		BOOL fHasIconicBitmap = fForceIconic;
		DwmSetWindowAttribute(
			_hWnd,
			DWMWA_FORCE_ICONIC_REPRESENTATION,
			&fForceIconic,
			sizeof(fForceIconic));
		DwmSetWindowAttribute(
			_hWnd,
			DWMWA_HAS_ICONIC_BITMAP,
			&fHasIconicBitmap,
			sizeof(fHasIconicBitmap));
	}, lifetime());
}

void MainWindow::setupNativeWindowFrame() {
	auto nativeFrame = rpl::single(
		Core::App().settings().nativeWindowFrame()
	) | rpl::then(
		Core::App().settings().nativeWindowFrameChanges()
	);

	rpl::combine(
		std::move(nativeFrame),
		Window::Theme::IsNightModeValue()
	) | rpl::skip(1) | rpl::on_next([=](bool native, bool night) {
		validateWindowTheme(native, night);
	}, lifetime());
}

void MainWindow::shadowsActivate() {
	_hasActiveFrame = true;
}

void MainWindow::shadowsDeactivate() {
	_hasActiveFrame = false;
}

void MainWindow::destroyedFromSystem() {
	if (!Core::App().closeNonLastAsync(&controller())) {
		Core::Quit();
	}
}

bool MainWindow::setDwmThumbnail(QSize size) {
	validateDwmPreviewColors();
	if (size.isEmpty()) {
		return false;
	} else if (!_dwmThumbnail || _dwmThumbnailSize != size) {
		const auto result = PrepareLogoPreview(size, QImage::Format_RGB32);
		const auto bitmap = qt_imageToWinHBITMAP(result, HBitmapNoAlpha);
		if (!bitmap) {
			return false;
		}
		_dwmThumbnail.reset(bitmap);
		_dwmThumbnailSize = size;
	}
	DwmSetIconicThumbnail(_hWnd, _dwmThumbnail.get(), NULL);
	return true;
}

bool MainWindow::setDwmPreview(QSize size, int radius) {
	Expects(radius >= 0);

	validateDwmPreviewColors();
	if (size.isEmpty()) {
		return false;
	} else if (!_dwmPreview
		|| _dwmPreviewSize != size
		|| _dwmPreviewRadius != radius) {
		const auto format = (radius > 0)
			? QImage::Format_ARGB32_Premultiplied
			: QImage::Format_RGB32;
		const auto result = PrepareLogoPreview(size, format, radius);
		const auto bitmap = qt_imageToWinHBITMAP(
			result,
			(radius > 0) ? HBitmapPremultipliedAlpha : HBitmapNoAlpha);
		if (!bitmap) {
			return false;
		}
		_dwmPreview.reset(bitmap);
		_dwmPreviewRadius = radius;
		_dwmPreviewSize = size;
	}
	const auto flags = 0;
	DwmSetIconicLivePreviewBitmap(_hWnd, _dwmPreview.get(), NULL, flags);
	return true;
}

void MainWindow::validateDwmPreviewColors() {
	if (_dwmBackground == st::windowBg->c) {
		return;
	}
	_dwmBackground = st::windowBg->c;
	_dwmThumbnail.reset();
	_dwmPreview.reset();
}

void MainWindow::forceIconRefresh() {
	const auto refresher = std::make_unique<QWidget>(this);
	refresher->setWindowFlags(
		static_cast<Qt::WindowFlags>(Qt::Tool) | Qt::FramelessWindowHint);
	refresher->setGeometry(x() + 1, y() + 1, 1, 1);
	auto palette = refresher->palette();
	palette.setColor(
		QPalette::Window,
		(isActiveWindow() ? st::titleBgActive : st::titleBg)->c);
	refresher->setPalette(palette);
	refresher->show();
	refresher->raise();
	refresher->activateWindow();

	updateTaskbarAndIconCounters();
}

void MainWindow::workmodeUpdated(Core::Settings::WorkMode mode) {
	using WorkMode = Core::Settings::WorkMode;

	switch (mode) {
	case WorkMode::WindowAndTray: {
		HWND psOwner = (HWND)GetWindowLongPtr(_hWnd, GWLP_HWNDPARENT);
		if (psOwner) {
			SetWindowLongPtr(_hWnd, GWLP_HWNDPARENT, 0);
			windowHandle()->setTransientParent(nullptr);
			forceIconRefresh();
		}
	} break;

	case WorkMode::TrayOnly: {
		HWND psOwner = (HWND)GetWindowLongPtr(_hWnd, GWLP_HWNDPARENT);
		if (!psOwner) {
			const auto hwnd = _taskbarHiderWindow->winId();
			SetWindowLongPtr(_hWnd, GWLP_HWNDPARENT, (LONG_PTR)hwnd);
			windowHandle()->setTransientParent(_taskbarHiderWindow.get());
		}
	} break;

	case WorkMode::WindowOnly: {
		HWND psOwner = (HWND)GetWindowLongPtr(_hWnd, GWLP_HWNDPARENT);
		if (psOwner) {
			SetWindowLongPtr(_hWnd, GWLP_HWNDPARENT, 0);
			windowHandle()->setTransientParent(nullptr);
			forceIconRefresh();
		}
	} break;
	}
}

bool MainWindow::hasTabletView() const {
	if (!_private->viewSettings) {
		return false;
	}
	auto mode = ViewManagement::UserInteractionMode();
	_private->viewSettings->get_UserInteractionMode(&mode);
	return (mode == ViewManagement::UserInteractionMode_Touch);
}

bool MainWindow::initGeometryFromSystem() {
	if (!hasTabletView() || !screen()) {
		return false;
	}
	Ui::RpWidget::setGeometry(screen()->availableGeometry());
	return true;
}

bool MainWindow::nativeEvent(
		const QByteArray &eventType,
		void *message,
		native_event_filter_result *result) {
	if (message) {
		const auto msg = static_cast<MSG*>(message);
		if (msg->message == WM_IME_STARTCOMPOSITION) {
			Core::Sandbox::Instance().customEnterFromEventLoop([&] {
				imeCompositionStartReceived();
			});
		}
	}
	return false;
}

void MainWindow::updateWindowIcon() {
	updateTaskbarAndIconCounters();
}

bool MainWindow::isActiveForTrayMenu() {
	return !_lastDeactivateTime
		|| (_lastDeactivateTime + kKeepActiveForTrayIcon >= crl::now());
}

void MainWindow::unreadCounterChangedHook() {
	updateTaskbarAndIconCounters();
}

void MainWindow::updateTaskbarAndIconCounters() {
	const auto counter = Core::App().unreadBadge();
	const auto muted = Core::App().unreadBadgeMuted();
	const auto controller = sessionController();
	const auto session = controller ? &controller->session() : nullptr;

	const auto iconSizeSmall = QSize(
		GetSystemMetrics(SM_CXSMICON),
		GetSystemMetrics(SM_CYSMICON));
	const auto iconSizeBig = QSize(
		GetSystemMetrics(SM_CXICON),
		GetSystemMetrics(SM_CYICON));
	const auto supportMode = session && session->supportMode();

	auto iconSmallPixmap16 = Tray::IconWithCounter(
		Tray::CounterLayerArgs(16, counter, muted),
		true,
		false,
		supportMode);
	auto iconSmallPixmap32 = Tray::IconWithCounter(
		Tray::CounterLayerArgs(32, counter, muted),
		true,
		false,
		supportMode);
	QIcon iconSmall, iconBig;
	iconSmall.addPixmap(iconSmallPixmap16);
	iconSmall.addPixmap(iconSmallPixmap32);
	const auto integration = &Platform::WindowsIntegration::Instance();
	const auto taskbarList = integration->taskbarList();
	const auto bigCounter = taskbarList ? 0 : counter;
	iconBig.addPixmap(Tray::IconWithCounter(
		Tray::CounterLayerArgs(32, bigCounter, muted),
		false,
		false,
		supportMode));
	iconBig.addPixmap(Tray::IconWithCounter(
		Tray::CounterLayerArgs(64, bigCounter, muted),
		false,
		false,
		supportMode));

	destroyCachedIcons();
	_iconSmall = NativeIcon(iconSmall, iconSizeSmall);
	_iconBig = NativeIcon(iconBig, iconSizeBig);
	SendMessage(_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)_iconSmall);
	SendMessage(_hWnd, WM_SETICON, ICON_BIG, (LPARAM)(_iconBig ? _iconBig : _iconSmall));
	if (taskbarList) {
		if (counter > 0) {
			const auto pixmap = [&](int size) {
				return Ui::PixmapFromImage(Window::GenerateCounterLayer(
					Tray::CounterLayerArgs(size, counter, muted)));
			};
			QIcon iconOverlay;
			iconOverlay.addPixmap(pixmap(16));
			iconOverlay.addPixmap(pixmap(32));
			_iconOverlay = NativeIcon(iconOverlay, iconSizeSmall);
		}
		const auto description = (counter > 0)
			? tr::lng_unread_bar(tr::now, lt_count, counter).toStdWString()
			: std::wstring();
		taskbarList->SetOverlayIcon(_hWnd, _iconOverlay, description.c_str());
	}
	SetWindowPos(_hWnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void MainWindow::initHook() {
	_hWnd = reinterpret_cast<HWND>(winId());
	if (!_hWnd) {
		return;
	}

	WTSRegisterSessionNotification(_hWnd, NOTIFY_FOR_THIS_SESSION);

	using namespace base::Platform;
	auto factory = ComPtr<IUIViewSettingsInterop>();
	if (SupportsWRL()) {
		ABI::Windows::Foundation::GetActivationFactory(
			StringReferenceWrapper(
				RuntimeClass_Windows_UI_ViewManagement_UIViewSettings).Get(),
			&factory);
		if (factory) {
			// NB! No such method (or IUIViewSettingsInterop) in C++/WinRT :(
			factory->GetForWindow(
				_hWnd,
				IID_PPV_ARGS(&_private->viewSettings));
		}
	}

	validateWindowTheme(
		Core::App().settings().nativeWindowFrame(),
		Window::Theme::IsNightMode());
}

rpl::producer<> WallpaperPositionChanges() {
	return WallpaperPositionChanged.events();
}

void MainWindow::repaintWallpaperPosition() {
	if (!Window::UseWallpaperBackdrop() || _private->wallpaperRepainting
		|| !psHwnd() || !isVisible()
		|| !windowHandle() || !windowHandle()->handle()) {
		return;
	}
	const auto position = WallpaperGlobalPosition(this);
	if (_private->wallpaperPosition == position) {
		return;
	}
	_private->wallpaperPosition = position;
	_private->wallpaperRepainting = true;
	WallpaperPositionChanged.fire({});
	// Invalidate Qt's backing store too: WM_PAINT alone may reuse its pixels.
	for (const auto child : findChildren<QWidget*>()) {
		if (child->isVisible()) {
			child->update();
		}
	}
	repaint();
	_private->wallpaperRepainting = false;
}

void MainWindow::refreshWallpaper() {
	_private->wallpaperRefresh.start();
}

QBrush MainWindow::wallpaperBrush(QWidget *widget, QColor color, int minimumOpacity) {
	return _private->wallpaper.brush(widget, color, minimumOpacity);
}

namespace {

MainWindow *WallpaperWindow(QWidget *widget) {
	while (widget) {
		if (const auto window = dynamic_cast<MainWindow*>(widget)) {
			return window;
		}
		if (const auto parent = widget->parentWidget()) {
			widget = parent;
		} else {
			const auto handle = widget->windowHandle();
			const auto transient = handle ? handle->transientParent() : nullptr;
			widget = transient ? QWidget::find(transient->winId()) : nullptr;
		}
	}
	return nullptr;
}

QBrush ResolveWallpaperBrush(
		const QPainter &p,
		const QBrush &brush,
		const style::color *source) {
	if (WallpaperPaintWidget && source) {
		const auto titleFill = source->get() == st::titleButtonBg.get()
			|| source->get() == st::titleButtonBgActive.get()
			|| source->get() == st::titleButtonCloseBg.get()
			|| source->get() == st::titleButtonCloseBgActive.get();
		if (titleFill) {
			const auto window = WallpaperWindow(WallpaperPaintWidget);
			const auto title = window ? window->titleWidget() : nullptr;
			for (auto widget = WallpaperPaintWidget; widget; widget = widget->parentWidget()) {
				if (widget == title && (Window::UseWallpaperBackdrop()
					|| widget->property("chatBarBlurActive").toBool())) {
					return QBrush(Qt::NoBrush);
				}
			}
		}
	}
	if (!WallpaperPaintWidget || !Window::UseWallpaperBackdrop()
		|| (!dynamic_cast<const QWidget*>(p.device()) && !WallpaperRasterPaint)) {
		return brush;
	}
	// Dimming overlays must darken the wallpaper without introducing theme hue.
	if (source && (source->get() == st::layerBg.get()
		|| source->get() == st::slideFadeOutBg.get())) {
		return QBrush(QColor(0, 0, 0, brush.color().alpha()));
	}
	const auto hover = [&] {
		for (const auto color : { st::windowBgOver, st::dialogsBgOver,
			st::contactsBgOver, st::menuBgOver,
			st::dialogsBgActive, st::sideBarBgActive, st::lightButtonBgOver }) {
			if ((source && source->get() == color.get())
				|| (!source && brush.style() == Qt::SolidPattern
					&& brush.color() == color->c)) {
				return true;
			}
		}
		return false;
	}();
	const auto background = [&] {
		for (const auto color : {
			st::windowBg, st::dialogsBg, st::boxBg, st::boxDividerBg,
			st::historyComposeAreaBg, st::mainMenuBg, st::topBarBg,
			st::titleBg, st::titleBgActive, st::contactsBg,
			st::sideBarBg, st::introBg, st::menuBg,
			st::emojiPanBg, st::emojiPanCategories, st::emojiPanHeaderBg,
			st::historyPinnedBg, st::historyReplyBg, st::historyComposeButtonBg,
			st::filterInputInactiveBg, st::filterInputActiveBg,
			st::mentionBg, st::lightButtonBg, st::searchedBarBg,
		}) {
			if (source && source->get() == color.get()) {
				return true;
			}
		}
		return !source && brush.style() == Qt::SolidPattern
			&& brush.color() == st::dialogsBg->c;
	}();
	if (!background && !hover) {
		return brush;
	}
	const auto device = dynamic_cast<const QWidget*>(p.device());
	const auto widget = device ? const_cast<QWidget*>(device) : WallpaperPaintWidget;
	const auto window = WallpaperWindow(widget);
	if (!window) {
		return brush;
	}
	// Use one base tint across panels; preserve distinct hover/selection states.
	auto result = window->wallpaperBrush(widget,
		hover ? brush.color() : st::windowBg->c, hover ? 30 : 0);
	result.setTransform(result.transform()
		* (device ? QTransform() : WallpaperRasterTransform)
		* p.worldTransform().inverted());
	return result;
}

} // namespace

WallpaperPaintScope::WallpaperPaintScope(
		QWidget *widget,
		bool raster,
		QTransform transform)
: _previous(WallpaperPaintWidget)
, _previousRaster(WallpaperRasterPaint)
, _previousTransform(WallpaperRasterTransform) {
	WallpaperPaintWidget = widget;
	WallpaperRasterPaint = raster;
	WallpaperRasterTransform = transform;
	Ui::BackgroundBrushOverride = &ResolveWallpaperBrush;
	Ui::BackgroundRenderOverride = [](QWidget *widget, QTransform transform)
			-> std::unique_ptr<Ui::BackgroundRenderScope> {
		return std::make_unique<WallpaperPaintScope>(widget, true, transform);
	};
	Ui::BackgroundUsesDeviceCoordinates = [](QWidget *widget) {
		return Window::UseWallpaperBackdrop() && WallpaperWindow(widget);
	};
}

WallpaperPaintScope::~WallpaperPaintScope() {
	WallpaperPaintWidget = _previous;
	WallpaperRasterPaint = _previousRaster;
	WallpaperRasterTransform = _previousTransform;
}

bool PaintWallpaperBackground(QPainter &p, QRect area, QColor tint) {
	if (!WallpaperPaintWidget || !Window::UseWallpaperBackdrop()
		|| (!dynamic_cast<const QWidget*>(p.device()) && !WallpaperRasterPaint)) {
		return false;
	}
	const auto window = WallpaperWindow(WallpaperPaintWidget);
	if (!window) {
		return false;
	}
	auto brush = window->wallpaperBrush(WallpaperPaintWidget, st::windowBg->c);
	brush.setTransform(brush.transform() * WallpaperRasterTransform * p.worldTransform().inverted());
	p.fillRect(area, brush);
	return true;
}

void MainWindow::validateWindowTheme(bool native, bool night) {
	if (!IsWindows8OrGreater()) {
		const auto empty = native ? nullptr : L" ";
		SetWindowTheme(_hWnd, empty, empty);
		QApplication::setStyle(QStyleFactory::create(u"Windows"_q));
#if 0
	} else if (!Core::App().settings().systemDarkMode().has_value()/*
		|| (!Dlls::AllowDarkModeForApp && !Dlls::SetPreferredAppMode)
		|| !Dlls::AllowDarkModeForWindow
		|| !Dlls::RefreshImmersiveColorPolicyState
		|| !Dlls::FlushMenuThemes*/) {
		return;
#endif
	} else if (!native) {
		SetWindowTheme(_hWnd, nullptr, nullptr);
		return;
	}

	// See "https://github.com/microsoft/terminal/blob/"
	// "eb480b6bbbd83a2aafbe62992d360838e0ab9da5/"
	// "src/interactivity/win32/windowtheme.cpp#L43-L63"

	auto darkValue = BOOL(night ? TRUE : FALSE);

	const auto updateStyle = [&] {
		static const auto kSystemVersion = QOperatingSystemVersion::current();
		if (kSystemVersion.microVersion() >= 18875 && Dlls::SetWindowCompositionAttribute) {
			Dlls::WINDOWCOMPOSITIONATTRIBDATA data = {
				Dlls::WINDOWCOMPOSITIONATTRIB::WCA_USEDARKMODECOLORS,
				&darkValue,
				sizeof(darkValue)
			};
			Dlls::SetWindowCompositionAttribute(_hWnd, &data);
		} else if (kSystemVersion.microVersion() >= 17763) {
			static const auto kDWMWA_USE_IMMERSIVE_DARK_MODE = (kSystemVersion.microVersion() >= 18985)
				? DWORD(20)
				: DWORD(19);
			DwmSetWindowAttribute(
				_hWnd,
				kDWMWA_USE_IMMERSIVE_DARK_MODE,
				&darkValue,
				sizeof(darkValue));
		}
	};

	updateStyle();

	// See "https://osdn.net/projects/tortoisesvn/scm/svn/blobs/28812/"
	// "trunk/src/TortoiseIDiff/MainWindow.cpp"
	//
	// But for now it works event with a small part of that.
	//

	//const auto updateWindowTheme = [&] {
	//	const auto set = [&](LPCWSTR name) {
	//		return SetWindowTheme(_hWnd, name, nullptr);
	//	};
	//	if (!night || FAILED(set(L"DarkMode_Explorer"))) {
	//		set(L"Explorer");
	//	}
	//};
	//
	//if (night) {
	//	if (Dlls::SetPreferredAppMode) {
	//		Dlls::SetPreferredAppMode(Dlls::PreferredAppMode::AllowDark);
	//	} else {
	//		Dlls::AllowDarkModeForApp(TRUE);
	//	}
	//	Dlls::AllowDarkModeForWindow(_hWnd, TRUE);
	//	updateWindowTheme();
	//	updateStyle();
	//	Dlls::FlushMenuThemes();
	//	Dlls::RefreshImmersiveColorPolicyState();
	//} else {
	//	updateWindowTheme();
	//	Dlls::AllowDarkModeForWindow(_hWnd, FALSE);
	//	updateStyle();
	//	Dlls::FlushMenuThemes();
	//	Dlls::RefreshImmersiveColorPolicyState();
	//	if (Dlls::SetPreferredAppMode) {
	//		Dlls::SetPreferredAppMode(Dlls::PreferredAppMode::Default);
	//	} else {
	//		Dlls::AllowDarkModeForApp(FALSE);
	//	}
	//}

	// Didn't find any other way to definitely repaint with the new style.
	SendMessage(_hWnd, WM_NCACTIVATE, _hasActiveFrame ? 0 : 1, 0);
	SendMessage(_hWnd, WM_NCACTIVATE, _hasActiveFrame ? 1 : 0, 0);
}

HWND MainWindow::psHwnd() const {
	return _hWnd;
}

void MainWindow::destroyCachedIcons() {
	const auto destroy = [](HICON &icon) {
		if (icon) {
			DestroyIcon(icon);
			icon = nullptr;
		}
	};
	destroy(_iconBig);
	destroy(_iconSmall);
	destroy(_iconOverlay);
}

MainWindow::~MainWindow() {
	WTSUnRegisterSessionNotification(_hWnd);
	_private->viewSettings.Reset();
	destroyCachedIcons();
}

int32 ScreenNameChecksum(const QString &name) {
	constexpr int DeviceNameSize = base::array_size(MONITORINFOEX().szDevice);
	wchar_t buffer[DeviceNameSize] = { 0 };
	if (name.size() < DeviceNameSize) {
		name.toWCharArray(buffer);
	} else {
		memcpy(buffer, name.toStdWString().data(), sizeof(buffer));
	}
	return base::crc32(buffer, sizeof(buffer));
}

int32 ScreenNameChecksum(const QScreen *screen) {
	return ScreenNameChecksum(screen->name());
}

QString ScreenDisplayLabel(const QScreen *screen) {
	if (!screen) {
		return QString();
	}

	const auto geometry = screen->geometry();
	const auto hMonitor = MonitorFromPoint(
		POINT{
			geometry.x() + geometry.width() / 2,
			geometry.y() + geometry.height() / 2,
		},
		MONITOR_DEFAULTTONEAREST);

	const auto displayName = HumanReadableDisplayName(hMonitor);
	if (!displayName.isEmpty()) {
		return displayName;
	}

	const auto name = screen->name();
	const auto genericName = u"\\\\.\\DISPLAY"_q;
	if (name.startsWith(genericName)) {
		const auto displayNum = name.mid(genericName.size());
		return u"Display %1"_q.arg(displayNum);
	}

	return name;
}

} // namespace Platform
