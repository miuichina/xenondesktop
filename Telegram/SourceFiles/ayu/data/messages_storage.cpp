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
#include "api/api_text_entities.h"
#include "base/unixtime.h"
#include "core/version.h"
#include "data/data_document_media.h"
#include "data/data_forum_topic.h"
#include "data/data_media_types.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_session.h"
#include "storage/cache/storage_cache_database.h"
#include "storage/file_download.h"
#include "storage/serialize_document.h"
#include "ui/image/image.h"

#include <QtCore/QBuffer>

namespace AyuMessages {
namespace {

constexpr auto kSavedPhoto = 1;
constexpr auto kSavedDocument = 2;
constexpr auto kMediaVersion = quint32(1);
using PhotoSize = Data::PhotoSize;

constexpr auto kPhotoSizes = std::array{
	PhotoSize::Small,
	PhotoSize::Thumbnail,
	PhotoSize::Large,
};

std::vector<char> ToBlob(const QByteArray &bytes) {
	return { bytes.begin(), bytes.end() };
}

QByteArray FromBlob(const std::vector<char> &bytes) {
	return QByteArray(bytes.data(), bytes.size());
}

QByteArray PhotoBytes(Data::PhotoMedia *view, PhotoSize size) {
	if (!view) {
		return {};
	}
	auto bytes = view->imageBytes(size);
	if (bytes.isEmpty()) {
		if (const auto image = view->image(size)) {
			auto buffer = QBuffer(&bytes);
			buffer.open(QIODevice::WriteOnly);
			image->original().save(&buffer, "PNG");
		}
	}
	return bytes;
}

Storage::Cache::Key SaveMedia(
		not_null<HistoryItem*> item,
		DeletedMessage &message) {
	const auto media = item->media();
	if (!media) {
		return {};
	}
	auto metadata = QByteArray();
	auto stream = QDataStream(&metadata, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << kMediaVersion << media->hasSpoiler();

	auto bytes = QByteArray();
	auto key = Storage::Cache::Key();
	if (const auto photo = media->photo(); photo && !photo->isNull()) {
		message.documentType = kSavedPhoto;
		const auto input = photo->mtpInput().c_inputPhoto().vaccess_hash().v;
		const auto &large = photo->location(PhotoSize::Large);
		const auto storage = std::get_if<StorageFileLocation>(&large.file().data);
		stream << quint64(photo->id)
			<< quint64(input)
			<< qint32(photo->date())
			<< qint32(storage ? storage->dcId() : 0)
			<< photo->fileReference()
			<< photo->inlineThumbnailBytes();
		const auto view = photo->activeMediaView();
		bytes = PhotoBytes(view.get(), PhotoSize::Large);
		key = large.file().cacheKey();
		for (const auto size : kPhotoSizes) {
			auto location = photo->hasExact(size)
				? photo->location(size)
				: ImageLocation();
			if (location.valid() && size != PhotoSize::Large) {
				auto preview = PhotoBytes(view.get(), size);
				if (!preview.isEmpty()) {
					location = ImageLocation(
						DownloadLocation{ InMemoryLocation{ std::move(preview) } },
						location.width(),
						location.height());
				}
			}
			stream << location.serialize() << qint32(photo->imageByteSize(size));
		}
	} else if (const auto document = media->document()) {
		message.documentType = kSavedDocument;
		const auto sticker = document->sticker();
		const auto voice = document->voice();
		const auto song = document->song();
		stream << quint64(document->id)
			<< quint64(sticker ? sticker->set.id : 0)
			<< quint64(sticker ? sticker->set.accessHash : 0)
			<< (sticker ? sticker->set.shortName : QString())
			<< bool(voice)
			<< bool(song)
			<< qint32(document->duration() / 1000)
			<< (voice && !voice->waveform.isEmpty() && voice->waveform[0] >= 0
				? documentWaveformEncode5bit(voice->waveform)
				: QByteArray())
			<< (song ? song->title : QString())
			<< (song ? song->performer : QString());
		Serialize::Document::writeToStream(stream, document);
		if (const auto view = document->activeMediaView()) {
			bytes = view->bytes();
		}
		key = document->cacheKey();
	} else {
		return {};
	}
	message.documentSerialized = ToBlob(metadata);
	if (!bytes.isEmpty() && bytes.size() <= Storage::kMaxFileInMemory) {
		message.thumbsSerialized = ToBlob(bytes);
		return {};
	}
	return key;
}

void RestoreMediaBytes(
		not_null<History*> history,
		Storage::Cache::Key key,
		const AyuMessageBase &message,
		uint8 tag) {
	if (key && !message.thumbsSerialized.empty()) {
		history->owner().cache().putIfEmpty(
			key,
			Storage::Cache::Database::TaggedValue(
				FromBlob(message.thumbsSerialized),
				tag));
	}
}

PhotoData *RestorePhoto(
		not_null<History*> history,
		QDataStream &stream,
		const AyuMessageBase &message) {
	auto id = quint64();
	auto access = quint64();
	auto date = qint32();
	auto dc = qint32();
	auto reference = QByteArray();
	auto preview = QByteArray();
	stream >> id >> access >> date >> dc >> reference >> preview;
	auto sizes = std::array<ImageWithLocation, kPhotoSizes.size()>();
	for (const auto size : kPhotoSizes) {
		auto serialized = QByteArray();
		auto bytesCount = qint32();
		stream >> serialized >> bytesCount;
		auto &entry = sizes[Data::PhotoSizeIndex(size)];
		const auto location = ImageLocation::FromSerialized(serialized);
		if (!location) {
			return nullptr;
		}
		entry.location = *location;
		entry.bytesCount = bytesCount;
	}
	auto &large = sizes[Data::PhotoSizeIndex(PhotoSize::Large)];
	if (stream.status() != QDataStream::Ok || !id || !large.location.valid()) {
		return nullptr;
	}
	RestoreMediaBytes(history, large.location.file().cacheKey(), message,
		Data::kImageCacheTag);
	const auto existing = history->owner().photo(id);
	if (!existing->isNull()) {
		existing->clearFailed(PhotoSize::Large);
		return existing;
	}
	return history->owner().photo(
		id,
		access,
		reference,
		date ? date : message.date,
		dc,
		false,
		preview,
		sizes[Data::PhotoSizeIndex(PhotoSize::Small)],
		sizes[Data::PhotoSizeIndex(PhotoSize::Thumbnail)],
		large,
		{},
		{},
		0);
}

DocumentData *RestoreDocument(not_null<History*> history, QDataStream &stream) {
	auto id = quint64();
	auto setId = quint64();
	auto setAccess = quint64();
	auto setName = QString();
	auto voice = false;
	auto song = false;
	auto duration = qint32();
	auto waveform = QByteArray();
	auto title = QString();
	auto performer = QString();
	stream >> id >> setId >> setAccess >> setName
		>> voice >> song >> duration >> waveform >> title >> performer;
	if (stream.status() != QDataStream::Ok || !id) {
		return nullptr;
	}
	const auto existing = history->owner().document(id);
	if (existing->date) {
		return existing;
	}
	const auto document = Serialize::Document::readStickerFromStream(
		&history->session(),
		AppVersion,
		stream,
		Serialize::Document::StickerSetInfo(setId, setAccess, setName));
	if (document && (voice || song)) {
		using Flag = MTPDdocumentAttributeAudio::Flag;
		const auto flags = voice
			? Flag::f_voice | Flag::f_waveform
			: Flag::f_title | Flag::f_performer;
		document->setattributes({ MTP_documentAttributeAudio(
			MTP_flags(flags),
			MTP_int(duration),
			MTP_string(title),
			MTP_string(performer),
			MTP_bytes(waveform)) });
	}
	return document;
}

} // namespace

not_null<HistoryItem*> MakeSavedMessage(
		not_null<History*> history,
		HistoryItemCommonFields fields,
		const AyuMessageBase &message) {
	auto text = TextWithEntities{ QString::fromStdString(message.text) };
	text.entities = Api::EntitiesFromMTP(
		&history->session(),
		AyuMapper::deserializeTextWithEntities(message.textEntities).v);

	if (!message.documentSerialized.empty()) {
		auto stream = QDataStream(FromBlob(message.documentSerialized));
		stream.setVersion(QDataStream::Qt_5_1);
		auto version = quint32();
		stream >> version >> fields.mediaSpoiler;
		if (version == kMediaVersion) {
			if (message.documentType == kSavedPhoto) {
				if (const auto photo = RestorePhoto(history, stream, message)) {
					return history->makeMessage(std::move(fields), photo, text);
				}
			} else if (message.documentType == kSavedDocument) {
				if (const auto document = RestoreDocument(history, stream)) {
					if (!message.thumbsSerialized.empty()) {
						document->forceToCache(true);
					}
					RestoreMediaBytes(history, document->cacheKey(), message,
						document->cacheTag());
					return history->makeMessage(std::move(fields), document, text);
				}
			}
		}
	}
	fields.groupedId = 0;
	return history->makeMessage(std::move(fields), text, MTP_messageMediaEmpty());
}

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
	std::vector<DerivedMessage> messages) {
	std::vector<AyuMessageBase> based;
	based.reserve(messages.size());
	for (auto &msg : messages) {
		based.push_back(std::move(msg));
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
	if (item->history()->peer->isForum()) {
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

	message.mediaPath = "/";
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
	const auto cacheKey = SaveMedia(item, message);
	if (message.text.empty() && message.documentSerialized.empty()) {
		return;
	}

	const auto rowId = AyuDatabase::addDeletedMessage(message);
	if (rowId && cacheKey) {
		item->history()->owner().cache().get(cacheKey, [=](QByteArray &&bytes) {
			if (!bytes.isEmpty() && bytes.size() <= Storage::kMaxFileInMemory) {
				AyuDatabase::setDeletedMediaBytes(rowId, ToBlob(bytes));
			}
		});
	}
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
