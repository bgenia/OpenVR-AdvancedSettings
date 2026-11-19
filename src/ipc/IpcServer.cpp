#include "IpcServer.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QtWebSockets/QWebSocket>
#include <QtWebSockets/QWebSocketCorsAuthenticator>
#include <easylogging++.h>

namespace advsettings
{
namespace
{
    // Push window for subscription events (~60 Hz).
    constexpr int k_pushIntervalMs = 16;
    // Requests are tiny; refuse anything unreasonably large.
    constexpr quint64 k_maxIncomingMessageBytes = 64 * 1024;
} // namespace

IpcServer::IpcServer( MoveCenterTabController& moveCenter,
                      const QString& appVersion,
                      QObject* parent )
    : QObject( parent ), m_protocol( moveCenter, appVersion ),
      m_server( "OVRAS IPC", QWebSocketServer::NonSecureMode )
{
    connect( &m_server,
             &QWebSocketServer::newConnection,
             this,
             &IpcServer::onNewConnection );
    connect( &m_server,
             &QWebSocketServer::originAuthenticationRequired,
             this,
             &IpcServer::onOriginAuthenticationRequired );

    // Space Drag only emits offset signals on release, so subscriptions
    // compare the live state on a timer instead of relying on signals.
    m_pushTimer.setInterval( k_pushIntervalMs );
    connect( &m_pushTimer, &QTimer::timeout, this, &IpcServer::onPushTick );
}

IpcServer::~IpcServer()
{
    stop();
}

bool IpcServer::start( quint16 port )
{
    stop();
    if ( !m_server.listen( QHostAddress::LocalHost, port ) )
    {
        m_lastError = QString( "Could not listen on 127.0.0.1:%1: %2" )
                          .arg( port )
                          .arg( m_server.errorString() );
        LOG( ERROR ) << "IPC: " << m_lastError.toStdString();
        emit statusChanged();
        return false;
    }
    m_lastError.clear();
    LOG( INFO ) << "IPC: listening on 127.0.0.1:" << port;
    emit statusChanged();
    return true;
}

void IpcServer::stop()
{
    m_pushTimer.stop();
    m_subscribers.clear();

    // Copy: disconnect handlers modify m_clients.
    const auto clients = m_clients;
    m_clients.clear();
    for ( auto* client : clients )
    {
        client->disconnect( this );
        client->close( QWebSocketProtocol::CloseCodeGoingAway,
                       "server stopping" );
        client->deleteLater();
    }

    const bool hadError = !m_lastError.isEmpty();
    m_lastError.clear();
    if ( m_server.isListening() )
    {
        m_server.close();
        LOG( INFO ) << "IPC: stopped";
        emit statusChanged();
    }
    else if ( hadError )
    {
        emit statusChanged();
    }
}

bool IpcServer::isListening() const
{
    return m_server.isListening();
}

QString IpcServer::lastError() const
{
    return m_lastError;
}

void IpcServer::onOriginAuthenticationRequired(
    QWebSocketCorsAuthenticator* authenticator )
{
    // Native clients don't send Origin; browsers always do. Refusing any
    // Origin stops arbitrary web pages from moving the user's playspace.
    if ( !authenticator->origin().isEmpty() )
    {
        LOG( WARNING ) << "IPC: refused connection with Origin '"
                       << authenticator->origin().toStdString() << "'";
        authenticator->setAllowed( false );
    }
}

void IpcServer::onNewConnection()
{
    while ( m_server.hasPendingConnections() )
    {
        auto* client = m_server.nextPendingConnection();
        if ( client == nullptr )
        {
            continue;
        }
        client->setMaxAllowedIncomingMessageSize( k_maxIncomingMessageBytes );
        connect( client,
                 &QWebSocket::textMessageReceived,
                 this,
                 &IpcServer::onTextMessage );
        connect( client,
                 &QWebSocket::binaryMessageReceived,
                 this,
                 &IpcServer::onBinaryMessage );
        connect( client,
                 &QWebSocket::disconnected,
                 this,
                 &IpcServer::onDisconnected );
        m_clients.append( client );
        LOG( INFO ) << "IPC: client connected from "
                    << client->peerAddress().toString().toStdString() << ":"
                    << client->peerPort();
    }
}

void IpcServer::onTextMessage( const QString& message )
{
    auto* client = qobject_cast<QWebSocket*>( sender() );
    if ( client == nullptr )
    {
        return;
    }

    const auto result = m_protocol.handleMessage( message );
    sendJson( client, result.reply );

    switch ( result.subscription )
    {
    case IpcProtocol::SubscriptionChange::Subscribe:
        // The reply already carried this snapshot.
        m_subscribers.insert( client, result.reply.value( "value" ).toObject() );
        if ( !m_pushTimer.isActive() )
        {
            m_pushTimer.start();
        }
        break;
    case IpcProtocol::SubscriptionChange::Unsubscribe:
        m_subscribers.remove( client );
        if ( m_subscribers.isEmpty() )
        {
            m_pushTimer.stop();
        }
        break;
    case IpcProtocol::SubscriptionChange::None:
        break;
    }
}

void IpcServer::onBinaryMessage( const QByteArray& /*message*/ )
{
    auto* client = qobject_cast<QWebSocket*>( sender() );
    if ( client == nullptr )
    {
        return;
    }
    sendJson( client,
              IpcProtocol::errorReply( QJsonValue::Undefined,
                                       "bad_json",
                                       "binary frames are not supported; "
                                       "send JSON as text frames" ) );
}

void IpcServer::onDisconnected()
{
    auto* client = qobject_cast<QWebSocket*>( sender() );
    if ( client == nullptr )
    {
        return;
    }
    m_clients.removeAll( client );
    m_subscribers.remove( client );
    if ( m_subscribers.isEmpty() )
    {
        m_pushTimer.stop();
    }
    LOG( INFO ) << "IPC: client disconnected";
    client->deleteLater();
}

void IpcServer::onPushTick()
{
    const auto current = m_protocol.currentOffset();
    for ( auto it = m_subscribers.begin(); it != m_subscribers.end(); ++it )
    {
        if ( it.value() != current )
        {
            sendJson( it.key(), IpcProtocol::changedEvent( current ) );
            it.value() = current;
        }
    }
}

void IpcServer::sendJson( QWebSocket* client, const QJsonObject& object )
{
    client->sendTextMessage( QString::fromUtf8(
        QJsonDocument( object ).toJson( QJsonDocument::Compact ) ) );
}

} // namespace advsettings
