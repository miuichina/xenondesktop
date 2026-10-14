#include "settings/sections/settings_xenon.h"

#include "ayu/ui/boxes/edit_mark_box.h"
#include "ayu/deleted_messages.h"
#include "base/options.h"
#include "boxes/peers/edit_peer_permissions_box.h"
#include "chat_helpers/compose/compose_show.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/components/sponsored_messages.h"
#include "data/data_document.h"
#include "dialogs/dialogs_entry.h"
#include "history/view/history_view_top_bar_blur.h"
#include "history/view/history_view_top_bar_widget.h"
#include "lang/lang_keys.h"
#include "settings/sections/settings_main.h"
#include "settings/settings_builder.h"
#include "settings/settings_common_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/main_window.h"
#include "window/window_fps_counter.h"
#include "window/window_session_controller.h"

#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

using namespace Builder;

class Xenon final : public Section<Xenon> {
public:
	Xenon(QWidget *parent, not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

};

const auto kMeta = BuildHelper({
	.id = Xenon::Id(),
	.parentId = MainId(),
	.title = &tr::lng_settings_xenon,
	.icon = &st::menuIconStar,
}, [](SectionBuilder &builder) {
	const auto addArchiveToggle = [&](QString id,
			rpl::producer<QString> title,
			std::string key,
			bool value) {
		const auto button = builder.addButton({
			.id = std::move(id),
			.title = std::move(title),
			.toggled = rpl::single(value),
			.keywords = { u"deleted"_q, u"edited"_q, u"history"_q },
		});
		if (button) {
			button->toggledChanges() | rpl::on_next([=](bool checked) {
				Core::App().settings().writePref<bool>(key, checked);
				Core::App().saveSettingsDelayed();
			}, button->lifetime());
		}
	};
	addArchiveToggle(
		u"xenon/save_deleted_messages"_q,
		tr::lng_xenon_save_history(),
		"xenon-save-deleted-messages",
		AyuMessages::SaveDeletedMessages());
	addArchiveToggle(
		u"xenon/save_deleted_for_bots"_q,
		tr::lng_xenon_save_history_bots(),
		"xenon-save-deleted-for-bots",
		AyuMessages::SaveForBots());
	addArchiveToggle(
		u"xenon/transparent_deleted_messages"_q,
		tr::lng_xenon_transparent_deleted(),
		"xenon-transparent-deleted-messages",
		AyuMessages::SemiTransparentDeletedMessages());
	builder.addButton({
		.id = u"xenon/deleted_mark"_q,
		.title = tr::lng_xenon_deleted_mark(),
		.label = AyuMessages::DeletedMarkValue() | rpl::map([](QString mark) {
			return mark.isEmpty()
				? tr::lng_xenon_default_deleted_mark(tr::now)
				: mark;
		}),
		.onClick = [] {
			Ui::show(Box<EditMarkBox>(
				tr::lng_xenon_deleted_mark(),
				AyuMessages::DeletedMark(),
				QString(),
				[](const QString &value) {
					AyuMessages::SetDeletedMark(value);
				}));
		},
	});
	builder.addDividerText(tr::lng_xenon_save_history_about());

	auto &hideAds = base::options::lookup<bool>(Data::kOptionHideAds);
	const auto ads = builder.addButton({
		.id = u"xenon/hide_ads"_q,
		.title = rpl::single(u"Hide ads"_q),
		.toggled = rpl::single(hideAds.value()),
		.keywords = { u"ads"_q, u"sponsored"_q, u"advertising"_q },
	});
	if (ads) {
		ads->toggledChanges() | rpl::on_next([option = &hideAds](bool checked) {
			option->set(checked);
		}, ads->lifetime());
	}
	builder.addDivider();
	builder.add([](const WidgetContext &ctx) -> SectionBuilder::WidgetToAdd {
		auto controls = CreateEditSendConfirmations(
			ctx.container,
			SendConfirmationFlags());
		std::move(controls.changes) | rpl::on_next([](uint32 flags) {
			Core::App().settings().writePref<QByteArray>(
				"xenon-send-confirmations",
				QByteArray::number(flags));
			Core::App().saveSettingsDelayed();
		}, controls.widget->lifetime());
		return { .widget = std::move(controls.widget) };
	});
	builder.addDivider();
	auto &showFps = base::options::lookup<bool>(Window::kOptionShowFps);
	const auto fps = builder.addButton({
		.id = u"xenon/show_fps"_q,
		.title = tr::lng_settings_xenon_show_fps(),
		.toggled = rpl::single(showFps.value()),
		.keywords = { u"fps"_q, u"frames"_q, u"performance"_q },
	});
	if (fps) {
		fps->toggledChanges() | rpl::on_next([option = &showFps](bool checked) {
			option->set(checked);
		}, fps->lifetime());
	}
	builder.addDividerText(tr::lng_settings_xenon_show_fps_about());

	auto &blurEffects = base::options::lookup<bool>(
		HistoryView::kOptionBlurEffects);
	auto &realtimeBlur = base::options::lookup<bool>(HistoryView::kOptionRealtimeBlur);
	const auto blur = builder.addButton({
		.id = u"xenon/blur_effects"_q,
		.title = tr::lng_settings_xenon_blur_effects(),
		.toggled = rpl::single(blurEffects.value()),
		.keywords = { u"blur"_q, u"effects"_q, u"glass"_q },
	});
	if (blur) {
		blur->toggledChanges(
		) | rpl::on_next([option = &blurEffects](bool checked) {
			option->set(checked);
		}, blur->lifetime());
	}
	const auto realtime = builder.addButton({
		.id = u"xenon/realtime_blur"_q,
		.title = tr::lng_settings_xenon_realtime_blur(),
		.toggled = rpl::single(realtimeBlur.value()),
		.keywords = { u"blur"_q, u"live"_q, u"realtime"_q },
		.shown = blur ? blur->toggledValue() : rpl::producer<bool>(nullptr),
	});
	if (realtime) {
		realtime->toggledChanges() | rpl::on_next([option = &realtimeBlur](bool checked) {
			option->set(checked);
		}, realtime->lifetime());
	}
	builder.scope([&] {
		builder.addSubsectionTitle({
			.id = u"xenon/blur_intensity"_q,
			.title = tr::lng_settings_xenon_blur_intensity(),
			.keywords = { u"blur"_q, u"intensity"_q },
		});
		builder.add([](const WidgetContext &ctx) -> SectionBuilder::WidgetToAdd {
			auto &option = base::options::lookup<int>(
				HistoryView::kOptionBlurIntensity);
			auto control = MakeSliderWithLabel(
				ctx.container,
				st::settingsScale,
				st::settingsScaleLabel,
				st::normalFont->spacew * 2,
				st::settingsScaleLabel.style.font->width(u"200%"_q),
				true);
			const auto slider = control.slider;
			const auto label = control.label;
			slider->setAccessibleName(
				tr::lng_settings_xenon_blur_intensity(tr::now));
			slider->setPseudoDiscrete(
				200,
				[](int index) { return index + 1; },
				std::clamp(option.value(), 1, 200),
				[option = &option](int value) { option->set(value); });
			rpl::merge(
				rpl::single(rpl::empty_value()),
				option.changes()
			) | rpl::on_next([label, option = &option] {
				label->setText(QString::number(
					std::clamp(option->value(), 1, 200)) + '%');
			}, slider->lifetime());
			return {
				.widget = std::move(control.widget),
				.margin = st::settingsBigScalePadding,
			};
		});
	}, blur ? blur->toggledValue() : rpl::producer<bool>(nullptr));
	builder.addDivider();

#ifdef Q_OS_WIN
	auto &acrylicOption = base::options::lookup<bool>(Window::kOptionWindowsAcrylic);
	const auto acrylic = builder.addButton({
		.id = u"xenon/windows_acrylic"_q,
		.title = tr::lng_settings_xenon_windows_acrylic(),
		.toggled = rpl::single(acrylicOption.value()),
		.keywords = { u"windows"_q, u"acrylic"_q, u"blur"_q },
	});
	if (acrylic) {
		acrylic->setEnabled(acrylicOption.relevant());
		acrylic->toggledChanges() | rpl::on_next([
			option = &acrylicOption,
			controller = builder.controller()
		](bool checked) {
			option->set(checked);
			controller->show(Ui::MakeConfirmBox({
				.text = tr::lng_settings_need_restart(),
				.confirmed = [] { Core::Restart(); },
				.confirmText = tr::lng_settings_restart_now(),
				.cancelText = tr::lng_settings_restart_later(),
			}));
		}, acrylic->lifetime());
	}
	builder.addDividerText(tr::lng_settings_xenon_windows_acrylic_about());
	builder.scope([&] {
		builder.addSubsectionTitle({
			.id = u"xenon/windows_background_opacity"_q,
			.title = tr::lng_settings_xenon_windows_background_opacity(),
		});
		builder.add([](const WidgetContext &ctx) -> SectionBuilder::WidgetToAdd {
			auto &option = base::options::lookup<int>(
				Window::kOptionWindowsBackgroundOpacity);
			auto control = MakeSliderWithLabel(
				ctx.container,
				st::settingsScale,
				st::settingsScaleLabel,
				st::normalFont->spacew * 2,
				st::settingsScaleLabel.style.font->width(u"100%"_q),
				true);
			const auto slider = control.slider;
			const auto label = control.label;
			slider->setAccessibleName(
				tr::lng_settings_xenon_windows_background_opacity(tr::now));
			slider->setPseudoDiscrete(
				101,
				[](int index) { return index; },
				std::clamp(option.value(), 0, 100),
				[option = &option](int value) { option->set(value); });
			rpl::merge(
				rpl::single(rpl::empty_value()),
				option.changes()
			) | rpl::on_next([label, option = &option] {
				label->setText(QString::number(
					std::clamp(option->value(), 0, 100)) + '%');
			}, slider->lifetime());
			return {
				.widget = std::move(control.widget),
				.margin = st::settingsBigScalePadding,
			};
		});
	}, acrylic ? acrylic->toggledValue() : rpl::producer<bool>(nullptr));
	builder.addDivider();
#endif

	auto &unreadOnTop = base::options::lookup<bool>(
		Dialogs::kOptionDialogsUnreadOnTop);
	const auto main = builder.addButton({
		.id = u"xenon/unread_on_top"_q,
		.title = tr::lng_settings_xenon_unread_on_top(),
		.toggled = rpl::single(unreadOnTop.value()),
		.keywords = { u"unread"_q, u"top"_q, u"chats"_q },
	});
	if (main) {
		main->toggledChanges(
		) | rpl::on_next([option = &unreadOnTop](bool checked) {
			option->set(checked);
		}, main->lifetime());
	}
	builder.addDividerText(tr::lng_settings_xenon_unread_on_top_about());

	auto &includeArchive = base::options::lookup<bool>(
		Dialogs::kOptionDialogsUnreadOnTopIncludeArchive);
	const auto sub = builder.addButton({
		.id = u"xenon/unread_on_top_archive"_q,
		.title = tr::lng_settings_xenon_unread_on_top_archive(),
		.toggled = rpl::single(includeArchive.value()),
		.keywords = { u"archive"_q, u"unread"_q },
		.shown = main
			? main->toggledValue()
			: rpl::producer<bool>(nullptr),
	});
	if (sub) {
		sub->toggledChanges(
		) | rpl::on_next([option = &includeArchive](bool checked) {
			option->set(checked);
		}, sub->lifetime());
	}
	builder.addDividerText(
		tr::lng_settings_xenon_unread_on_top_archive_about());
});

Xenon::Xenon(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

rpl::producer<QString> Xenon::title() {
	return tr::lng_settings_xenon();
}

void Xenon::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

} // namespace

uint32 SendConfirmationFlags() {
	return Core::App().settings().readPref<QByteArray>(
		"xenon-send-confirmations").toUInt();
}

std::optional<SendConfirmation> BundleSendConfirmation(
		const Ui::PreparedBundle &bundle) {
	const auto enabled = SendConfirmationFlags();
	if (bundle.totalCount > 0
		&& (enabled & uint32(SendConfirmation::Media))) {
		return SendConfirmation::Media;
	}
	for (const auto &group : bundle.groups) {
		for (const auto &file : group.list.files) {
			if (!file.information) {
				continue;
			}
			if (file.isSticker()) {
				if (enabled & uint32(SendConfirmation::Stickers)) {
					return SendConfirmation::Stickers;
				}
			} else if (file.isGifv() || file.sendsVideoAsGif()) {
				if (enabled & uint32(SendConfirmation::Gifs)) {
					return SendConfirmation::Gifs;
				}
			} else if (file.information) {
				const auto image = std::get_if<Ui::PreparedFileInformation::Image>(
					&file.information->media);
				if (image && image->animated
					&& (enabled & uint32(SendConfirmation::Gifs))) {
					return SendConfirmation::Gifs;
				}
			}
		}
	}
	return std::nullopt;
}

SendConfirmation DocumentSendConfirmation(not_null<DocumentData*> document) {
	return document->sticker()
		? SendConfirmation::Stickers
		: document->isVoiceMessage()
		? SendConfirmation::Voice
		: document->isAnimation()
		? SendConfirmation::Gifs
		: SendConfirmation::Media;
}

bool ConfirmSend(
		not_null<Window::SessionController*> controller,
		SendConfirmation type,
		bool approved,
		Fn<void()> send) {
	return ConfirmSend(
		controller->uiShow(),
		type,
		approved,
		std::move(send));
}

bool ConfirmSend(
		const std::shared_ptr<Ui::Show> &show,
		SendConfirmation type,
		bool approved,
		Fn<void()> send) {
	const auto enabled = SendConfirmationFlags();
	const auto media = (type != SendConfirmation::Messages)
		&& (enabled & uint32(SendConfirmation::Media));
	if (approved || (!(enabled & uint32(type)) && !media)) {
		return false;
	}
	const auto name = media
		? u"media"_q
		: (type == SendConfirmation::Gifs)
		? u"GIF"_q
		: (type == SendConfirmation::Stickers)
		? u"sticker"_q
		: (type == SendConfirmation::Voice)
		? u"voice message"_q
		: u"message"_q;
	show->show(Ui::MakeConfirmBox({
		.text = u"Send this "_q + name + u"?"_q,
		.confirmed = [send = std::move(send)](Fn<void()> close) {
			close();
			send();
		},
		.confirmText = rpl::single(u"Send"_q),
		.cancelText = rpl::single(u"Cancel"_q),
	}));
	return true;
}

Type XenonId() {
	return Xenon::Id();
}

} // namespace Settings
