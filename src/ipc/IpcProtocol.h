#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace advsettings
{
class MoveCenterTabController;

// Transport-independent implementation of IPC protocol v1.
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
    };

    IpcProtocol( MoveCenterTabController& moveCenter, QString appVersion );

    // Handles one text frame and always produces exactly one reply.
    Result handleMessage( const QString& message );

    // Reply for a frame that could not be handled as text (e.g. binary).
    static QJsonObject errorReply( const QJsonValue& id,
                                   const QString& code,
                                   const QString& message );

    // Current full offset in wire units (meters, degrees).
    QJsonObject currentOffset() const;

    static QJsonObject changedEvent( const QJsonObject& offset );

private:
    Result handleRequest( const QJsonObject& request );
    QJsonObject handleWrite( const QJsonValue& id,
                             const QString& path,
                             const QJsonValue& value,
                             bool relative );
    void applyField( const QString& field, double value, bool relative );

    MoveCenterTabController& m_moveCenter;
    QString m_appVersion;
};

} // namespace advsettings
