#include "settings/sections/settings_text_animations.h"

#include "lang/lang_keys.h"
#include "settings/sections/settings_xenon.h"
#include "settings/settings_builder.h"
#include "settings/settings_common_session.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/effects/text_input_animation.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace Settings {
namespace {

using namespace Builder;
namespace Animation = Ui::TextInputAnimation;
using Animation::Mode;
using Animation::Parameter;
using Animation::Phase;

class TextAnimations final : public Section<TextAnimations> {
public:
	TextAnimations(QWidget *parent, not_null<Window::SessionController*> controller);
	[[nodiscard]] rpl::producer<QString> title() override;

};

template <typename Read>
auto Value(Read read) {
	return rpl::single(read()) | rpl::then(
		Animation::Changes() | rpl::map(read)) | rpl::distinct_until_changed();
}

QString ModeTitle(Mode mode) {
	switch (mode) {
	case Mode::Blur: return u"Blur"_q;
	case Mode::Explosion: return u"Explosion"_q;
	case Mode::Slide: return u"Slide"_q;
	}
	Unexpected("Text animation mode.");
}

void AddSlider(SectionBuilder &builder, Parameter parameter) {
	const auto spec = Animation::Spec(parameter);
	builder.addSubsectionTitle({
		.id = u"text_animations/"_q + QString::fromLatin1(spec.key),
		.title = rpl::single(Animation::Title(parameter)),
	});
	builder.add([=](const WidgetContext &ctx) -> SectionBuilder::WidgetToAdd {
		auto control = MakeSliderWithLabel(
			ctx.container,
			st::settingsScale,
			st::settingsScaleLabel,
			st::normalFont->spacew * 2,
			st::settingsScaleLabel.style.font->width(u"4000 dp/s²"_q),
			true);
		const auto slider = control.slider;
		const auto label = control.label;
		slider->setAccessibleName(Animation::Title(parameter));
		slider->setPseudoDiscrete(
			(spec.max - spec.min) / spec.step + 1,
			[=](int index) { return spec.min + index * spec.step; },
			Animation::Value(parameter),
			[=](int value) { Animation::SetValue(parameter, value); });
		Value([=] { return Animation::Value(parameter); }
		) | rpl::on_next([=](int value) {
			label->setText(QString::number(value)
				+ (spec.unit.isEmpty() ? QString() : u" "_q + spec.unit));
			slider->setValue(double(value - spec.min) / (spec.max - spec.min));
		}, slider->lifetime());
		return {
			.widget = std::move(control.widget),
			.margin = st::settingsBigScalePadding,
		};
	});
}

void AddToggle(
		SectionBuilder &builder,
		QString id,
		rpl::producer<QString> title,
		rpl::producer<bool> value,
		Fn<void(bool)> set) {
	const auto button = builder.addButton({
		.id = std::move(id),
		.title = std::move(title),
		.toggled = std::move(value),
	});
	if (button) {
		button->toggledChanges() | rpl::on_next(std::move(set), button->lifetime());
	}
}

void AddPhase(SectionBuilder &builder, Mode mode, Phase phase) {
	AddToggle(
		builder,
		u"text_animations/phase_%1_%2"_q.arg(int(mode)).arg(int(phase)),
		phase == Phase::Removal ? tr::lng_xenon_text_disable_removal()
			: phase == Phase::BlurAppearance ? tr::lng_xenon_text_skip_blur()
			: mode == Mode::Slide ? tr::lng_xenon_text_skip_slide()
			: tr::lng_xenon_text_disable_appearance(),
		Value([=] { return Animation::Disabled(mode, phase); }),
		[=](bool value) { Animation::SetDisabled(mode, phase, value); });
}

const auto kMeta = BuildHelper({
	.id = TextAnimations::Id(),
	.parentId = XenonId(),
	.title = &tr::lng_xenon_text_animations,
	.icon = &st::menuIconChatBubble,
}, [](SectionBuilder &builder) {
	AddToggle(
		builder,
		u"text_animations/enabled"_q,
		tr::lng_xenon_text_enabled(),
		Value([] { return Animation::Enabled(); }),
		[](bool enabled) { Animation::SetEnabled(enabled); });
	builder.addDividerText(tr::lng_xenon_text_about());
	builder.scope([&] {
		builder.add([](const WidgetContext &ctx) -> SectionBuilder::WidgetToAdd {
			auto field = object_ptr<Ui::InputField>(
				ctx.container,
				st::defaultInputField,
				Ui::InputField::Mode::MultiLine,
				tr::lng_xenon_text_preview());
			field->setMinHeight(st::normalFont->height * 3);
			field->setMaxHeight(st::normalFont->height * 6);
			Animation::Attach(field.data());
			return { .widget = std::move(field), .margin = st::settingsBigScalePadding };
		});
		builder.addButton({
			.id = u"text_animations/mode"_q,
			.title = tr::lng_xenon_text_mode(),
			.label = Value([] { return ModeTitle(Animation::CurrentMode()); }),
			.onClick = [controller = builder.controller()] {
				controller->show(Box([](not_null<Ui::GenericBox*> box) {
					const auto options = std::vector<QString>{
						ModeTitle(Mode::Blur),
						ModeTitle(Mode::Explosion),
						ModeTitle(Mode::Slide),
					};
					SingleChoiceBox(box, {
						.title = tr::lng_xenon_text_mode(),
						.options = options,
						.initialSelection = int(Animation::CurrentMode()),
						.callback = [](int index) { Animation::SetMode(Mode(index)); },
					});
				}));
			},
			.keywords = { u"blur"_q, u"explosion"_q, u"slide"_q },
		});
		for (const auto mode : { Mode::Blur, Mode::Explosion, Mode::Slide }) {
			builder.scope([&] {
				AddPhase(builder, mode, Phase::Appearance);
				if (mode == Mode::Slide) AddPhase(builder, mode, Phase::BlurAppearance);
				AddPhase(builder, mode, Phase::Removal);
			}, Value([=] { return Animation::CurrentMode() == mode; }));
		}
		AddSlider(builder, Parameter::Cursor);
		builder.scope([&] {
			AddSlider(builder, Parameter::Fade);
			AddSlider(builder, Parameter::Blur);
			AddSlider(builder, Parameter::BlurDuration);
		}, Value([] {
			return Animation::CurrentMode() != Mode::Slide
				|| !Animation::Disabled(Mode::Slide, Phase::BlurAppearance);
		}));
		builder.scope([&] {
			builder.addDivider();
			for (const auto parameter : {
				Parameter::Force, Parameter::Gravity, Parameter::Bounce,
				Parameter::Spin, Parameter::Shards, Parameter::Direction,
				Parameter::LifetimeMin, Parameter::LifetimeMax,
			}) AddSlider(builder, parameter);
			builder.addDividerText(tr::lng_xenon_text_direction_hint());
			AddToggle(builder, u"text_animations/fall"_q,
				tr::lng_xenon_text_fall(), Value([] { return Animation::Fall(); }),
				[](bool value) { Animation::SetFall(value); });
			AddToggle(builder, u"text_animations/ignore_floor"_q,
				tr::lng_xenon_text_ignore_floor(), Value([] { return Animation::IgnoreFloor(); }),
				[](bool value) { Animation::SetIgnoreFloor(value); });
		}, Value([] { return Animation::CurrentMode() == Mode::Explosion; }));
		builder.scope([&] {
			builder.addDivider();
			AddSlider(builder, Parameter::SlideAngle);
			builder.addDividerText(tr::lng_xenon_text_slide_hint());
			AddSlider(builder, Parameter::DistanceMin);
			AddSlider(builder, Parameter::DistanceMax);
			AddSlider(builder, Parameter::SlideDuration);
		}, Value([] { return Animation::CurrentMode() == Mode::Slide; }));
	}, Value([] { return Animation::Enabled(); }));
});

TextAnimations::TextAnimations(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

rpl::producer<QString> TextAnimations::title() {
	return tr::lng_xenon_text_animations();
}

}

Type TextAnimationsId() {
	return TextAnimations::Id();
}

}
