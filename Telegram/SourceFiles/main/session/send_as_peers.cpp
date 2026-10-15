/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "main/session/send_as_peers.h"

#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "apiwrap.h"

namespace Main {
namespace {

constexpr auto kRequestEach = 30 * crl::time(1000);

} // namespace

SendAsPeers::SendAsPeers(not_null<Session*> session)
: _session(session)
, _onlyMe({ { .peer = session->user(), .premiumRequired = false } })
, _onlyMePaid({ session->user() }) {
	_session->changes().peerUpdates(
		Data::PeerUpdate::Flag::Rights
	) | rpl::map([=](const Data::PeerUpdate &update) {
		const auto peer = update.peer;
		const auto channel = peer->asChannel();
		const auto bits = 0
			| (peer->amAnonymous() ? (1 << 0) : 0)
			| ((channel && channel->isPublic()) ? (1 << 1) : 0)
			| ((channel && channel->addsSignature()) ? (1 << 2) : 0)
			| ((channel && channel->signatureProfiles()) ? (1 << 3) : 0);
		return std::tuple(peer, bits);
	}) | rpl::distinct_until_changed(
	) | rpl::filter([=](not_null<PeerData*> peer, int) {
		return _lists.contains(peer) || _lastRequestTime.contains(peer);
	}) | rpl::on_next([=](not_null<PeerData*> peer, int) {
		refresh(peer, true);
	}, _lifetime);
	_session->domain().accountsChanges() | rpl::on_next([=] {
		watchAccounts();
		accountsUpdated();
	}, _lifetime);
	watchAccounts();
}

void SendAsPeers::watchAccounts() {
	_accountsLifetime.destroy();
	for (const auto &entry : _session->domain().accounts()) {
		const auto account = entry.account.get();
		if (account == &_session->account()) {
			continue;
		}
		account->sessionValue() | rpl::map([=](Session *session)
		-> rpl::producer<> {
			if (!session) {
				return rpl::single(rpl::empty);
			}
			if (!_accountChats.empty()) {
				loadAccountPeers(session);
			}
			return rpl::merge(
					session->changes().topicUpdates(
						Data::TopicUpdate::Flag::Closed
					) | rpl::to_empty,
					session->changes().peerUpdates(
						Data::PeerUpdate::Flag::Rights
						| Data::PeerUpdate::Flag::ChannelAmIn
						| Data::PeerUpdate::Flag::FullInfo
						| Data::PeerUpdate::Flag::Members
						| Data::PeerUpdate::Flag::Name
						| Data::PeerUpdate::Flag::Photo
					) | rpl::filter([=](const Data::PeerUpdate &update) {
						return update.peer == session->user()
							|| ranges::any_of(_accountChats, [&](const auto peer) {
								return peer->id == update.peer->id;
							});
					}) | rpl::to_empty,
					session->changes().historyUpdates(
						Data::HistoryUpdate::Flag::ClientSideMessages
					) | rpl::filter([=](const Data::HistoryUpdate &update) {
						return ranges::any_of(_chosenAccounts, [&](const auto &entry) {
							return entry.first->id == update.history->peer->id
								&& entry.second == session->userId();
						});
					}) | rpl::to_empty,
					session->frozenValue() | rpl::to_empty,
					session->data().chatsListLoadedEvents() | rpl::to_empty);
		}) | rpl::flatten_latest() | rpl::on_next([=] {
			accountsUpdated();
		}, _accountsLifetime);
	}
}

void SendAsPeers::accountsUpdated() {
	const auto peers = _accountChats;
	for (const auto peer : peers) {
		_updates.fire({ peer });
	}
}

void SendAsPeers::loadAccountPeers(
		not_null<Session*> session,
		int folder,
		int offsetDate,
		int offsetId,
		PeerId offsetPeer) {
	const auto key = session->uniqueId();
	if (!folder && !offsetId) {
		const auto i = _lookups.find(key);
		if (i != end(_lookups) && i->second.get() == session) {
			return;
		}
		_lookups[key] = base::make_weak(session);
	}
	using Flag = MTPmessages_GetDialogs::Flag;
	session->api().request(MTPmessages_GetDialogs(
		MTP_flags(Flag::f_folder_id
			| (offsetId ? Flag::f_exclude_pinned : Flag())),
		MTP_int(folder),
		MTP_int(offsetDate),
		MTP_int(offsetId),
		offsetPeer
			? session->data().peer(offsetPeer)->input()
			: MTP_inputPeerEmpty(),
		MTP_int(100),
		MTP_long(0)
	)).done(crl::guard(_session, [=](const MTPmessages_Dialogs &result) {
		auto nextPeer = PeerId();
		auto nextId = 0;
		auto nextDate = 0;
		const auto parse = [&](const auto &data) {
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			for (const auto &entry : ranges::views::reverse(data.vdialogs().v)) {
				if (entry.type() == mtpc_dialog) {
					nextPeer = peerFromMTP(entry.c_dialog().vpeer());
					nextId = entry.c_dialog().vtop_message().v;
					break;
				}
			}
			for (const auto &entry : data.vmessages().v) {
				entry.match([&](const MTPDmessageEmpty &) {
				}, [&](const auto &message) {
					if (message.vid().v == nextId
						&& peerFromMTP(message.vpeer_id()) == nextPeer) {
						nextDate = message.vdate().v;
					}
				});
			}
		};
		result.match([&](const MTPDmessages_dialogs &data) {
			parse(data);
		}, [&](const MTPDmessages_dialogsSlice &data) {
			parse(data);
		}, [](const MTPDmessages_dialogsNotModified &) {});
		for (const auto source : _accountChats) {
			const auto peer = session->data().peerLoaded(source->id);
			if (peer && !peer->isFullLoaded()) {
				session->api().requestFullPeer(peer);
			}
		}
		accountsUpdated();
		if (result.type() == mtpc_messages_dialogsSlice
			&& nextDate && nextId
			&& (nextId != offsetId || nextPeer != offsetPeer)) {
			loadAccountPeers(session, folder, nextDate, nextId, nextPeer);
		} else if (!folder) {
			loadAccountPeers(session, 1);
		}
	})).fail(crl::guard(_session, [=] {
		_lookups.remove(key);
	})).send();
}

UserId SendAsPeers::chosenAccount(not_null<PeerData*> peer) const {
	const auto i = _chosenAccounts.find(peer);
	return (i == end(_chosenAccounts)) ? UserId() : i->second;
}

PeerData *SendAsPeers::accountPeer(
		not_null<PeerData*> peer,
		UserId accountId) const {
	if (peer->isUser() || peer->isMonoforum()
		|| !accountId || accountId == _session->userId()) {
		return nullptr;
	}
	for (const auto &entry : _session->domain().accounts()) {
		const auto session = entry.account->maybeSession();
		if (!session
			|| session == _session
			|| session->userId() != accountId
			|| session->mtp().environment() != _session->mtp().environment()
			|| session->frozen()) {
			continue;
		}
		const auto other = session->data().peerLoaded(peer->id);
		if (!other) {
			return nullptr;
		}
		const auto channel = other->asChannel();
		const auto chat = other->asChat();
		return ((channel && channel->amIn()) || (chat && chat->amIn()))
			&& (!other->isBroadcast() || channel->canPostMessages())
			&& Data::CanSendAnything(other, false)
			? other
			: nullptr;
	}
	return nullptr;
}

not_null<PeerData*> SendAsPeers::displayChosen(
		not_null<PeerData*> peer) const {
	if (const auto accountId = chosenAccount(peer)) {
		return _session->data().user(accountId);
	}
	return resolveChosen(peer);
}

bool SendAsPeers::shouldChoose(SendAsKey key) {
	refresh(key);
	if (key.type == SendAsType::Message) {
		const auto available = choices(key.peer);
		if ((chosenAccount(key.peer) && Data::CanSendAnything(key.peer, false))
			|| ranges::any_of(available, [&](const SendAsPeer &entry) {
				return entry.accountId && accountPeer(key.peer, entry.accountId);
			})) {
			return true;
		}
	}
	if (list(key).size() < 2) {
		return false;
	}
	if (key.type == SendAsType::VideoStream) {
		return true;
	}
	const auto channel = key.peer->asBroadcast();
	return Data::CanSendAnything(key.peer, false)
		&& (!channel
			|| channel->addsSignature()
			|| channel->signatureProfiles());
}

void SendAsPeers::refresh(SendAsKey key, bool force) {
	if (key.type == SendAsType::Message && !key.peer->isUser()) {
		_accountChats.emplace(key.peer);
	}
	if (key.peer->isUser()
		&& key.type != SendAsType::VideoStream) {
		return;
	}
	const auto now = crl::now();
	const auto i = _lastRequestTime.find(key);
	const auto when = (i == end(_lastRequestTime)) ? -1 : i->second;
	if (!force && (when >= 0 && now < when + kRequestEach)) {
		return;
	}
	_lastRequestTime[key] = now;
	if (key.type == SendAsType::Message) {
		for (const auto &entry : _session->domain().accounts()) {
			const auto session = entry.account->maybeSession();
			if (!session || session == _session
				|| session->mtp().environment() != _session->mtp().environment()) {
				continue;
			}
			if (const auto peer = session->data().peerLoaded(key.peer->id)) {
				session->api().requestFullPeer(peer);
			} else {
				loadAccountPeers(session);
			}
		}
	}
	if (key.peer->isChannel() || key.type == SendAsType::VideoStream) {
		request(key);
		if (key.type == SendAsType::Message) {
			request({ key.peer, SendAsType::PaidReaction });
		}
	}
}

const std::vector<SendAsPeer> &SendAsPeers::list(SendAsKey key) const {
	const auto i = _lists.find(key);
	return (i != end(_lists)) ? i->second : _onlyMe;
}

std::vector<SendAsPeer> SendAsPeers::choices(not_null<PeerData*> peer) const {
	auto result = list({ peer });
	if (peer->isUser()) {
		return result;
	}
	for (const auto &entry : _session->domain().accounts()) {
		const auto session = entry.account->maybeSession();
		if (!session || !accountPeer(peer, session->userId())) {
			continue;
		}
		const auto user = session->user();
		const auto display = _session->data().user(session->userId());
		if (!display->isLoaded()) {
			display->setName(user->firstName, user->lastName, {}, {});
		}
		result.push_back({ .peer = display, .accountId = session->userId() });
	}
	const auto selected = chosenAccount(peer);
	if (selected && !ranges::contains(result, selected, &SendAsPeer::accountId)) {
		result.push_back({
			.peer = _session->data().user(selected),
			.accountId = selected,
		});
	}
	return result;
}

rpl::producer<SendAsKey> SendAsPeers::updated() const {
	return _updates.events();
}

void SendAsPeers::saveChosen(
		not_null<PeerData*> peer,
		not_null<PeerData*> chosen) {
	const auto available = choices(peer);
	const auto account = ranges::find(available, chosen, &SendAsPeer::peer);
	if (account != end(available) && account->accountId) {
		if (!accountPeer(peer, account->accountId)) {
			return;
		}
		_chosenAccounts[peer] = account->accountId;
		_updates.fire({ peer });
		return;
	}
	if (chosen->isUser() && chosen != _session->user()) {
		return;
	}
	const auto hadAccount = _chosenAccounts.remove(peer);
	if (!peer->isChannel()) {
		if (hadAccount) {
			_updates.fire({ peer });
		}
		return;
	}
	peer->session().api().request(MTPmessages_SaveDefaultSendAs(
		peer->input(),
		chosen->input()
	)).send();

	setChosen(peer, chosen->id);
	if (hadAccount) {
		_updates.fire({ peer });
	}
}

void SendAsPeers::setChosen(not_null<PeerData*> peer, PeerId chosenId) {
	if (chosen(peer) == chosenId) {
		return;
	}
	const auto fallback = peer->amAnonymous()
		? peer
		: peer->session().user();
	if (fallback->id == chosenId) {
		_chosen.remove(peer);
	} else {
		_chosen[peer] = chosenId;
	}
	_updates.fire({ peer });
}

PeerId SendAsPeers::chosen(not_null<PeerData*> peer) const {
	const auto i = _chosen.find(peer);
	return (i != end(_chosen)) ? i->second : PeerId();
}

not_null<PeerData*> SendAsPeers::resolveChosen(
		not_null<PeerData*> peer) const {
	return ResolveChosen(peer, list({ peer }), chosen(peer));
}

not_null<PeerData*> SendAsPeers::ResolveChosen(
		not_null<PeerData*> peer,
		const std::vector<SendAsPeer> &list,
		PeerId chosen) {
	const auto fallback = peer->amAnonymous()
		? peer
		: peer->session().user();
	if (!chosen) {
		chosen = fallback->id;
	}
	const auto i = ranges::find(list, chosen, [](const SendAsPeer &as) {
		return as.peer->id;
	});
	return (i != end(list))
		? i->peer
		: !list.empty()
		? list.front().peer
		: fallback;
}

void SendAsPeers::request(SendAsKey key) {
	using Flag = MTPchannels_GetSendAs::Flag;
	key.peer->session().api().request(MTPchannels_GetSendAs(
		MTP_flags((key.type == SendAsType::PaidReaction)
			? Flag::f_for_paid_reactions
			: (key.type == SendAsType::VideoStream)
			? Flag::f_for_live_stories
			: Flag()),
		key.peer->input()
	)).done([=](const MTPchannels_SendAsPeers &result) {
		auto parsed = std::vector<SendAsPeer>();
		auto &owner = key.peer->owner();
		result.match([&](const MTPDchannels_sendAsPeers &data) {
			owner.processUsers(data.vusers());
			owner.processChats(data.vchats());
			const auto &list = data.vpeers().v;
			parsed.reserve(list.size());
			for (const auto &as : list) {
				const auto &data = as.data();
				const auto peerId = peerFromMTP(data.vpeer());
				if (const auto peer = owner.peerLoaded(peerId)) {
					parsed.push_back({
						.peer = peer,
						.premiumRequired = data.is_premium_required(),
					});
				}
			}
		});
		if (parsed.size() > 1) {
			auto &now = _lists[key];
			if (now != parsed) {
				now = std::move(parsed);
				_updates.fire_copy(key);
			}
		} else if (const auto i = _lists.find(key); i != end(_lists)) {
			_lists.erase(i);
			_updates.fire_copy(key);
		}
	}).send();
}

} // namespace Main
