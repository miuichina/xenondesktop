/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_common.h"

#include "base/qt/qt_key_modifiers.h"
#include "data/data_histories.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "data/data_user.h"
#include "history/history.h"
#include "main/session/send_as_peers.h"
#include "main/main_session.h"

namespace Api {

SendOptions CaptureSendAccount(
		PeerData *peer,
		SendOptions options) {
	if (peer && !options.sendAccountCaptured) {
		options.sendFromAccount = peer->session().sendAsPeers().chosenAccount(peer);
		options.sendAccountCaptured = true;
	}
	return options;
}

bool ResolveAccountSendAction(SendAction &action) {
	const auto accountId = action.options.sendFromAccount;
	if (!accountId) {
		return true;
	}
	const auto peer = action.history->peer;
	const auto target = peer->session().sendAsPeers().accountPeer(
		peer,
		accountId);
	if (!target || action.replaceMediaOf || action.options.shortcutId) {
		return false;
	}
	if (target->starsPerMessageChecked()
		&& !action.history->forwardDraft(
			action.replyTo.topicRootId,
			action.replyTo.monoforumPeerId).ids.empty()) {
		return false;
	}
	if (action.replyTo.messageId
		&& (peer->isChat()
			|| action.replyTo.messageId.peer != peer->id
			|| !IsServerMsgId(action.replyTo.messageId.msg))) {
		return false;
	}
	action.history = target->owner().history(target);
	action.options.sendFromAccount = 0;
	action.options.sendAs = target->isBroadcast()
		? nullptr
		: target->session().user();
	action.options.suggest = {};
	action.clearDraft = false;
	return true;
}

MTPSuggestedPost SuggestToMTP(SuggestOptions suggest) {
	using Flag = MTPDsuggestedPost::Flag;
	return suggest.exists
		? MTP_suggestedPost(
			MTP_flags((suggest.date ? Flag::f_schedule_date : Flag())
				| (suggest.price().empty() ? Flag() : Flag::f_price)),
			StarsAmountToTL(suggest.price()),
			MTP_int(suggest.date))
		: MTPSuggestedPost();
}

SendAction::SendAction(
	not_null<Data::Thread*> thread,
	SendOptions options)
: history(thread->owningHistory())
, options(options)
, replyTo({ .messageId = { history->peer->id, thread->topicRootId() } }) {
	replyTo.topicRootId = replyTo.messageId.msg;
}

SendOptions DefaultSendWhenOnlineOptions() {
	return {
		.scheduled = kScheduledUntilOnlineTimestamp,
		.silent = base::IsCtrlPressed(),
	};
}

MTPInputReplyTo SendAction::mtpReplyTo() const {
	return Data::ReplyToForMTP(history, replyTo);
}

} // namespace Api
