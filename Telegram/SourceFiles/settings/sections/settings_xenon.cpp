#include "settings/sections/settings_xenon.h"

#include "base/options.h"
#include "dialogs/dialogs_entry.h"
#include "lang/lang_keys.h"
#include "settings/sections/settings_main.h"
#include "settings/settings_builder.h"
#include "settings/settings_common_session.h"
#include "ui/widgets/checkbox.h"
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
