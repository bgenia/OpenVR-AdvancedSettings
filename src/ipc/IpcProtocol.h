#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QtGlobal>

namespace advsettings
{
class MoveCenterTabController;
struct OffsetStack;

// Transport-independent implementation of IPC protocol v1 (spec 1.1).
// The wire contract lives in docs/ipc/asyncapi.yaml and docs/ipc/schemas;
// keep this file in sync with it.
class IpcProtocol
{
public:
    static constexpr int k_protocolVersion = 1;

    enum class SubscriptionChange
    {
        None,
        Subscribe,
        Unsubscribe,
    };

    struct Result
    {
        QJsonObject reply;
        SubscriptionChange subscription = SubscriptionChange::None;
        QString subscriptionPath;
    };

    IpcProtocol( MoveCenterTabController& moveCenter, QString appVersion );

    // Handles one text frame from `client` (an opaque id that owns the
    // non-persistent stacks it writes) and always produces exactly one reply.
    Result handleMessage( const QString& message, quintptr client );

    // Reply for a frame that could not be handled as text (e.g. binary).
    static QJsonObject errorReply( const QJsonValue& id,
                                   const QString& code,
                                   const QString& message );

    // Current value of a subscribable path (/space/offset, /space/total,
    // /space/stacks), or Undefined for anything else.
    QJsonValue snapshot( const QString& path ) const;

    static QJsonObject changedEvent( const QString& path,
                                     const QJsonValue& value );

private:
    Result handleRequest( const QJsonObject& request, quintptr client );
    QJsonObject handleWrite( const QJsonValue& id,
                             const QJsonObject& request,
                             const QString& field,
                             bool relative,
                             quintptr client );

    QJsonObject userOffset() const;
    QJsonObject totalOffset() const;
    QJsonObject stackOffset( const QString& stackId ) const;
    QJsonObject stackList() const;

    MoveCenterTabController& m_moveCenter;
    QString m_appVersion;
};

} // namespace advsettings
