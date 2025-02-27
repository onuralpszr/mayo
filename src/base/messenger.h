/****************************************************************************
** Copyright (c) 2021, Fougue Ltd. <http://www.fougue.pro>
** All rights reserved.
** See license at https://github.com/fougue/mayo/blob/master/LICENSE.txt
****************************************************************************/

#pragma once

#include <QtCore/QString>
#include <functional>

namespace Mayo {

class Messenger {
public:
    enum class MessageType {
        Trace,
        Info,
        Warning,
        Error
    };

    struct Message {
        MessageType type;
        QString text;
    };

    void emitTrace(const QString& text);
    void emitInfo(const QString& text);
    void emitWarning(const QString& text);
    void emitError(const QString& text);
    virtual void emitMessage(MessageType msgType, const QString& text) = 0;
};

// Provides facility to construct a Messenger object from a lambda
// This avoids to subclass Messenger
class MessengerByCallback : public Messenger {
public:
    MessengerByCallback(std::function<void(MessageType, QString)> fnCallback);
    void emitMessage(MessageType msgType, const QString& text) override;

private:
    std::function<void(MessageType, QString)> m_fnCallback;
};

class NullMessenger : public Messenger {
public:
    static Messenger* instance();
    void emitMessage(MessageType msgType, const QString& text) override;

private:
    NullMessenger() = default;
};

} // namespace Mayo
