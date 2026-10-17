#include "core/xenon_updates.h"

#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/update_channel.h"
#include "core/update_checker.h"
#include "core/version.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "xenon_build.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>

#include <limits>

#include "styles/style_layers.h"

namespace Core::XenonUpdates {
namespace {

constexpr auto kInterval = 3600;
constexpr auto kResponseLimit = 4 * 1024 * 1024;
constexpr auto kChangelogLimit = 8 * 1024 * 1024;
constexpr auto kCheckPreference = "xenon-check-updates";
constexpr auto kInstallPreference = "xenon-install-updates";
constexpr auto kSkippedPreference = "xenon-skipped-release";

QString ApiRoot() {
	return u"https://api.github.com/repos/miuichina/xenondesktop"_q;
}

struct Release {
	QString tag;
	QString commit;
	QString package;
	QString changes;
	qint64 time = 0;
	quint64 version = 0;
};

class Service final : public QObject {
public:
	Service();
	~Service() override;
	void check(bool manual, bool force = false);
	void preferencesChanged();
	[[nodiscard]] rpl::producer<QString> status() const;

private:
	void get(const QString &url, Fn<void(QJsonDocument)> done);
	void metadata(const QJsonObject &release);
	void compare(int page);
	void offer();
	void download();
	void fail();
	void finish();

	QNetworkAccessManager _network;
	QTimer _timer;
	QTimer _timeout;
	QPointer<QNetworkReply> _reply;
	QPointer<Ui::GenericBox> _box;
	UpdateChecker _checker;
	rpl::variable<QString> _status;
	Release _release;
	bool _busy = false;
	bool _manual = false;
	bool _force = false;
	bool _downloading = false;
	int _totalCommits = 0;
	int _receivedCommits = 0;
	qint64 _nextCheck = 0;
	rpl::lifetime _lifetime;

};

Service &Instance() {
	static auto instance = QPointer<Service>();
	if (!instance) {
		instance = new Service();
	}
	return *instance;
}

Service::Service() : QObject(QCoreApplication::instance()) {
	_timer.setSingleShot(true);
	_timeout.setSingleShot(true);
	connect(&_timer, &QTimer::timeout, this, [=] { check(false); });
	connect(&_timeout, &QTimer::timeout, this, [=] {
		if (_reply) {
			_reply->abort();
		}
	});
	_checker.progress() | rpl::on_next([=] {
		if (_downloading) {
			_status = _checker.statusText();
		}
	}, _lifetime);
	_checker.failed() | rpl::on_next([=] {
		if (_downloading) {
			_downloading = false;
			fail();
		}
	}, _lifetime);
	_checker.ready() | rpl::on_next([=] {
		if (_downloading) {
			_downloading = false;
			_status = tr::lng_xenon_update_ready(tr::now);
		}
	}, _lifetime);
}

Service::~Service() {
	if (_reply) {
		_reply->disconnect(this);
		_reply->abort();
	}
}

rpl::producer<QString> Service::status() const {
	return _status.value();
}

void Service::preferencesChanged() {
	_timer.stop();
	if (AutomaticallyCheck()) {
		check(false);
	}
}

void Service::check(bool manual, bool force) {
	if (_busy || _downloading || Quitting()) {
		return;
	}
	if (force) {
		if (_box) {
			_box->closeBox();
			_box = nullptr;
		}
		_checker.discard();
	} else if (_box || _checker.state() == UpdateChecker::State::Ready) {
		finish();
		return;
	} else if (!manual && !AutomaticallyCheck()) {
		return;
	}
	const auto now = base::unixtime::now();
	if (!manual && _nextCheck > now) {
		_timer.start(int(std::min(_nextCheck - now, qint64(24 * kInterval))) * 1000);
		return;
	}
	_busy = true;
	_manual = manual;
	_force = force;
	_release = {};
	_totalCommits = 0;
	_receivedCommits = 0;
	_status = tr::lng_xenon_update_checking(tr::now);
	get(ApiRoot() + u"/releases/latest"_q, [=](QJsonDocument result) {
		if (!result.isObject()) {
			return fail();
		}
		metadata(result.object());
	});
}

void Service::get(const QString &url, Fn<void(QJsonDocument)> done) {
	auto request = QNetworkRequest(QUrl(url));
	request.setRawHeader("User-Agent", "Xenon-Desktop-Updater");
	request.setRawHeader("Accept", "application/vnd.github+json");
	request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setTransferTimeout(30000);
	const auto reply = _network.get(request);
	_reply = reply;
	_timeout.start(60000);
	connect(reply, &QNetworkReply::readyRead, this, [=] {
		if (reply->bytesAvailable() > kResponseLimit) {
			reply->abort();
		}
	});
	connect(reply, &QNetworkReply::finished, this, [=] {
		_timeout.stop();
		_reply = nullptr;
		reply->deleteLater();
		const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (reply->error() != QNetworkReply::NoError || status != 200) {
			if (status == 403 || status == 429) {
				_nextCheck = std::max(
					qint64(base::unixtime::now()) + kInterval,
					std::min(reply->rawHeader("X-RateLimit-Reset").toLongLong(),
						qint64(base::unixtime::now()) + 24 * kInterval));
			}
			return fail();
		}
		const auto bytes = reply->readAll();
		auto error = QJsonParseError();
		const auto document = QJsonDocument::fromJson(bytes, &error);
		if (bytes.size() > kResponseLimit || error.error != QJsonParseError::NoError) {
			return fail();
		}
		done(document);
	});
}

void Service::metadata(const QJsonObject &release) {
	_release.tag = release.value(u"tag_name"_q).toString();
	if (!QRegularExpression(u"^windows-build-[0-9]+$"_q).match(_release.tag).hasMatch()
		|| release.value(u"draft"_q).toBool()
		|| release.value(u"prerelease"_q).toBool()) {
		return fail();
	}
	const auto prefix = u"https://github.com/miuichina/xenondesktop/releases/download/"_q
		+ _release.tag + '/';
	auto metadataUrl = QString();
	const auto assets = release.value(u"assets"_q).toArray();
	for (const auto &entry : assets) {
		const auto asset = entry.toObject();
		if (asset.value(u"name"_q).toString() == u"xenon-update.json"_q) {
			metadataUrl = asset.value(u"browser_download_url"_q).toString();
		}
	}
	if (metadataUrl != prefix + u"xenon-update.json"_q) {
		return fail();
	}
	get(metadataUrl, [=](QJsonDocument document) {
		const auto data = document.object();
		_release.commit = data.value(u"commit"_q).toString();
		_release.time = data.value(u"commit_time"_q).toInteger();
		const auto base = data.value(u"base_version"_q).toInteger();
		const auto build = data.value(u"build_number"_q).toInteger();
		const auto name = data.value(u"package"_q).toString();
		if (data.value(u"format"_q).toInt() != 1
			|| data.value(u"tag"_q).toString() != _release.tag
			|| !QRegularExpression(u"^[0-9a-f]{40}$"_q).match(_release.commit).hasMatch()
			|| _release.time <= 0 || base <= 0 || base > 999999999
			|| build <= 0 || build > std::numeric_limits<quint32>::max()
			|| name != u"td-update-win-x64-"_q + QString::number(base)) {
			return fail();
		}
		_release.version = Updates::MakeUpdateVersion(quint32(base), quint32(build));
		if (!_force && (_release.commit == QLatin1String(CommitHash)
			|| _release.version <= RunningUpdateVersion())) {
			_status = tr::lng_xenon_update_latest(tr::now);
			return finish();
		}
		for (const auto &entry : assets) {
			const auto asset = entry.toObject();
			if (asset.value(u"name"_q).toString() == name
				&& asset.value(u"browser_download_url"_q).toString() == prefix + name
				&& asset.value(u"size"_q).toInteger() > 0
				&& asset.value(u"size"_q).toInteger() <= MTP::AbstractDedicatedLoader::kMaxFileSize) {
				_release.package = prefix + name;
			}
		}
		if (_release.package.isEmpty()) {
			return fail();
		} else if (!_manual && App().settings().readPref<QByteArray>(
				kSkippedPreference) == _release.tag.toUtf8()) {
			_status = tr::lng_xenon_update_skipped(tr::now);
			return finish();
		}
		if (_force) {
			finish();
			download();
		} else {
			compare(1);
		}
	});
}

void Service::compare(int page) {
	get(ApiRoot() + u"/compare/"_q + QLatin1String(CommitHash)
		+ u"..."_q + _release.commit
		+ u"?per_page=100&page="_q + QString::number(page), [=](QJsonDocument document) {
		const auto data = document.object();
		if (data.value(u"status"_q).toString() != u"ahead"_q
			|| data.value(u"base_commit"_q).toObject().value(u"sha"_q).toString() != QLatin1String(CommitHash)) {
			return fail();
		}
		const auto total = data.value(u"total_commits"_q).toInt();
		if (total <= 0 || total > 100000 || (page > 1 && total != _totalCommits)) {
			return fail();
		}
		_totalCommits = total;
		const auto commits = data.value(u"commits"_q).toArray();
		if (commits.isEmpty() || commits.size() > 100) {
			return fail();
		}
		for (const auto &entry : commits) {
			const auto commit = entry.toObject();
			const auto hash = commit.value(u"sha"_q).toString();
			const auto details = commit.value(u"commit"_q).toObject();
			const auto message = details.value(u"message"_q).toString().section('\n', 0, 0);
			if (!QRegularExpression(u"^[0-9a-f]{40}$"_q).match(hash).hasMatch() || message.isEmpty()) {
				return fail();
			}
			_release.changes += hash.left(8) + u"  "_q + message + '\n';
			++_receivedCommits;
		}
		if (_receivedCommits > _totalCommits || _release.changes.size() > kChangelogLimit) {
			return fail();
		} else if (_receivedCommits < _totalCommits) {
			return compare(page + 1);
		}
		finish();
		_status = tr::lng_xenon_update_available(tr::now);
		if (AutomaticallyInstall() && !_manual) {
			download();
		} else {
			offer();
		}
	});
}

void Service::offer() {
	if (_box || Quitting()) {
		return;
	}
	const auto window = App().activePrimaryWindow();
	if (!window) {
		return;
	}
	window->show(Box([=](not_null<Ui::GenericBox*> box) {
		_box = box.get();
		box->setTitle(tr::lng_xenon_update_title());
		box->setWidth(st::boxWideWidth);
		box->setMaxHeight(st::boxMaxListHeight);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(BuildDescription() + u" → "_q + _release.commit.left(8)
				+ u"\n"_q + QDateTime::fromSecsSinceEpoch(_release.time).toString(Qt::ISODate)),
			st::boxLabel));
		box->addRow(object_ptr<Ui::FlatLabel>(box, status(), st::boxLabel));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(_release.changes),
			st::boxLabel));
		box->addButton(tr::lng_xenon_update_action(), [=] {
			if (_downloading) {
				return;
			}
			box->closeBox();
			download();
		});
		box->addButton(tr::lng_xenon_update_skip(), [=] {
			if (_downloading) {
				return;
			}
			_checker.discard();
			App().settings().writePref<QByteArray>(kSkippedPreference, _release.tag.toUtf8());
			App().saveSettingsDelayed();
			_status = tr::lng_xenon_update_skipped(tr::now);
			box->closeBox();
			finish();
		});
	}));
}

void Service::download() {
	if (_downloading || _release.package.isEmpty()) {
		return;
	}
	_downloading = true;
	_status = tr::lng_xenon_update_downloading(
		tr::now,
		lt_percent,
		QString::number(0));
	_checker.download(_release.package, _release.version, _force);
}

void Service::fail() {
	_status = tr::lng_xenon_update_failed(tr::now);
	finish();
}

void Service::finish() {
	_busy = false;
	_nextCheck = std::max(_nextCheck, qint64(base::unixtime::now()) + kInterval);
	if (AutomaticallyCheck()) {
		_timer.start(int(_nextCheck - base::unixtime::now()) * 1000);
	}
}

} // namespace

bool Supported() {
#if defined Q_OS_WIN && defined _WIN64 && !defined _M_ARM64 && !defined OS_WIN_STORE
	return !UpdaterDisabled();
#else
	return false;
#endif
}

bool AutomaticallyCheck() {
	return App().settings().readPref<bool>(kCheckPreference, true);
}

bool AutomaticallyInstall() {
	return App().settings().readPref<bool>(kInstallPreference, false);
}

void SetAutomaticallyCheck(bool enabled) {
	App().settings().writePref<bool>(kCheckPreference, enabled);
	App().saveSettingsDelayed();
	Instance().preferencesChanged();
}

void SetAutomaticallyInstall(bool enabled) {
	App().settings().writePref<bool>(kInstallPreference, enabled);
	App().saveSettingsDelayed();
}

void Start(bool scheduled) {
	if (Supported() && IsAppLaunched() && !scheduled && !cManyInstance()) {
		Instance().check(false);
	}
}

void CheckNow() {
	if (Supported()) {
		Instance().check(true);
	}
}

void ForceUpdate() {
	if (Supported()) {
		Instance().check(true, true);
	}
}

rpl::producer<QString> Status() {
	return Instance().status();
}

QString BuildDescription() {
	return u"Xenon %1 · %2 · %3"_q
		.arg(BuildNumber)
		.arg(QLatin1String(CommitHash).left(8))
		.arg(QDateTime::fromSecsSinceEpoch(CommitTime).toString(Qt::ISODate));
}

} // namespace Core::XenonUpdates
