#pragma once

#include "ui/text/text_custom_emoji.h"
#include <QtCore/QMap>
#include <QtNetwork/QNetworkAccessManager>

namespace Main {
class Session;
} // namespace Main

namespace Ui {

struct CustomBadgeInfo {
	QString english;
	QString russian;
	uint64 type = 0;
	DocumentId document = 0;
	[[nodiscard]] QString description() const;
};

class CustomBadges final : public QObject {
public:
	explicit CustomBadges(not_null<Main::Session*> session);
	[[nodiscard]] const CustomBadgeInfo *lookup(not_null<PeerData*> peer) const;
	[[nodiscard]] rpl::producer<> updated() const;

private:
	void fetch();
	[[nodiscard]] bool apply(const QByteArray &bytes);
	const not_null<Main::Session*> _session;
	QNetworkAccessManager _network;
	QMap<int64, CustomBadgeInfo> _badges;
	rpl::event_stream<> _updated;

};

class CustomBadge final {
public:
	[[nodiscard]] static int Width(PeerData *peer);
	[[nodiscard]] QRect rect() const;
	void paint(
		QPainter &p,
		not_null<PeerData*> peer,
		QRect rect,
		QColor color,
		crl::time now,
		bool paused,
		Fn<void()> repaint);

private:
	DocumentId _document = 0;
	uint64 _type = uint64(-1);
	QColor _color;
	QRect _rect;
	std::unique_ptr<Text::CustomEmoji> _emoji;

};

} // namespace Ui
