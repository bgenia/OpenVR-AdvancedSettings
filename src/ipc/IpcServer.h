#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QtWebSockets/QWebSocketServer>

#include "IpcProtocol.h"

class QWebSocket;
class QWebSocketCorsAuthenticator;

namespace advsettings
{
class MoveCenterTabController;

// Localhost-only WebSocket transport for IpcProtocol.
// Contract: docs/ipc/asyncapi.yaml. Lives on the main (GUI) thread together
// with the tab controllers, so no locking is needed.
class IpcServer : public QObject
{
    Q_OBJECT

public:
    IpcServer( MoveCenterTabController& moveCenter,
               const QString& appVersion,
               QObject* parent = nullptr );
    ~IpcServer() override;

    // Returns false and sets lastError() if the port could not be bound.
    bool start( quint16 port );
    void stop();

    bool isListening() const;
    QString lastError() const;

signals:
    void statusChanged();

private slots:
    void onNewConnection();
    void onOriginAuthenticationRequired(
        QWebSocketCorsAuthenticator* authenticator );
    void onTextMessage( const QString& message );
    void onBinaryMessage( const QByteArray& message );
    void onDisconnected();
    void onPushTick();

private:
    void sendJson( QWebSocket* client, const QJsonObject& object );
    void removeSubscriber( QWebSocket* client, const QString& path );
    void forgetClient( QWebSocket* client );

    MoveCenterTabController& m_moveCenter;
    IpcProtocol m_protocol;
    QWebSocketServer m_server;
    QList<QWebSocket*> m_clients;
    // Subscriber -> (path -> last value sent for that path).
    QHash<QWebSocket*, QHash<QString, QJsonValue>> m_subscribers;
    QTimer m_pushTimer;
    QString m_lastError;
};

} // namespace advsettings
