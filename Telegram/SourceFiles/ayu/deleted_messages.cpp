// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/deleted_messages.h"

#include "api/api_text_entities.h"
#include "ayu/data/messages_storage.h"
#include "ayu/utils/ayu_mapper.h"
#include "ayu/ui/message_history/history_section.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_forum_topic.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/view/history_view_element.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_edition.h"
#include "iv/iv_rich_page.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/show.h"
#include "ui/text/text_utilities.h"
#include "window/window_session_controller.h"

#include "styles/style_chat.h"
#include "styles/style_layers.h"

namespace AyuMessages {
namespace {

rpl::event_stream<QString> &DeletedMarkChanges() {
	static auto result = rpl::event_stream<QString>();
	return result;
}


bool PrependPseudoReply(not_null<Main::Session*> session,
	not_null<History*> history,
	TextWithTags &textWithTags,
	FullReplyTo &replyTo) {
	if (!replyTo) {
		return false;
	}
	const auto replyItem = session->data().message(replyTo.messageId);
	if (!replyItem || !replyItem->isDeleted()) {
		return false;
	}
	const auto shortify = [&](const QString &text, int maxLength) {
		if (text.isEmpty() || text.length() < maxLength) {
			return text;
		}
		return text.left(maxLength - 1) + QChar(8230);	// …
	};
	const auto shiftEntities
		= [&](QVector<TextWithTags::Tag> &tags, int offset) {
			  if (tags.isEmpty() || !offset) {
				  return;
			  }
			  for (auto &tag : tags) {
				  tag.offset += offset;
			  }
		  };

	const auto from = replyItem->from();
	auto name = QString();
	if (!history->peer->isUser()
		|| replyItem->history()->peer != history->peer) {
		name = from->name();
	}

	auto msgText = !replyTo.quote.empty() ? replyTo.quote.text
										  : replyItem->originalText().text;
	if (msgText.isEmpty()) {
		msgText = replyItem->notificationText().text;
	}
	const auto shortifiedText = shortify(msgText, 100);

	const auto prefix
		= name.isEmpty() ? shortifiedText : (name + "\n" + shortifiedText);

	if (textWithTags.empty()) {
		textWithTags.text = prefix;
	} else {
		textWithTags.text.prepend(prefix + "\n");
	}
	const auto prefixLength = prefix.length()
		+ (textWithTags.text.length() > prefix.length() ? 1 : 0);

	shiftEntities(textWithTags.tags, prefixLength);

	EntitiesInText newEntities;
	const auto nameLength = int(name.length());

	newEntities.push_back(
		EntityInText{EntityType::Blockquote, 0, int(prefix.length()), {}});

	if (nameLength > 0) {
		newEntities.push_back(
			EntityInText{EntityType::Bold, 0, nameLength, QString()});

		if (const auto user = from->asUser()) {
			if (const auto accessHash = user->accessHash()) {
				const auto mentionData = u"%1.%2:%3"_q.arg(user->id.value)
											 .arg(accessHash)
											 .arg(session->userId().bare);

				newEntities.push_back(EntityInText{EntityType::MentionName,
					0,
					nameLength,
					mentionData});
			}
		}
	}

	const auto newTags = TextUtilities::ConvertEntitiesToTextTags(newEntities);
	textWithTags.tags.append(newTags);

	return true;
}

} // namespace

bool SaveDeletedMessages() {
	return Core::App().settings().readPref<bool>("xenon-save-deleted-messages",
		true);
}

bool SaveForBots() {
	return Core::App().settings().readPref<bool>("xenon-save-deleted-for-bots",
		false);
}

bool SemiTransparentDeletedMessages() {
	return Core::App().settings().readPref<bool>(
		"xenon-transparent-deleted-messages",
		false);
}

bool SaveEditedMessages() {
	return SaveDeletedMessages();
}

QString DeletedMark() {
	return QString::fromUtf8(Core::App().settings().readPref<QByteArray>(
		"xenon-deleted-mark")).trimmed();
}

rpl::producer<QString> DeletedMarkValue() {
	return DeletedMarkChanges().events_starting_with(DeletedMark());
}

void SetDeletedMark(QString value) {
	value = value.trimmed();
	Core::App().settings().writePref<QByteArray>(
		"xenon-deleted-mark",
		value.toUtf8());
	Core::App().saveSettingsDelayed();
	DeletedMarkChanges().fire(std::move(value));
}

TextWithEntities DeletedMarkText() {
	const auto mark = DeletedMark();
	return mark.isEmpty()
		? Ui::Text::IconEmoji(&st::messageDeletedIconEmoji)
		: tr::marked(mark);
}

void SaveMessageRevision(
		not_null<HistoryItem*> item,
		const HistoryMessageEdition &edition) {
	if (!SaveEditedMessages()
		|| !IsMessageSavable(item)
		|| edition.isEditHide) {
		return;
	}
	const auto &previous = item->originalText();
	const auto updated = edition.richPage
		? Iv::FlattenRichPageSummary(edition.richPage)
		: edition.textWithEntities;
	if (!previous.empty() && updated != previous) {
		AyuMessages::addEditedMessage(item);
	}
}

void ShowEditedMessages(
		not_null<Window::SessionController*> controller,
		not_null<PeerData*> peer,
		MsgId messageId) {
	controller->showSection(std::make_shared<MessageHistory::SectionMemento>(
		peer,
		messageId,
		ID(0)));
}

ID DialogId(not_null<PeerData*> peer) {
	const auto bare = ID(peer->id.value & PeerId::kChatTypeMask);
	return (peer->isChannel() || peer->isChat()) ? -bare : bare;
}

bool IsMessageSavable(not_null<HistoryItem*> item) {
	if (!SaveDeletedMessages() || !item->isRegular()) {
		return false;
	}
	const auto user = item->history()->peer->asUser();
	return !user || !user->isBot() || SaveForBots();
}

void ProcessMessageDelete(not_null<HistoryItem*> item) {
	if (!IsMessageSavable(item)) {
		item->destroy();
		return;
	}
	item->applyTTL(0);
	if (item->isDeleted()) {
		return;
	}
	AyuMessages::addDeletedMessage(item);
	item->setDeleted();
}

std::vector<not_null<HistoryItem*>> RestoreDeletedMessages(
		not_null<History*> history,
		ID minId,
		ID maxId) {
	auto result = std::vector<not_null<HistoryItem*>>();
	const auto user = history->peer->asUser();
	if (!SaveDeletedMessages() || (user && user->isBot() && !SaveForBots())) {
		return result;
	}
	const auto messages = getDeletedMessages(history->peer, 0, minId, maxId, -1);
	for (const auto &message : ranges::views::reverse(messages)) {
		if (message.messageId <= 0) {
			continue;
		}
		auto item = history->owner().message(
			history->peer,
			MsgId(message.messageId));
		if (item) {
			if (item->isDeleted() && !item->mainView()) {
				result.push_back(item);
			}
			continue;
		}
		auto flags = MessageFlag::HistoryEntry | MessageFlag::HasFromId;
		if (message.flags & 0x00000002) {
			flags |= MessageFlag::Outgoing;
		}
		if (message.flags & 0x00004000) {
			flags |= MessageFlag::Post;
		}
		if (message.topicId) {
			flags |= MessageFlag::HasReplyInfo;
		}
		if (message.flags & 0x00200000) {
			flags |= MessageFlag::HideEdited;
		}
		auto text = TextWithEntities{ QString::fromStdString(message.text) };
		text.entities = Api::EntitiesFromMTP(
			&history->session(),
			AyuMapper::deserializeTextWithEntities(message.textEntities).v);
		item = history->makeMessage({
			.id = MsgId(message.messageId),
			.flags = flags,
			.from = MessageSender(history->peer, message),
			.replyTo = FullReplyTo{
				.messageId = FullMsgId(history->peer->id, MsgId(message.topicId)),
				.topicRootId = MsgId(message.topicId),
			},
			.date = message.date,
			.editDate = (message.flags & 0x00008000) ? message.editDate : 0,
		}, text, MTP_messageMediaEmpty());
		item->setDeleted();
		item->markDeletedAnimated();
		result.push_back(item);
	}
	return result;
}

void PrepareDeletedReply(not_null<History*> history,
	TextWithTags &text,
	FullReplyTo &replyTo) {
	if (PrependPseudoReply(&history->session(), history, text, replyTo)) {
		const auto item = history->owner().message(replyTo.messageId);
		if (!replyTo.topicRootId && item) {
			replyTo.topicRootId = item->topicRootId();
		}
		replyTo.messageId
			= FullMsgId(replyTo.messageId.peer, replyTo.topicRootId);
		replyTo.quote = {};
	}
}

void ShowDeletedMessages(not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	ID topicId) {
	controller->showSection(
		std::make_shared<MessageHistory::SectionMemento>(peer,
			MsgId(),
			topicId));
}

void ClearDeletedMessages(std::shared_ptr<Ui::Show> show,
	not_null<PeerData*> peer,
	ID topicId) {
	show->show(Ui::MakeConfirmBox({
		.text = tr::lng_xenon_clear_deleted_confirm(tr::now),
		.confirmed =
			[=](Fn<void()> close) {
				auto items = std::vector<not_null<HistoryItem*>>();
				if (const auto history
					= peer->owner().historyLoaded(peer->id)) {
					for (const auto &block : history->blocks) {
						for (const auto &view : block->messages) {
							const auto item = view->data();
							if (item->isDeleted()
								&& (!topicId
									|| item->topicRootId().bare == topicId)) {
								items.push_back(item);
							}
						}
					}
				}
				AyuMessages::clearDeletedMessages(peer, topicId);
				peer->owner().notifyItemsAboutToBeDestroyed(items);
				for (const auto item : items) {
					item->destroy();
				}
				close();
			},
		.confirmText = tr::lng_xenon_clear_deleted(tr::now),
		.confirmStyle = &st::attentionBoxButton,
	}));
}

} // namespace AyuMessages
