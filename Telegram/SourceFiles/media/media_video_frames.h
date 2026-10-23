/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <atomic>
#include <memory>
#include <optional>

namespace Media::Video {

struct FileInfo {
	QSize dimensions;
	crl::time duration = 0;

	[[nodiscard]] bool valid() const {
		return !dimensions.isEmpty() && (duration > 0);
	}
};

[[nodiscard]] FileInfo ReadFileInfo(
	const QString &path,
	const QByteArray &content = QByteArray());

struct ExtractRequest {
	std::vector<crl::time> positions;
	QSize box;
	bool cover = false;
};

using ExtractCallback = Fn<bool(int index, QImage &&frame)>;

void ExtractFrames(
	const QString &path,
	const QByteArray &content,
	const ExtractRequest &request,
	ExtractCallback callback);

class FrameReader final {
public:
	FrameReader(QString path, QByteArray content);
	~FrameReader();

	[[nodiscard]] std::optional<crl::time> adjacentPosition(
		crl::time position,
		bool forward,
		QSize box = QSize(),
		const std::shared_ptr<std::atomic<bool>> &cancel = nullptr);
	[[nodiscard]] QImage cachedFrame(crl::time position, QSize box);
	[[nodiscard]] QImage frame(
		crl::time position,
		QSize box = QSize(),
		const std::shared_ptr<std::atomic<bool>> &cancel = nullptr,
		bool preloadPrevious = false);

private:
	struct Private;
	const std::unique_ptr<Private> _private;

};

[[nodiscard]] QImage ExtractFrame(
	const QString &path,
	const QByteArray &content,
	crl::time position,
	QSize box = QSize());

} // namespace Media::Video
