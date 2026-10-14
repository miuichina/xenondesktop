#include "ui/custom_badge.h"

#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/stickers/data_custom_emoji.h"
#include "lang/lang_instance.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/painter.h"
#include "ui/power_saving.h"
#include "ui/effects/premium_stars_colored.h"
#include "styles/style_dialogs.h"
#include "styles/style_chat.h"
#include <QtCore/QFile>
#include <QtCore/QSaveFile>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkReply>
#include <cmath>

namespace Ui {
namespace {

constexpr auto kChannelOffset = int64(1000000000000);
constexpr auto kMaximumBytes = 1024 * 1024;
constexpr auto kRefreshInterval = 3 * 60 * 60 * 1000;

class BadgeGlyph final : public Text::CustomEmoji {
public:
	explicit BadgeGlyph(bool shaped) : _shaped(shaped) {
	}

	int width() override { return st::emojiSize; }
	QString entityData() override { return QString(); }
	bool ready() override { return true; }
	bool readyInDefaultState() override { return true; }
	void unload() override {
	}
	void paint(QPainter &p, const Context &context) override {
		p.save();
		PainterHighQualityEnabler hq(p);
		const auto size = st::emojiSize;
		const auto center = QPointF(context.position) + QPointF(size / 2., size / 2.);
		auto color = context.textColor;
		if (_shaped) {
			auto shape = QPolygonF();
			const auto outer = size / 2.;
			for (auto i = 0; i != 8; ++i) {
				const auto angle = (-2 + i) * 3.141592653589793 / 4.;
				const auto radius = outer * ((i % 2) ? 0.62 : 1.);
				shape << center + QPointF(std::cos(angle), std::sin(angle)) * radius;
			}
			p.setPen(Qt::NoPen);
			p.setBrush(st::dialogsVerifiedIconBg->c);
			p.drawPolygon(shape);
			color = st::windowBg->c;
		}
		p.setPen(color);
		p.setFont(st::dialogsScamFont);
		p.drawText(QRect(context.position, QSize(size, size)), Qt::AlignCenter, u":3"_q);
		p.restore();
	}

private:
	bool _shaped = false;

};

QString CachePath() {
	return cWorkingDir() + u"tdata/xenon-custom-badges.csv"_q;
}

} // namespace

QString CustomBadgeInfo::description() const {
	return Lang::GetInstance().id().startsWith(u"ru"_q) && !russian.isEmpty()
		? russian
		: !english.isEmpty() ? english : russian;
}

CustomBadges::CustomBadges(not_null<Main::Session*> session)
: _session(session)
, _network(this) {
	auto cache = QFile(CachePath());
	if (cache.open(QIODevice::ReadOnly) && cache.size() <= kMaximumBytes) {
		if (!apply(cache.readAll())) {
			LOG(("Xenon: invalid custom badge cache."));
		}
	}
	QTimer::singleShot(0, this, [=] { fetch(); });
	const auto timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, [=] { fetch(); });
	timer->start(kRefreshInterval);
}

void CustomBadges::fetch() {
	auto request = QNetworkRequest(QUrl(u"https://gist.githubusercontent.com/miuichina/021db1054eab6820e00c927c910e534a/raw/gistfile1.txt"_q));
	request.setRawHeader("Cache-Control", "no-cache");
	request.setTransferTimeout(10000);
	const auto reply = _network.get(request);
	connect(reply, &QNetworkReply::readyRead, this, [=] {
		if (reply->bytesAvailable() > kMaximumBytes) {
			reply->abort();
		}
	});
	connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError
			|| reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
			return;
		}
		const auto bytes = reply->readAll();
		if (bytes.size() > kMaximumBytes) {
			return;
		}
		if (!apply(bytes)) {
			return;
		}
		auto cache = QSaveFile(CachePath());
		if (cache.open(QIODevice::WriteOnly)) {
			if (cache.write(bytes) == bytes.size()) {
				if (!cache.commit()) {
					LOG(("Xenon: could not save custom badge cache."));
				}
			}
		}
	});
}

bool CustomBadges::apply(const QByteArray &bytes) {
	auto parsed = QMap<int64, CustomBadgeInfo>();
	for (const auto &line : QString::fromUtf8(bytes).split('\n')) {
		const auto fields = line.trimmed().split(',');
		if (fields.size() < 2) {
			continue;
		}
		auto valid = false;
		const auto id = fields[0].trimmed().toLongLong(&valid);
		if (!valid || !id) {
			continue;
		}
		auto info = CustomBadgeInfo{
			.english = fields[1].trimmed(),
			.russian = fields.size() > 2 ? fields[2].trimmed() : QString(),
			.type = fields.size() > 3 ? fields[3].trimmed().toULongLong() : 0,
		};
		info.document = (info.type == 2)
			? DocumentId(5195282850303197154ULL)
			: (info.type == 3 && fields.size() > 4)
			? fields[4].trimmed().toULongLong()
			: (info.type > 3) ? info.type : 0;
		parsed.insert(id, std::move(info));
	}
	if (parsed.isEmpty() && !bytes.trimmed().isEmpty()) {
		return false;
	}
	_badges = std::move(parsed);
	_updated.fire({});
	_session->data().refreshCustomBadges();
	return true;
}

const CustomBadgeInfo *CustomBadges::lookup(not_null<PeerData*> peer) const {
	const auto id = int64(peerToBareMTPInt(peer->id).v);
	const auto keys = peer->isUser()
		? std::array{ id, -id, -kChannelOffset - id }
		: std::array{ -id, -kChannelOffset - id, id };
	for (const auto key : keys) {
		const auto i = _badges.constFind(key);
		if (i != _badges.cend()) {
			return &i.value();
		}
	}
	return nullptr;
}

rpl::producer<> CustomBadges::updated() const {
	return _updated.events();
}

int CustomBadge::Width(PeerData *peer) {
	return peer && peer->session().customBadges().lookup(peer)
		? st::emojiSize + st::msgServiceFont->spacew
		: 0;
}

QRect CustomBadge::rect() const {
	return _rect;
}

void CustomBadge::paint(
		QPainter &p,
		not_null<PeerData*> peer,
		QRect rect,
		QColor color,
		crl::time now,
		bool paused,
		Fn<void()> repaint) {
	const auto info = peer->session().customBadges().lookup(peer);
	if (!info) {
		_emoji = nullptr;
		_rect = {};
		return;
	}
	if (!_emoji || _document != info->document || _type != info->type || _color != color) {
		_document = info->document;
		_type = info->type;
		_color = color;
		_emoji = _document
			? peer->session().data().customEmojiManager().create(
				Data::SerializeCustomEmojiId(_document), repaint)
			: std::make_unique<BadgeGlyph>(_type == 0);
		if (_type == 0 || _type == 3) {
			_emoji = Premium::MakeCollectibleEmoji(
				QStringView(),
				color,
				color,
				std::move(_emoji),
				repaint,
				st::emojiSize);
		}
	}
	const auto size = st::emojiSize;
	const auto position = QPoint(
		rect.x() + st::msgServiceFont->spacew,
		rect.y() + (rect.height() - size) / 2);
	_rect = QRect(position, QSize(size, size));
	_emoji->paint(p, {
		.textColor = color,
		.now = now,
		.position = position,
		.paused = paused || On(PowerSaving::kEmojiStatus),
	});
}

} // namespace Ui
