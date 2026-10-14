// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include <string>
#include <vector>

using ID = long long;

class AyuMessageBase {
public:
	ID fakeId = 0;
	ID userId = 0;
	ID dialogId = 0;
	ID groupedId = 0;
	ID peerId = 0;
	ID fromId = 0;
	ID topicId = 0;
	int messageId = 0;
	int date = 0;
	int flags = 0;
	int editDate = 0;
	int views = 0;
	int fwdFlags = 0;
	ID fwdFromId = 0;
	std::string fwdName;
	int fwdDate = 0;
	std::string fwdPostAuthor;
	std::string postAuthor;
	int replyFlags = 0;
	int replyMessageId = 0;
	ID replyPeerId = 0;
	int replyTopId = 0;
	bool replyForumTopic = false;
	std::vector<char> replySerialized;
	std::vector<char> replyMarkupSerialized;
	int entityCreateDate = 0;
	std::string text;
	std::vector<char> textEntities;
	std::string mediaPath;
	std::string hqThumbPath;
	int documentType = 0;
	std::vector<char> documentSerialized;
	std::vector<char> thumbsSerialized;
	std::vector<char> documentAttributesSerialized;
	std::string mimeType;

};

class DeletedMessage : public AyuMessageBase {};

class EditedMessage : public AyuMessageBase {};
