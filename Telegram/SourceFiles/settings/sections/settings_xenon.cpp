#include "settings/sections/settings_xenon.h"

#include "lang/lang_keys.h"
#include "settings/sections/settings_main.h"
#include "settings/settings_builder.h"
#include "settings/settings_common_session.h"
#include "ui/vertical_list.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_menu_icons.h"

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
	builder.addDividerText(tr::lng_settings_xenon_empty());
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
