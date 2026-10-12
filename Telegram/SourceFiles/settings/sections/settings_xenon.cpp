#include "settings/sections/settings_xenon.h"

#include "base/options.h"
#include "dialogs/dialogs_entry.h"
#include "history/view/history_view_top_bar_widget.h"
#include "lang/lang_keys.h"
#include "settings/sections/settings_main.h"
#include "settings/settings_builder.h"
#include "settings/settings_common_session.h"
#include "window/main_window.h"
#include "window/window_session_controller.h"
#include "ui/boxes/confirm_box.h"
#include "core/application.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/vertical_list.h"
#include "ui/wrap/vertical_layout.h"
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
	auto &blurEffects = base::options::lookup<bool>(
		HistoryView::kOptionBlurEffects);
	const auto blur = builder.addCheckbox({
		.id = u"xenon/blur_effects"_q,
		.title = tr::lng_settings_xenon_blur_effects(),
		.checked = blurEffects.value(),
		.keywords = { u"blur"_q, u"effects"_q, u"glass"_q },
	});
	if (blur) {
		blur->checkedChanges(
		) | rpl::on_next([option = &blurEffects](bool checked) {
			option->set(checked);
		}, blur->lifetime());
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
				st::settingsScaleLabel.style.font->width(u"100%"_q),
				true);
			const auto slider = control.slider;
			const auto label = control.label;
			slider->setAccessibleName(
				tr::lng_settings_xenon_blur_intensity(tr::now));
			slider->setPseudoDiscrete(
				100,
				[](int index) { return index + 1; },
				std::clamp(option.value(), 1, 100),
				[option = &option](int value) { option->set(value); });
			rpl::merge(
				rpl::single(rpl::empty_value()),
				option.changes()
			) | rpl::on_next([label, option = &option] {
				label->setText(QString::number(
					std::clamp(option->value(), 1, 100)) + '%');
			}, slider->lifetime());
			return {
				.widget = std::move(control.widget),
				.margin = st::settingsBigScalePadding,
			};
		});
	}, blur ? blur->checkedValue() : rpl::producer<bool>(nullptr));
	builder.addDivider();

#ifdef Q_OS_WIN
	auto &acrylicOption = base::options::lookup<bool>(Window::kOptionWindowsAcrylic);
	const auto acrylic = builder.addCheckbox({
		.id = u"xenon/windows_acrylic"_q,
		.title = tr::lng_settings_xenon_windows_acrylic(),
		.checked = acrylicOption.value(),
		.keywords = { u"windows"_q, u"acrylic"_q, u"blur"_q },
	});
	if (acrylic) {
		acrylic->setEnabled(acrylicOption.relevant());
		acrylic->checkedChanges() | rpl::on_next([
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
	}, acrylic ? acrylic->checkedValue() : rpl::producer<bool>(nullptr));
	builder.addDivider();
#endif

	auto &unreadOnTop = base::options::lookup<bool>(
		Dialogs::kOptionDialogsUnreadOnTop);
	const auto main = builder.addCheckbox({
		.id = u"xenon/unread_on_top"_q,
		.title = tr::lng_settings_xenon_unread_on_top(),
		.checked = unreadOnTop.value(),
		.keywords = { u"unread"_q, u"top"_q, u"chats"_q },
	});
	if (main) {
		main->checkedChanges(
		) | rpl::on_next([option = &unreadOnTop](bool checked) {
			option->set(checked);
		}, main->lifetime());
	}
	builder.addDividerText(tr::lng_settings_xenon_unread_on_top_about());

	auto &includeArchive = base::options::lookup<bool>(
		Dialogs::kOptionDialogsUnreadOnTopIncludeArchive);
	const auto sub = builder.addCheckbox({
		.id = u"xenon/unread_on_top_archive"_q,
		.title = tr::lng_settings_xenon_unread_on_top_archive(),
		.checked = includeArchive.value(),
		.keywords = { u"archive"_q, u"unread"_q },
		.shown = main
			? main->checkedValue()
			: rpl::producer<bool>(nullptr),
	});
	if (sub) {
		sub->checkedChanges(
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

Type XenonId() {
	return Xenon::Id();
}

} // namespace Settings
