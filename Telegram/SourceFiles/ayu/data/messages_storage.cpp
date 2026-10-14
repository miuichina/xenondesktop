// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/data/messages_storage.h"

#include "ayu/data/ayu_database.h"
#include "ayu/utils/ayu_mapper.h"
#include "ayu/deleted_messages.h"
#include "base/unixtime.h"
#include "data/data_forum_topic.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_session.h"

namespace AyuMessages {

PeerId MessageSender(
		not_null<PeerData*> peer,
		const AyuMessageBase &message) {
	if (message.fromId > PeerId::kChatTypeMask) {
		return PeerId(message.fromId);
	} else if ((message.flags & 0x00004000)
		&& message.fromId == (peer->id.value & PeerId::kChatTypeMask)) {
		return peer->id;
	}
	return message.fromId ? peerFromUser(UserId(message.fromId)) : peer->id;
}

template <typename DerivedMessage>
std::vector<AyuMessageBase> convertToBase(
	const std::vector<DerivedMessage> &messages) {
	std::vector<AyuMessageBase> based;
	based.reserve(messages.size());
	for (const auto &msg : messages) {
		based.push_back(static_cast<AyuMessageBase>(msg));
	}
	return based;
}

void map(not_null<HistoryItem*> item, AyuMessageBase &message) {
	const ID userId = item->history()->owner().session().userId().bare
		& PeerId::kChatTypeMask;

	message.userId = userId;
	message.dialogId = AyuMessages::DialogId(item->history()->peer);
	message.groupedId = item->groupId().raw();
	message.peerId = item->history()->peer->id.value & PeerId::kChatTypeMask;
	message.fromId = item->from()->id.value;
	if (item->topic()) {
		message.topicId = item->topicRootId().bare;
	} else {
		message.topicId = 0;
	}
	message.messageId = item->id.bare;
	message.date = item->date();
	message.flags = AyuMapper::mapItemFlagsToMTPFlags(item);

	if (const auto edited = item->Get<HistoryMessageEdited>()) {
		message.editDate = edited->date;
	} else {
		message.editDate = base::unixtime::now();
	}

	message.views = item->viewsCount();
	message.fwdFlags = 0;
	message.fwdFromId = 0;
	// message.fwdName
	message.fwdDate = 0;
	// message.fwdPostAuthor
	if (const auto msgsigned = item->Get<HistoryMessageSigned>()) {
		message.postAuthor = msgsigned->author.toStdString();
	}
	message.replyFlags = 0;
	message.replyMessageId = 0;
	message.replyPeerId = 0;
	message.replyTopId = 0;
	message.replyForumTopic = false;
	// message.replySerialized
	// message.replyMarkupSerialized
	message.entityCreateDate = base::unixtime::now();

	auto serializedText = AyuMapper::serializeTextWithEntities(item);
	message.text = serializedText.first;
	message.textEntities = serializedText.second;

	// todo: implement mapping
	message.mediaPath = "/";
	// message.hqThumbPath
	message.documentType = 0;  // document type none
							   // message.documentSerialized
							   // message.thumbsSerialized
							   // message.documentAttributesSerialized
							   // message.mimeType
}

void addEditedMessage(not_null<HistoryItem*> item) {
	auto message = EditedMessage();
	map(item, message);

	if (message.text.empty()) {
		return;
	}

	AyuDatabase::addEditedMessage(message);
}

std::vector<AyuMessageBase> getEditedMessages(not_null<HistoryItem*> item,
	ID minId,
	ID maxId,
	int totalLimit) {
	const ID userId = item->history()->owner().session().userId().bare
		& PeerId::kChatTypeMask;
	const auto dialogId = AyuMessages::DialogId(item->history()->peer);
	const auto msgId = item->id.bare;

	return convertToBase(AyuDatabase::getEditedMessages(userId,
		dialogId,
		msgId,
		minId,
		maxId,
		totalLimit));
}

bool hasRevisions(not_null<HistoryItem*> item) {
	const ID userId = item->history()->owner().session().userId().bare
		& PeerId::kChatTypeMask;
	const auto dialogId = AyuMessages::DialogId(item->history()->peer);
	const auto msgId = item->id.bare;

	return AyuDatabase::hasRevisions(userId, dialogId, msgId);
}

void addDeletedMessage(not_null<HistoryItem*> item) {
	auto message = DeletedMessage();
	map(item, message);

	if (message.text.empty()) {
		return;
	}

	AyuDatabase::addDeletedMessage(message);
}

std::vector<AyuMessageBase> getDeletedMessages(not_null<PeerData*> peer,
	ID topicId,
	ID minId,
	ID maxId,
	int totalLimit,
	const QString &searchQuery) {
	const ID userId = peer->session().userId().bare & PeerId::kChatTypeMask;
	return convertToBase(AyuDatabase::getDeletedMessages(userId,
		AyuMessages::DialogId(peer),
		topicId,
		minId,
		maxId,
		totalLimit,
		searchQuery.toStdString()));
}

bool hasDeletedMessages(not_null<PeerData*> peer, ID topicId) {
	const ID userId = peer->session().userId().bare & PeerId::kChatTypeMask;
	return AyuDatabase::hasDeletedMessages(userId,
		AyuMessages::DialogId(peer),
		topicId);
}

void removeDeletedMessage(not_null<HistoryItem*> item) {
	const auto peer = item->history()->peer;
	const ID userId = peer->session().userId().bare & PeerId::kChatTypeMask;
	AyuDatabase::removeDeletedMessage(userId,
		AyuMessages::DialogId(peer),
		item->id.bare);
}

void clearDeletedMessages(not_null<PeerData*> peer, ID topicId) {
	const ID userId = peer->session().userId().bare & PeerId::kChatTypeMask;
	AyuDatabase::clearDeletedMessages(userId,
		AyuMessages::DialogId(peer),
		topicId);
}

} // namespace AyuMessages
