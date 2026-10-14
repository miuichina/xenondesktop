// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/data/ayu_database.h"

#include "ayu/data/entities.h"
#include "ayu/libs/sqlite/sqlite_orm.h"
#include "settings.h"

#include <QtCore/QDir>
#include <mutex>

using namespace sqlite_orm;

namespace {

std::mutex databaseMutex;

auto &MessageStorage() {
	static auto storage = make_storage(
		(cWorkingDir() + u"tdata/xenon-messages.db"_q).toStdString(),
		make_unique_index("idx_deleted_message",
			column<DeletedMessage>(&DeletedMessage::userId),
			column<DeletedMessage>(&DeletedMessage::dialogId),
			column<DeletedMessage>(&DeletedMessage::messageId)),
		make_index("idx_edited_message_user_dialog_message",
			column<EditedMessage>(&EditedMessage::userId),
			column<EditedMessage>(&EditedMessage::dialogId),
			column<EditedMessage>(&EditedMessage::messageId)),
		make_table<DeletedMessage>("DeletedMessage",
			make_column("fakeId",
				&DeletedMessage::fakeId,
				primary_key().autoincrement()),
			make_column("userId", &DeletedMessage::userId),
			make_column("dialogId", &DeletedMessage::dialogId),
			make_column("groupedId", &DeletedMessage::groupedId),
			make_column("peerId", &DeletedMessage::peerId),
			make_column("fromId", &DeletedMessage::fromId),
			make_column("topicId", &DeletedMessage::topicId),
			make_column("messageId", &DeletedMessage::messageId),
			make_column("date", &DeletedMessage::date),
			make_column("flags", &DeletedMessage::flags),
			make_column("editDate", &DeletedMessage::editDate),
			make_column("views", &DeletedMessage::views),
			make_column("fwdFlags", &DeletedMessage::fwdFlags),
			make_column("fwdFromId", &DeletedMessage::fwdFromId),
			make_column("fwdName", &DeletedMessage::fwdName),
			make_column("fwdDate", &DeletedMessage::fwdDate),
			make_column("fwdPostAuthor", &DeletedMessage::fwdPostAuthor),
			make_column("replyFlags", &DeletedMessage::replyFlags),
			make_column("replyMessageId", &DeletedMessage::replyMessageId),
			make_column("replyPeerId", &DeletedMessage::replyPeerId),
			make_column("replyTopId", &DeletedMessage::replyTopId),
			make_column("replyForumTopic", &DeletedMessage::replyForumTopic),
			make_column("replySerialized", &DeletedMessage::replySerialized),
			make_column("entityCreateDate", &DeletedMessage::entityCreateDate),
			make_column("text", &DeletedMessage::text),
			make_column("textEntities", &DeletedMessage::textEntities),
			make_column("mediaPath", &DeletedMessage::mediaPath),
			make_column("hqThumbPath", &DeletedMessage::hqThumbPath),
			make_column("documentType", &DeletedMessage::documentType),
			make_column("documentSerialized",
				&DeletedMessage::documentSerialized),
			make_column("thumbsSerialized", &DeletedMessage::thumbsSerialized),
			make_column("documentAttributesSerialized",
				&DeletedMessage::documentAttributesSerialized),
			make_column("mimeType", &DeletedMessage::mimeType)),
		make_table<EditedMessage>("EditedMessage",
			make_column("fakeId",
				&EditedMessage::fakeId,
				primary_key().autoincrement()),
			make_column("userId", &EditedMessage::userId),
			make_column("dialogId", &EditedMessage::dialogId),
			make_column("groupedId", &EditedMessage::groupedId),
			make_column("peerId", &EditedMessage::peerId),
			make_column("fromId", &EditedMessage::fromId),
			make_column("topicId", &EditedMessage::topicId),
			make_column("messageId", &EditedMessage::messageId),
			make_column("date", &EditedMessage::date),
			make_column("flags", &EditedMessage::flags),
			make_column("editDate", &EditedMessage::editDate),
			make_column("views", &EditedMessage::views),
			make_column("fwdFlags", &EditedMessage::fwdFlags),
			make_column("fwdFromId", &EditedMessage::fwdFromId),
			make_column("fwdName", &EditedMessage::fwdName),
			make_column("fwdDate", &EditedMessage::fwdDate),
			make_column("fwdPostAuthor", &EditedMessage::fwdPostAuthor),
			make_column("replyFlags", &EditedMessage::replyFlags),
			make_column("replyMessageId", &EditedMessage::replyMessageId),
			make_column("replyPeerId", &EditedMessage::replyPeerId),
			make_column("replyTopId", &EditedMessage::replyTopId),
			make_column("replyForumTopic", &EditedMessage::replyForumTopic),
			make_column("replySerialized", &EditedMessage::replySerialized),
			make_column("entityCreateDate", &EditedMessage::entityCreateDate),
			make_column("text", &EditedMessage::text),
			make_column("textEntities", &EditedMessage::textEntities),
			make_column("mediaPath", &EditedMessage::mediaPath),
			make_column("hqThumbPath", &EditedMessage::hqThumbPath),
			make_column("documentType", &EditedMessage::documentType),
			make_column("documentSerialized",
				&EditedMessage::documentSerialized),
			make_column("thumbsSerialized", &EditedMessage::thumbsSerialized),
			make_column("documentAttributesSerialized",
				&EditedMessage::documentAttributesSerialized),
			make_column("mimeType", &EditedMessage::mimeType)));
	return storage;
}

bool Initialize() {
	static auto ready = false;
	if (ready) {
		return true;
	}
	try {
		QDir().mkpath(cWorkingDir() + u"tdata"_q);
		MessageStorage().sync_schema(true);
		ready = true;
	} catch (const std::exception &error) {
		LOG(("Deleted messages: database initialization failed: %1")
				.arg(error.what()));
	}
	return ready;
}

} // namespace

namespace AyuDatabase {

void addEditedMessage(const EditedMessage &message) {
	std::lock_guard lock(databaseMutex);
	if (!Initialize()) {
		return;
	}
	try {
		MessageStorage().begin_transaction();
		MessageStorage().insert(message);
		MessageStorage().commit();
	} catch (std::exception &ex) {
		try {
			MessageStorage().rollback();
		} catch (...) {
		}
		LOG(("Failed to save edited message for some reason: %1")
				.arg(ex.what()));
	}
}

std::vector<EditedMessage> getEditedMessages(ID userId,
	ID dialogId,
	ID messageId,
	ID minId,
	ID maxId,
	int totalLimit) {
	std::lock_guard lock(databaseMutex);
	if (!Initialize()) {
		return {};
	}
	try {
		return MessageStorage().get_all<EditedMessage>(
			where(column<EditedMessage>(&EditedMessage::userId) == userId
				and column<EditedMessage>(&EditedMessage::dialogId) == dialogId
				and column<EditedMessage>(&EditedMessage::messageId)
					== messageId
				and (column<EditedMessage>(&EditedMessage::fakeId) > minId
					or minId == 0)
				and (column<EditedMessage>(&EditedMessage::fakeId) < maxId
					or maxId == 0)),
			order_by(column<EditedMessage>(&EditedMessage::fakeId)).desc(),
			limit(totalLimit));
	} catch (std::exception &ex) {
		LOG(("Failed to get edited messages: %1").arg(ex.what()));
		return {};
	}
}

bool hasRevisions(ID userId, ID dialogId, ID messageId) {
	std::lock_guard lock(databaseMutex);
	if (!Initialize()) {
		return false;
	}
	try {
		return !MessageStorage()
					.select(columns(column<EditedMessage>(
								&EditedMessage::messageId)),
						where(column<EditedMessage>(&EditedMessage::userId)
								== userId
							and column<EditedMessage>(&EditedMessage::dialogId)
								== dialogId
							and column<EditedMessage>(&EditedMessage::messageId)
								== messageId),
						limit(1))
					.empty();
	} catch (std::exception &ex) {
		LOG(("Failed to check if message has revisions: %1").arg(ex.what()));
		return false;
	}
}

void addDeletedMessage(const DeletedMessage &message) {
	std::lock_guard lock(databaseMutex);
	if (!Initialize()) {
		return;
	}
	try {
		MessageStorage().begin_transaction();
		MessageStorage().remove_all<DeletedMessage>(where(
			column<DeletedMessage>(&DeletedMessage::userId) == message.userId
			and column<DeletedMessage>(&DeletedMessage::dialogId)
				== message.dialogId
			and column<DeletedMessage>(&DeletedMessage::messageId)
				== message.messageId));
		MessageStorage().insert(message);
		MessageStorage().commit();
	} catch (std::exception &ex) {
		try {
			MessageStorage().rollback();
		} catch (...) {
		}
		LOG(("Failed to save edited message for some reason: %1")
				.arg(ex.what()));
	}
}

std::vector<DeletedMessage> getDeletedMessages(ID userId,
	ID dialogId,
	ID topicId,
	ID minId,
	ID maxId,
	int totalLimit,
	const std::string &searchQuery) {
	std::lock_guard lock(databaseMutex);
	if (!Initialize()) {
		return {};
	}
	try {
		if (searchQuery.empty()) {
			return MessageStorage().get_all<DeletedMessage>(
				where(column<DeletedMessage>(&DeletedMessage::userId) == userId
					and column<DeletedMessage>(&DeletedMessage::dialogId)
						== dialogId
					and (column<DeletedMessage>(&DeletedMessage::topicId)
							== topicId
						or topicId == 0)
					and (column<DeletedMessage>(&DeletedMessage::messageId)
							> minId
						or minId == 0)
					and (column<DeletedMessage>(&DeletedMessage::messageId)
							< maxId
						or maxId == 0)),
				order_by(column<DeletedMessage>(&DeletedMessage::messageId))
					.desc(),
				limit(totalLimit));
		}

		std::string escaped;
		escaped.reserve(searchQuery.size());
		for (const auto c : searchQuery) {
			if (c == '%' || c == '_' || c == '\\') {
				escaped += '\\';
			}
			escaped += c;
		}
		const auto pattern = "%" + escaped + "%";
		return MessageStorage().get_all<DeletedMessage>(
			where(column<DeletedMessage>(&DeletedMessage::userId) == userId
				and column<DeletedMessage>(&DeletedMessage::dialogId)
					== dialogId
				and (column<DeletedMessage>(&DeletedMessage::topicId) == topicId
					or topicId == 0)
				and (column<DeletedMessage>(&DeletedMessage::messageId) > minId
					or minId == 0)
				and (column<DeletedMessage>(&DeletedMessage::messageId) < maxId
					or maxId == 0)
				and like(column<DeletedMessage>(&DeletedMessage::text),
					pattern,
					"\\")),
			order_by(column<DeletedMessage>(&DeletedMessage::messageId)).desc(),
			limit(totalLimit));
	} catch (std::exception &ex) {
		LOG(("Failed to get deleted messages: %1").arg(ex.what()));
		return {};
	}
}

bool hasDeletedMessages(ID userId, ID dialogId, ID topicId) {
	std::lock_guard lock(databaseMutex);
	if (!Initialize()) {
		return false;
	}
	try {
		return !MessageStorage()
					.select(columns(column<DeletedMessage>(
								&DeletedMessage::dialogId)),
						where(column<DeletedMessage>(&DeletedMessage::userId)
								== userId
							and column<DeletedMessage>(
									&DeletedMessage::dialogId)
								== dialogId
							and (column<DeletedMessage>(
									 &DeletedMessage::topicId)
									== topicId
								or topicId == 0)),
						limit(1))
					.empty();
	} catch (std::exception &ex) {
		LOG(("Failed to check if dialog has deleted message: %1")
				.arg(ex.what()));
		return false;
	}
}

void removeDeletedMessage(ID userId, ID dialogId, ID messageId) {
	std::lock_guard lock(databaseMutex);
	if (!Initialize()) {
		return;
	}
	try {
		MessageStorage().remove_all<DeletedMessage>(where(
			column<DeletedMessage>(&DeletedMessage::userId) == userId
			and column<DeletedMessage>(&DeletedMessage::dialogId) == dialogId
			and column<DeletedMessage>(&DeletedMessage::messageId)
				== messageId));
	} catch (std::exception &ex) {
		LOG(("Failed to remove deleted message: %1").arg(ex.what()));
	}
}

void clearDeletedMessages(ID userId, ID dialogId, ID topicId) {
	std::lock_guard lock(databaseMutex);
	if (!Initialize()) {
		return;
	}
	try {
		MessageStorage().remove_all<DeletedMessage>(where(
			column<DeletedMessage>(&DeletedMessage::userId) == userId
			and column<DeletedMessage>(&DeletedMessage::dialogId) == dialogId
			and (column<DeletedMessage>(&DeletedMessage::topicId) == topicId
				or topicId == 0)));
	} catch (std::exception &) {
	}
}

} // namespace AyuDatabase
