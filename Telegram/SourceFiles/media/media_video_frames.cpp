/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/media_video_frames.h"

#include "ffmpeg/ffmpeg_bytes_io_wrap.h"
#include "ffmpeg/ffmpeg_utility.h"

#include <mutex>

namespace Media::Video {
namespace {

using namespace FFmpeg;

constexpr auto kMaxDecodeSpan = crl::time(10000);
constexpr auto kForwardDecodeWindow = crl::time(3000);
constexpr auto kMaxDecodeArea = 4096 * int64(4096);
constexpr auto kMaxPositionCache = 2048;
constexpr auto kMaxPreviewCacheBytes = int64(64) * 1024 * 1024;
constexpr auto kMaxPreviewCacheFrames = 256;
constexpr auto kReversePreviewSpan = crl::time(3000);

struct Source {
	ReadFileWrap fileWrap;
	ReadBytesWrap bytesWrap;
	QByteArray content;
	FormatPointer format;
	AVStream *videoStream = nullptr;
	CodecPointer codec;
	int videoId = -1;
	int rotation = 0;
};

[[nodiscard]] bool OpenSource(
		Source &source,
		const QString &path,
		const QByteArray &content,
		bool withDecoder) {
	if (!content.isEmpty()) {
		source.content = content;
		const auto data = source.content.constData();
		source.bytesWrap = ReadBytesWrap{
			.size = int64(source.content.size()),
			.data = reinterpret_cast<const uchar*>(data),
		};
		source.format = MakeFormatPointer(
			&source.bytesWrap,
			&ReadBytesWrap::Read,
			nullptr,
			&ReadBytesWrap::Seek);
	} else {
		source.fileWrap.file.setFileName(path);
		if (!source.fileWrap.file.open(QIODevice::ReadOnly)) {
			return false;
		}
		source.format = MakeFormatPointer(
			&source.fileWrap,
			&ReadFileWrap::Read,
			nullptr,
			&ReadFileWrap::Seek);
	}
	if (!source.format) {
		return false;
	}
	const auto error = AvErrorWrap(
		avformat_find_stream_info(source.format.get(), nullptr));
	if (error) {
		LogError(u"avformat_find_stream_info"_q, error);
		return false;
	}
	source.videoId = av_find_best_stream(
		source.format.get(),
		AVMEDIA_TYPE_VIDEO,
		-1,
		-1,
		nullptr,
		0);
	if (source.videoId < 0) {
		return false;
	}
	source.videoStream = source.format->streams[source.videoId];
	if (source.videoStream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
		return false;
	}
	source.rotation = ReadRotationFromMetadata(source.videoStream);
	if (withDecoder) {
		source.codec = MakeCodecPointer({
			.stream = source.videoStream,
			.videoMaxArea = kMaxDecodeArea,
		});
		if (!source.codec) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] crl::time SourceDuration(const Source &source) {
	const auto stream = source.videoStream;
	if (stream->duration > 0) {
		return PtsToTimeCeil(stream->duration, stream->time_base);
	} else if (source.format->duration > 0) {
		return PtsToTimeCeil(source.format->duration, kUniversalTimeBase);
	}
	return 0;
}

[[nodiscard]] int64 StreamStart(not_null<const AVStream*> stream) {
	return (stream->start_time != AV_NOPTS_VALUE) ? stream->start_time : 0;
}

[[nodiscard]] crl::time FramePosition(
		not_null<AVFrame*> frame,
		not_null<const AVStream*> stream) {
	const auto pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
		? frame->best_effort_timestamp
		: frame->pts;
	return (pts != AV_NOPTS_VALUE)
		? PtsToTime(pts - StreamStart(stream), stream->time_base)
		: -1;
}

[[nodiscard]] QImage ConvertFrame(
		not_null<AVFrame*> frame,
		int rotation,
		QSize box,
		bool cover,
		SwscalePointer &scale) {
	const auto srcSize = QSize(frame->width, frame->height);
	if (srcSize.isEmpty()) {
		return {};
	}
	const auto displaySize = TransposeSizeByRotation(srcSize, rotation);
	auto scaledDisplay = displaySize;
	if (!box.isEmpty()) {
		scaledDisplay = displaySize.scaled(
			box,
			cover
				? Qt::KeepAspectRatioByExpanding
				: Qt::KeepAspectRatio);
	}
	auto dstSize = TransposeSizeByRotation(scaledDisplay, rotation);
	dstSize = QSize(
		std::max(dstSize.width(), 1),
		std::max(dstSize.height(), 1));

	auto storage = CreateFrameStorage(dstSize);
	if (storage.isNull()) {
		return {};
	}
	const auto srcFormat = (frame->format == AV_PIX_FMT_NONE)
		? AV_PIX_FMT_YUV420P
		: AVPixelFormat(frame->format);
	scale = MakeSwscalePointer(
		srcSize,
		srcFormat,
		dstSize,
		AV_PIX_FMT_BGRA,
		&scale);
	if (!scale) {
		return {};
	}
	uint8_t *dstData[AV_NUM_DATA_POINTERS] = { storage.bits(), nullptr };
	int dstLinesize[AV_NUM_DATA_POINTERS] = {
		int(storage.bytesPerLine()),
		0,
	};
	sws_scale(
		scale.get(),
		frame->data,
		frame->linesize,
		0,
		frame->height,
		dstData,
		dstLinesize);

	if (srcFormat == AV_PIX_FMT_BGRA || srcFormat == AV_PIX_FMT_YUVA420P) {
		PremultiplyInplace(storage);
	}

	auto result = storage;
	if (rotation) {
		auto transform = QTransform();
		transform.rotate(rotation);
		result = result.transformed(transform);
	}
	if (cover && !box.isEmpty() && result.size() != box) {
		const auto x = (result.width() - box.width()) / 2;
		const auto y = (result.height() - box.height()) / 2;
		result = result.copy(QRect(
			std::max(x, 0),
			std::max(y, 0),
			std::min(box.width(), result.width()),
			std::min(box.height(), result.height())));
	}
	return result;
}

class Extractor final {
public:
	Extractor(Source &source, const ExtractRequest &request);
	~Extractor();

	[[nodiscard]] bool valid() const;
	[[nodiscard]] QImage take(
		crl::time position,
		const std::shared_ptr<std::atomic<bool>> &cancel = nullptr,
		Fn<void(crl::time, QImage)> cache = nullptr,
		bool exact = false);
	[[nodiscard]] std::optional<crl::time> adjacentPosition(
		crl::time position,
		bool forward,
		const std::shared_ptr<std::atomic<bool>> &cancel = nullptr,
		Fn<void(crl::time, QImage)> cache = nullptr);

private:
	[[nodiscard]] bool receiveInto(
		not_null<AVFrame*> frame,
		const std::shared_ptr<std::atomic<bool>> &cancel = nullptr);
	void seekTo(crl::time position);
	void keepDecodedFrame(crl::time position);
	void cacheDecodedFrame(
		crl::time position,
		crl::time target,
		const Fn<void(crl::time, QImage)> &cache);

	Source &_source;
	const ExtractRequest &_request;
	FramePointer _frame;
	FramePointer _kept;
	SwscalePointer _scale;
	AVPacket *_packet = nullptr;
	std::vector<crl::time> _positions;
	crl::time _decodedPosition = -1;
	bool _finished = false;

};

Extractor::Extractor(Source &source, const ExtractRequest &request)
: _source(source)
, _request(request)
, _frame(MakeFramePointer())
, _kept(MakeFramePointer())
, _packet(av_packet_alloc()) {
}

Extractor::~Extractor() {
	if (_packet) {
		av_packet_free(&_packet);
	}
}

bool Extractor::valid() const {
	return _frame && _kept && _packet && _source.codec;
}

void Extractor::seekTo(crl::time position) {
	const auto stream = _source.videoStream;
	const auto target = TimeToPts(
		std::max(position, crl::time(0)),
		stream->time_base) + StreamStart(stream);
	const auto seeked = av_seek_frame(
		_source.format.get(),
		_source.videoId,
		target,
		AVSEEK_FLAG_BACKWARD);
	if (seeked < 0) {
		av_seek_frame(
			_source.format.get(),
			_source.videoId,
			StreamStart(stream),
			AVSEEK_FLAG_BACKWARD);
	}
	avcodec_flush_buffers(_source.codec.get());
	_decodedPosition = -1;
	_positions.clear();
	av_frame_unref(_kept.get());
	_finished = false;
}

bool Extractor::receiveInto(
		not_null<AVFrame*> frame,
		const std::shared_ptr<std::atomic<bool>> &cancel) {
	while (true) {
		if (cancel && cancel->load()) {
			return false;
		}
		auto got = AvErrorWrap(avcodec_receive_frame(
			_source.codec.get(),
			frame));
		if (!got) {
			return true;
		} else if (got.code() != AVERROR(EAGAIN)) {
			if (got.code() != AVERROR_EOF) {
				LogError(u"avcodec_receive_frame"_q, got);
			}
			return false;
		} else if (_finished) {
			return false;
		}
		auto read = AvErrorWrap(av_read_frame(
			_source.format.get(),
			_packet));
		if (read.code() == AVERROR_EOF) {
			_finished = true;
			avcodec_send_packet(_source.codec.get(), nullptr);
			continue;
		} else if (read) {
			LogError(u"av_read_frame"_q, read);
			return false;
		}
		const auto unref = gsl::finally([&] {
			av_packet_unref(_packet);
		});
		if (_packet->stream_index != _source.videoId) {
			continue;
		}
		auto sent = AvErrorWrap(avcodec_send_packet(
			_source.codec.get(),
			_packet));
		if (sent && sent.code() != AVERROR(EAGAIN)) {
			LogError(u"avcodec_send_packet"_q, sent);
			return false;
		}
	}
}

void Extractor::keepDecodedFrame(crl::time position) {
	av_frame_unref(_kept.get());
	av_frame_ref(_kept.get(), _frame.get());
	_decodedPosition = position;
	if (position < 0) {
		return;
	} else if (!_positions.empty() && position < _positions.back()) {
		_positions.clear();
	}
	if (_positions.empty() || position > _positions.back()) {
		_positions.push_back(position);
		if (_positions.size() > kMaxPositionCache) {
			_positions.erase(begin(_positions));
		}
	}
}

void Extractor::cacheDecodedFrame(
		crl::time position,
		crl::time target,
		const Fn<void(crl::time, QImage)> &cache) {
	if (!cache || _request.box.isEmpty() || position >= target) {
		return;
	}
	const auto stream = _source.videoStream;
	const auto rate = (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0)
		? stream->avg_frame_rate
		: stream->r_frame_rate;
	const auto bytes = int64(_request.box.width()) * _request.box.height() * 4;
	const auto frames = std::max(kMaxPreviewCacheBytes / bytes - 1, int64(0));
	const auto span = (rate.num > 0 && rate.den > 0)
		? crl::time(std::min(
			float64(kReversePreviewSpan),
			1000. * frames * rate.den / rate.num))
		: crl::time(0);
	if (position >= std::max(target - span, crl::time(0))) {
		cache(position, ConvertFrame(
			_frame.get(),
			_source.rotation,
			_request.box,
			_request.cover,
			_scale));
	}
}

std::optional<crl::time> Extractor::adjacentPosition(
		crl::time position,
		bool forward,
		const std::shared_ptr<std::atomic<bool>> &cancel,
		Fn<void(crl::time, QImage)> cache) {
	if (cancel && cancel->load()) {
		return std::nullopt;
	}
	position = std::max(position, crl::time(0));
	const auto current = ranges::lower_bound(_positions, position);
	if (current != end(_positions)) {
		if (forward && std::next(current) != end(_positions)) {
			return *std::next(current);
		} else if (!forward && current != begin(_positions)) {
			return *std::prev(current);
		}
	}
	const auto selectedPosition = (current != end(_positions))
		? *current
		: position;
	const auto continuing = forward && (_decodedPosition == selectedPosition);
	if (!continuing) {
		seekTo(std::max(position - 1, crl::time(0)));
	}
	const auto stream = _source.videoStream;
	auto previous = crl::time(-1);
	auto beforePrevious = crl::time(-1);
	auto selected = continuing ? selectedPosition : crl::time(-1);
	while (receiveInto(_frame.get(), cancel)) {
		const auto at = FramePosition(_frame.get(), stream);
		keepDecodedFrame(at);
		cacheDecodedFrame(at, position, cache);
		av_frame_unref(_frame.get());
		if (at < 0) {
			continue;
		} else if (selected >= 0) {
			if (at > selected) {
				return at;
			}
		} else if (at >= position) {
			if (!forward) {
				return (previous >= 0) ? previous : at;
			}
			selected = at;
		}
		beforePrevious = previous;
		previous = at;
	}
	if (cancel && cancel->load()) {
		return std::nullopt;
	} else if (selected >= 0) {
		return selected;
	} else if (!forward && beforePrevious >= 0) {
		return beforePrevious;
	} else if (!forward && previous >= 0 && previous < position) {
		return adjacentPosition(previous, false, cancel, cache);
	}
	return (previous >= 0) ? std::make_optional(previous) : std::nullopt;
}

QImage Extractor::take(
		crl::time position,
		const std::shared_ptr<std::atomic<bool>> &cancel,
		Fn<void(crl::time, QImage)> cache,
		bool exact) {
	if (cancel && cancel->load()) {
		return {};
	}
	const auto known = ranges::lower_bound(_positions, position);
	if (known != end(_positions)
		&& *known == _decodedPosition
		&& FrameHasData(_kept.get())) {
		return ConvertFrame(
			_kept.get(),
			_source.rotation,
			_request.box,
			_request.cover,
			_scale);
	}
	const auto stream = _source.videoStream;
	const auto behind = (_decodedPosition < 0)
		|| (position < _decodedPosition);
	const auto tooFarAhead = !behind
		&& (position - _decodedPosition > kForwardDecodeWindow);
	if (behind || tooFarAhead) {
		seekTo(position);
	}
	auto has = false;
	auto firstAt = crl::time(-1);
	while (true) {
		if (!receiveInto(_frame.get(), cancel)) {
			break;
		}
		const auto at = FramePosition(_frame.get(), stream);
		if (at >= 0) {
			_decodedPosition = at;
			if (firstAt < 0) {
				firstAt = at;
			}
		}
		keepDecodedFrame(at);
		cacheDecodedFrame(at, position, cache);
		has = true;
		av_frame_unref(_frame.get());
		if (at < 0 || at >= position) {
			break;
		} else if (!exact && firstAt >= 0 && at - firstAt > kMaxDecodeSpan) {
			break;
		}
	}
	const auto seeked = (behind || tooFarAhead);
	if ((cancel && cancel->load())
		|| (!has && seeked)
		|| !FrameHasData(_kept.get())) {
		return {};
	}
	return ConvertFrame(
		_kept.get(),
		_source.rotation,
		_request.box,
		_request.cover,
		_scale);
}

} // namespace

FileInfo ReadFileInfo(const QString &path, const QByteArray &content) {
	auto source = Source();
	if (!OpenSource(source, path, content, true)) {
		return {};
	}
	const auto stream = source.videoStream;
	const auto original = QSize(
		stream->codecpar->width,
		stream->codecpar->height);
	return {
		.dimensions = TransposeSizeByRotation(original, source.rotation),
		.duration = SourceDuration(source),
	};
}

void ExtractFrames(
		const QString &path,
		const QByteArray &content,
		const ExtractRequest &request,
		ExtractCallback callback) {
	Expects(callback != nullptr);

	if (request.positions.empty()) {
		return;
	}
	auto source = Source();
	if (!OpenSource(source, path, content, true)) {
		return;
	}
	auto extractor = Extractor(source, request);
	if (!extractor.valid()) {
		return;
	}
	for (auto i = 0, count = int(request.positions.size()); i != count; ++i) {
		auto frame = extractor.take(request.positions[i]);
		if (!callback(i, std::move(frame))) {
			return;
		}
	}
}

struct FrameReader::Private {
	struct Preview {
		crl::time position = 0;
		QSize box;
		QImage image;
	};

	Private(QString path, QByteArray content)
	: path(std::move(path))
	, content(std::move(content)) {
	}

	[[nodiscard]] bool ensureOpen() {
		if (!attempted) {
			attempted = true;
			if (OpenSource(source, path, content, true)) {
				extractor = std::make_unique<Extractor>(source, request);
			}
		}
		return extractor && extractor->valid();
	}

	void cachePreview(crl::time position, QSize box, QImage image) {
		if (image.isNull() || box.isEmpty()) {
			return;
		}
		const auto bytes = image.sizeInBytes();
		if (bytes > kMaxPreviewCacheBytes) {
			return;
		}
		const auto lock = std::lock_guard(cacheMutex);
		const auto existing = ranges::find_if(previews, [&](const auto &p) {
			return p.position == position && p.box == box;
		});
		if (existing != end(previews)) {
			previewBytes -= existing->image.sizeInBytes();
			previews.erase(existing);
		}
		while (!previews.empty()
			&& (previewBytes + bytes > kMaxPreviewCacheBytes
				|| previews.size() >= kMaxPreviewCacheFrames)) {
			previewBytes -= previews.front().image.sizeInBytes();
			previews.erase(begin(previews));
		}
		previews.push_back({ position, box, std::move(image) });
		previewBytes += bytes;
	}

	const QString path;
	const QByteArray content;
	std::mutex mutex;
	std::mutex cacheMutex;
	Source source;
	ExtractRequest request;
	std::unique_ptr<Extractor> extractor;
	std::vector<Preview> previews;
	int64 previewBytes = 0;
	bool attempted = false;
};

FrameReader::FrameReader(QString path, QByteArray content)
: _private(std::make_unique<Private>(std::move(path), std::move(content))) {
}

FrameReader::~FrameReader() = default;

std::optional<crl::time> FrameReader::adjacentPosition(
		crl::time position,
		bool forward,
		QSize box,
		const std::shared_ptr<std::atomic<bool>> &cancel) {
	const auto lock = std::lock_guard(_private->mutex);
	if ((cancel && cancel->load()) || !_private->ensureOpen()) {
		return std::nullopt;
	}
	_private->request.box = box;
	auto cache = Fn<void(crl::time, QImage)>();
	if (!forward && !box.isEmpty()) {
		cache = [&](crl::time at, QImage image) {
			_private->cachePreview(at, box, std::move(image));
		};
	}
	return _private->extractor->adjacentPosition(position, forward, cancel, cache);
}

QImage FrameReader::cachedFrame(crl::time position, QSize box) {
	const auto lock = std::lock_guard(_private->cacheMutex);
	const auto cached = ranges::find_if(_private->previews, [&](const auto &p) {
		return p.position == position && p.box == box;
	});
	return (cached != end(_private->previews)) ? cached->image : QImage();
}

QImage FrameReader::frame(
		crl::time position,
		QSize box,
		const std::shared_ptr<std::atomic<bool>> &cancel,
		bool preloadPrevious) {
	if (cancel && cancel->load()) {
		return {};
	}
	auto cached = cachedFrame(position, box);
	if (!cached.isNull()) {
		return cached;
	}
	const auto lock = std::lock_guard(_private->mutex);
	if ((cancel && cancel->load()) || !_private->ensureOpen()) {
		return {};
	}
	_private->request.box = box;
	auto cache = Fn<void(crl::time, QImage)>();
	if (preloadPrevious && !box.isEmpty()) {
		cache = [&](crl::time at, QImage image) {
			_private->cachePreview(at, box, std::move(image));
		};
	}
	auto result = _private->extractor->take(position, cancel, cache, true);
	_private->cachePreview(position, box, result);
	return result;
}

QImage ExtractFrame(
		const QString &path,
		const QByteArray &content,
		crl::time position,
		QSize box) {
	auto result = QImage();
	ExtractFrames(path, content, {
		.positions = { position },
		.box = box,
	}, [&](int index, QImage &&frame) {
		result = std::move(frame);
		return false;
	});
	return result;
}

} // namespace Media::Video
