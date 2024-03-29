#include "IrcMessageHandler.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "debug/Log.hpp"
#include "messages/LimitedQueue.hpp"
#include "messages/Message.hpp"
#include "providers/twitch/TwitchAccountManager.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchHelpers.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/twitch/TwitchMessageBuilder.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/IrcHelpers.hpp"

#include <IrcMessage>

#include <unordered_set>

namespace chatterino {

static QMap<QString, QString> parseBadges(QString badgesString)
{
    QMap<QString, QString> badges;

    for (auto badgeData : badgesString.split(','))
    {
        auto parts = badgeData.split('/');
        if (parts.length() != 2)
        {
            continue;
        }

        badges.insert(parts[0], parts[1]);
    }

    return badges;
}

IrcMessageHandler &IrcMessageHandler::instance()
{
    static IrcMessageHandler instance;
    return instance;
}

std::vector<MessagePtr> IrcMessageHandler::parseMessage(
    Channel *channel, Communi::IrcMessage *message)
{
    std::vector<MessagePtr> builtMessages;

    auto command = message->command();

    if (command == "PRIVMSG")
    {
        return this->parsePrivMessage(
            channel, static_cast<Communi::IrcPrivateMessage *>(message));
    }
    else if (command == "USERNOTICE")
    {
        return this->parseUserNoticeMessage(channel, message);
    }
    else if (command == "NOTICE")
    {
        return this->parseNoticeMessage(
            static_cast<Communi::IrcNoticeMessage *>(message));
    }

    return builtMessages;
}

std::vector<MessagePtr> IrcMessageHandler::parsePrivMessage(
    Channel *channel, Communi::IrcPrivateMessage *message)
{
    std::vector<MessagePtr> builtMessages;
    MessageParseArgs args;
    TwitchMessageBuilder builder(channel, message, args, message->content(),
                                 message->isAction());
    if (!builder.isIgnored())
    {
        builtMessages.emplace_back(builder.build());
        builder.triggerHighlights();
    }
    return builtMessages;
}

void IrcMessageHandler::handlePrivMessage(Communi::IrcPrivateMessage *message,
                                          TwitchIrcServer &server)
{
    this->addMessage(message, message->target(), message->content(), server,
                     false, message->isAction());
}

void IrcMessageHandler::addMessage(Communi::IrcMessage *_message,
                                   const QString &target,
                                   const QString &content,
                                   TwitchIrcServer &server, bool isSub,
                                   bool isAction)
{
    QString channelName;
    if (!trimChannelName(target, channelName))
    {
        return;
    }

    auto chan = server.getChannelOrEmpty(channelName);

    if (chan->isEmpty())
    {
        return;
    }

    MessageParseArgs args;
    if (isSub)
    {
        args.trimSubscriberUsername = true;
    }

    if (chan->isBroadcaster())
    {
        args.isStaffOrBroadcaster = true;
    }

    TwitchMessageBuilder builder(chan.get(), _message, args, content, isAction);

    if (isSub || !builder.isIgnored())
    {
        if (isSub)
        {
            builder->flags.set(MessageFlag::Subscription);
            builder->flags.unset(MessageFlag::Highlighted);
        }

        auto msg = builder.build();
        builder.triggerHighlights();
        auto highlighted = msg->flags.has(MessageFlag::Highlighted);

        if (!isSub)
        {
            if (highlighted)
            {
                server.mentionsChannel->addMessage(msg);
                getApp()->highlights->addHighlight(msg);
            }
        }

        chan->addMessage(msg);
        if (auto chatters = dynamic_cast<ChannelChatters *>(chan.get()))
        {
            chatters->addRecentChatter(msg->displayName);
        }
    }
}

void IrcMessageHandler::handleRoomStateMessage(Communi::IrcMessage *message)
{
    const auto &tags = message->tags();
    auto app = getApp();

    // get twitch channel
    QString chanName;
    if (!trimChannelName(message->parameter(0), chanName))
    {
        return;
    }
    auto chan = app->twitch.server->getChannelOrEmpty(chanName);

    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(chan.get()))
    {
        // room-id
        decltype(tags.find("xD")) it;

        if ((it = tags.find("room-id")) != tags.end())
        {
            auto roomId = it.value().toString();

            twitchChannel->setRoomId(roomId);
        }

        // Room modes
        {
            auto roomModes = *twitchChannel->accessRoomModes();

            if ((it = tags.find("emote-only")) != tags.end())
            {
                roomModes.emoteOnly = it.value() == "1";
            }
            if ((it = tags.find("subs-only")) != tags.end())
            {
                roomModes.submode = it.value() == "1";
            }
            if ((it = tags.find("slow")) != tags.end())
            {
                roomModes.slowMode = it.value().toInt();
            }
            if ((it = tags.find("r9k")) != tags.end())
            {
                roomModes.r9k = it.value() == "1";
            }
            if ((it = tags.find("broadcaster-lang")) != tags.end())
            {
                roomModes.broadcasterLang = it.value().toString();
            }
            if ((it = tags.find("followers-only")) != tags.end())
            {
                roomModes.followerOnly = it.value().toInt();
            }
            twitchChannel->setRoomModes(roomModes);
        }

        twitchChannel->roomModesChanged.invoke();
    }
}

void IrcMessageHandler::handleClearChatMessage(Communi::IrcMessage *message)
{
    // check parameter count
    if (message->parameters().length() < 1)
    {
        return;
    }

    QString chanName;
    if (!trimChannelName(message->parameter(0), chanName))
    {
        return;
    }

    auto app = getApp();

    // get channel
    auto chan = app->twitch.server->getChannelOrEmpty(chanName);

    if (chan->isEmpty())
    {
        log("[IrcMessageHandler:handleClearChatMessage] Twitch channel {} not "
            "found",
            chanName);
        return;
    }

    // check if the chat has been cleared by a moderator
    if (message->parameters().length() == 1)
    {
        chan->disableAllMessages();
        chan->addMessage(
            makeSystemMessage("Chat has been cleared by a moderator."));

        return;
    }

    // get username, duration and message of the timed out user
    QString username = message->parameter(1);
    QString durationInSeconds, reason;
    QVariant v = message->tag("ban-duration");
    if (v.isValid())
    {
        durationInSeconds = v.toString();
    }

    v = message->tag("ban-reason");
    if (v.isValid())
    {
        reason = v.toString();
    }

    auto timeoutMsg = MessageBuilder(timeoutMessage, username,
                                     durationInSeconds, reason, false)
                          .release();
    chan->addOrReplaceTimeout(timeoutMsg);

    // refresh all
    app->windows->repaintVisibleChatWidgets(chan.get());
    if (getSettings()->hideModerated)
    {
        app->windows->forceLayoutChannelViews();
    }
}

void IrcMessageHandler::handleClearMessageMessage(Communi::IrcMessage *message)
{
    // check parameter count
    if (message->parameters().length() < 1)
    {
        return;
    }

    QString chanName;
    if (!trimChannelName(message->parameter(0), chanName))
    {
        return;
    }

    auto app = getApp();

    // get channel
    auto chan = app->twitch.server->getChannelOrEmpty(chanName);

    if (chan->isEmpty())
    {
        log("[IrcMessageHandler:handleClearMessageMessage] Twitch channel {} "
            "not "
            "found",
            chanName);
        return;
    }

    auto tags = message->tags();

    QString targetID = tags.value("target-msg-id").toString();

    chan->deleteMessage(targetID);
}

void IrcMessageHandler::handleUserStateMessage(Communi::IrcMessage *message)
{
    auto app = getApp();

    QString channelName;
    if (!trimChannelName(message->parameter(0), channelName))
    {
        return;
    }

    auto c = app->twitch.server->getChannelOrEmpty(channelName);
    if (c->isEmpty())
    {
        return;
    }

    QVariant _badges = message->tag("badges");
    if (_badges.isValid())
    {
        TwitchChannel *tc = dynamic_cast<TwitchChannel *>(c.get());
        if (tc != nullptr)
        {
            auto parsedBadges = parseBadges(_badges.toString());
            tc->setVIP(parsedBadges.contains("vip"));
            tc->setStaff(parsedBadges.contains("staff"));
        }
    }

    QVariant _mod = message->tag("mod");
    if (_mod.isValid())
    {
        TwitchChannel *tc = dynamic_cast<TwitchChannel *>(c.get());
        if (tc != nullptr)
        {
            tc->setMod(_mod == "1");
        }
    }
}

void IrcMessageHandler::handleWhisperMessage(Communi::IrcMessage *message)
{
    auto app = getApp();
    log("Received whisper!");
    MessageParseArgs args;

    args.isReceivedWhisper = true;

    auto c = app->twitch.server->whispersChannel.get();

    TwitchMessageBuilder builder(c, message, args, message->parameter(1),
                                 false);

    if (!builder.isIgnored())
    {
        builder->flags.set(MessageFlag::Whisper);
        MessagePtr _message = builder.build();
        builder.triggerHighlights();

        app->twitch.server->lastUserThatWhisperedMe.set(builder.userName);

        if (_message->flags.has(MessageFlag::Highlighted))
        {
            app->twitch.server->mentionsChannel->addMessage(_message);
        }

        c->addMessage(_message);

        auto overrideFlags = boost::optional<MessageFlags>(_message->flags);
        overrideFlags->set(MessageFlag::DoNotTriggerNotification);
        overrideFlags->set(MessageFlag::DoNotLog);

        if (getSettings()->inlineWhispers)
        {
            app->twitch.server->forEachChannel(
                [&_message, overrideFlags](ChannelPtr channel) {
                    channel->addMessage(_message, overrideFlags);
                });
        }
    }
}

std::vector<MessagePtr> IrcMessageHandler::parseUserNoticeMessage(
    Channel *channel, Communi::IrcMessage *message)
{
    std::vector<MessagePtr> builtMessages;

    auto data = message->toData();

    auto tags = message->tags();
    auto parameters = message->parameters();

    auto target = parameters[0];
    QString msgType = tags.value("msg-id", "").toString();
    QString content;
    if (parameters.size() >= 2)
    {
        content = parameters[1];
    }

    if (msgType == "sub" || msgType == "resub" || msgType == "subgift")
    {
        // Sub-specific message. I think it's only allowed for "resub" messages
        // atm
        if (!content.isEmpty())
        {
            MessageParseArgs args;
            args.trimSubscriberUsername = true;

            TwitchMessageBuilder builder(channel, message, args, content,
                                         false);
            builder->flags.set(MessageFlag::Subscription);
            builder->flags.unset(MessageFlag::Highlighted);
            builtMessages.emplace_back(builder.build());
        }
    }

    auto it = tags.find("system-msg");

    if (it != tags.end())
    {
        auto b = MessageBuilder(systemMessage,
                                parseTagString(it.value().toString()));

        b->flags.set(MessageFlag::Subscription);
        auto newMessage = b.release();
        builtMessages.emplace_back(newMessage);
    }

    return builtMessages;
}

void IrcMessageHandler::handleUserNoticeMessage(Communi::IrcMessage *message,
                                                TwitchIrcServer &server)
{
    auto data = message->toData();

    auto tags = message->tags();
    auto parameters = message->parameters();

    auto target = parameters[0];
    QString msgType = tags.value("msg-id", "").toString();
    QString content;
    if (parameters.size() >= 2)
    {
        content = parameters[1];
    }

    if (msgType == "sub" || msgType == "resub" || msgType == "subgift")
    {
        // Sub-specific message. I think it's only allowed for "resub" messages
        // atm
        if (!content.isEmpty())
        {
            this->addMessage(message, target, content, server, true, false);
        }
    }

    auto it = tags.find("system-msg");

    if (it != tags.end())
    {
        auto b = MessageBuilder(systemMessage,
                                parseTagString(it.value().toString()));

        b->flags.set(MessageFlag::Subscription);
        auto newMessage = b.release();

        QString channelName;

        if (message->parameters().size() < 1)
        {
            return;
        }

        if (!trimChannelName(message->parameter(0), channelName))
        {
            return;
        }

        auto chan = server.getChannelOrEmpty(channelName);

        if (!chan->isEmpty())
        {
            chan->addMessage(newMessage);
        }
    }
}

void IrcMessageHandler::handleModeMessage(Communi::IrcMessage *message)
{
    auto app = getApp();

    auto channel = app->twitch.server->getChannelOrEmpty(
        message->parameter(0).remove(0, 1));

    if (channel->isEmpty())
    {
        return;
    }

    if (message->parameter(1) == "+o")
    {
        channel->modList.append(message->parameter(2));
    }
    else if (message->parameter(1) == "-o")
    {
        channel->modList.append(message->parameter(2));
    }
}

std::vector<MessagePtr> IrcMessageHandler::parseNoticeMessage(
    Communi::IrcNoticeMessage *message)
{
    if (message->content().startsWith("Login auth", Qt::CaseInsensitive))
    {
        return {MessageBuilder(systemMessage,
                               "Login expired! Try logging in again.")
                    .release()};
    }
    else
    {
        std::vector<MessagePtr> builtMessages;

        builtMessages.emplace_back(makeSystemMessage(message->content()));

        return builtMessages;
    }
}  // namespace chatterino

void IrcMessageHandler::handleNoticeMessage(Communi::IrcNoticeMessage *message)
{
    auto app = getApp();
    auto builtMessages = this->parseNoticeMessage(message);

    for (auto msg : builtMessages)
    {
        QString channelName;
        if (!trimChannelName(message->target(), channelName) ||
            channelName == "jtv")
        {
            // Notice wasn't targeted at a single channel, send to all twitch
            // channels
            app->twitch.server->forEachChannelAndSpecialChannels(
                [msg](const auto &c) {
                    c->addMessage(msg);  //
                });

            return;
        }

        auto channel = app->twitch.server->getChannelOrEmpty(channelName);

        if (channel->isEmpty())
        {
            log("[IrcManager:handleNoticeMessage] Channel {} not found in "
                "channel "
                "manager ",
                channelName);
            return;
        }

        QString tags = message->tags().value("msg-id", "").toString();
        if (tags == "bad_delete_message_error" || tags == "usage_delete")
        {
            channel->addMessage(makeSystemMessage(
                "Usage: \"/delete <msg-id>\" - can't take more "
                "than one argument"));
        }
        else
        {
            channel->addMessage(msg);
        }
    }
}

void IrcMessageHandler::handleJoinMessage(Communi::IrcMessage *message)
{
    auto app = getApp();
    auto channel = app->twitch.server->getChannelOrEmpty(
        message->parameter(0).remove(0, 1));

    if (TwitchChannel *twitchChannel =
            dynamic_cast<TwitchChannel *>(channel.get()))
    {
        if (message->nick() !=
                getApp()->accounts->twitch.getCurrent()->getUserName() &&
            getSettings()->showJoins.getValue())
        {
            twitchChannel->addJoinedUser(message->nick());
        }
    }
}

void IrcMessageHandler::handlePartMessage(Communi::IrcMessage *message)
{
    auto app = getApp();
    auto channel = app->twitch.server->getChannelOrEmpty(
        message->parameter(0).remove(0, 1));

    if (TwitchChannel *twitchChannel =
            dynamic_cast<TwitchChannel *>(channel.get()))
    {
        if (message->nick() !=
                getApp()->accounts->twitch.getCurrent()->getUserName() &&
            getSettings()->showJoins.getValue())
        {
            twitchChannel->addPartedUser(message->nick());
        }
    }
}

}  // namespace chatterino
