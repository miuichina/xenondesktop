// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "ayu/data/entities.h"

#include <functional>

namespace AyuDatabase {
void addEditedMessage(const EditedMessage &message);
std::vector<EditedMessage> getEditedMessages(ID userId,
	ID dialogId,
	ID messageId,
	ID minId,
	ID maxId,
	int totalLimit);
bool hasRevisions(ID userId, ID dialogId, ID messageId);
void addDeletedMessage(const DeletedMessage &message);
std::vector<DeletedMessage> getDeletedMessages(ID userId,
	ID dialogId,
	ID topicId,
	ID minId,
	ID maxId,
	int totalLimit,
	const std::string &searchQuery = "");
bool hasDeletedMessages(ID userId, ID dialogId, ID topicId);
void removeDeletedMessage(ID userId, ID dialogId, ID messageId);
void clearDeletedMessages(ID userId, ID dialogId, ID topicId);
} // namespace AyuDatabase
