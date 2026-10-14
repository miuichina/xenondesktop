#pragma once

#include "ayu/data/entities.h"

namespace Ui {
class Show;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

class History;
struct TextWithTags;
struct HistoryMessageEdition;

namespace AyuMessages {

[[nodiscard]] bool SaveDeletedMessages();
[[nodiscard]] bool SaveEditedMessages();
[[nodiscard]] rpl::producer<QString> DeletedMarkValue();
void SetDeletedMark(QString value);
[[nodiscard]] TextWithEntities DeletedMarkText();
void SaveMessageRevision(
	not_null<HistoryItem*> item,
	const HistoryMessageEdition &edition);
void ShowEditedMessages(
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	MsgId messageId);
[[nodiscard]] bool SaveForBots();
[[nodiscard]] bool SemiTransparentDeletedMessages();
[[nodiscard]] QString DeletedMark();
[[nodiscard]] ID DialogId(not_null<PeerData*> peer);
[[nodiscard]] bool IsMessageSavable(not_null<HistoryItem*> item);
void ProcessMessageDelete(not_null<HistoryItem*> item);
[[nodiscard]] std::vector<not_null<HistoryItem*>> RestoreDeletedMessages(
	not_null<History*> history,
	ID minId,
	ID maxId);
void PrepareDeletedReply(not_null<History*> history,
	TextWithTags &text,
	FullReplyTo &replyTo);
void ShowDeletedMessages(not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	ID topicId = 0);
void ClearDeletedMessages(std::shared_ptr<Ui::Show> show,
	not_null<PeerData*> peer,
	ID topicId = 0);

} // namespace AyuMessages
