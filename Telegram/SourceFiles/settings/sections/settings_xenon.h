#pragma once

#include "settings/settings_type.h"

namespace Ui {
class Show;
struct PreparedBundle;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

class DocumentData;

namespace Settings {

enum class SendConfirmation : uint32 {
	Gifs = 1,
	Stickers = 2,
	Voice = 4,
	Messages = 8,
	Media = 16,
};

[[nodiscard]] uint32 SendConfirmationFlags();
[[nodiscard]] std::optional<SendConfirmation> BundleSendConfirmation(
	const Ui::PreparedBundle &bundle);
[[nodiscard]] bool ConfirmSend(
	const std::shared_ptr<Ui::Show> &show,
	SendConfirmation type,
	bool approved,
	Fn<void()> send);
[[nodiscard]] bool ConfirmSend(
	not_null<Window::SessionController*> controller,
	SendConfirmation type,
	bool approved,
	Fn<void()> send);
[[nodiscard]] SendConfirmation DocumentSendConfirmation(
	not_null<DocumentData*> document);

[[nodiscard]] Type XenonId();

} // namespace Settings
