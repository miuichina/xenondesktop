// Message Shot is based on AyuGram Desktop, copyright Radolyn, 2026.
// Distributed under the GNU General Public License, version 3.
#pragma once

#include "data/data_types.h"

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace HistoryView::MessageShot {

enum class Part {
	Date,
	Reactions,
	HeaderDecorations,
	ColorfulReplies,
};

[[nodiscard]] bool Rendering();
[[nodiscard]] bool Hide(Part part);

[[nodiscard]] bool Enabled(not_null<Main::Session*> session);
void SetEnabled(not_null<Main::Session*> session, bool enabled);
[[nodiscard]] rpl::producer<> EnabledChanges(not_null<Main::Session*> session);

void Show(
	not_null<Window::SessionController*> controller,
	MessageIdsList ids,
	Fn<void()> clearSelected);

} // namespace HistoryView::MessageShot
