#include "IpcProtocol.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringList>
#include <QtGlobal>
#include <cmath>
#include <utility>

#include "../tabcontrollers/MoveCenterTabController.h"

namespace advsettings
{
namespace
{
    constexpr auto k_offsetPath = "/space/offset";
    constexpr auto k_fieldPathPrefix = "/space/offset/";

    // Input limits (see partialOffset in docs/ipc/schemas/offset.schema.json).
    constexpr double k_maxMeters = 10000.0;
    constexpr double k_maxDegrees = 1000000.0;

    // Largest integer exactly representable in a double (2^53).
    constexpr double k_maxSafeInteger = 9007199254740992.0;

    const QStringList& offsetFields()
    {
        static const QStringList fields{ "x", "y", "z", "rotation" };
        return fields;
    }

    // Floats like 0.1f would otherwise appear as 0.10000000149011612.
    double roundForWire( double value )
    {
        return std::round( value * 1e6 ) / 1e6;
    }

    QJsonObject okReply( const QJsonValue& id, const QJsonValue& value )
    {
        QJsonObject reply;
        if ( !id.isUndefined() )
        {
            reply["id"] = id;
        }
        reply["ok"] = true;
        reply["value"] = value;
        return reply;
    }

    // Returns the field name for "/space/offset/<field>", or an empty string.
    QString fieldFromPath( const QString& path )
    {
        if ( !path.startsWith( k_fieldPathPrefix ) )
        {
            return {};
        }
        const auto field = path.mid( static_cast<int>(
            QString( k_fieldPathPrefix ).size() ) );
        return offsetFields().contains( field ) ? field : QString{};
    }

    // Empty string when valid, otherwise a human-readable reason.
    QString validateNumber( const QString& field, const QJsonValue& value )
    {
        if ( !value.isDouble() )
        {
            return QString( "%1 must be a number" ).arg( field );
        }
        const double v = value.toDouble();
        if ( !std::isfinite( v ) )
        {
            return QString( "%1 must be finite" ).arg( field );
        }
        const double limit
            = field == "rotation" ? k_maxDegrees : k_maxMeters;
        if ( std::abs( v ) > limit )
        {
            return QString( "%1 must be within +/-%2" ).arg( field ).arg(
                limit );
        }
        return {};
    }

    bool hasOnlyKeys( const QJsonObject& object, const QStringList& allowed )
    {
        for ( const auto& key : object.keys() )
        {
            if ( !allowed.contains( key ) )
            {
                return false;
            }
        }
        return true;
    }
} // namespace

IpcProtocol::IpcProtocol( MoveCenterTabController& moveCenter,
                          QString appVersion )
    : m_moveCenter( moveCenter ), m_appVersion( std::move( appVersion ) )
{
}

QJsonObject IpcProtocol::errorReply( const QJsonValue& id,
                                     const QString& code,
                                     const QString& message )
{
    QJsonObject reply;
    if ( !id.isUndefined() )
    {
        reply["id"] = id;
    }
    reply["ok"] = false;
    reply["error"] = QJsonObject{ { "code", code }, { "message", message } };
    return reply;
}

QJsonObject IpcProtocol::currentOffset() const
{
    return QJsonObject{
        { "x", roundForWire( static_cast<double>( m_moveCenter.offsetX() ) ) },
        { "y", roundForWire( static_cast<double>( m_moveCenter.offsetY() ) ) },
        { "z", roundForWire( static_cast<double>( m_moveCenter.offsetZ() ) ) },
        { "rotation",
          roundForWire( static_cast<double>( m_moveCenter.rotation() )
                        / 100.0 ) },
    };
}

QJsonObject IpcProtocol::changedEvent( const QJsonObject& offset )
{
    return QJsonObject{
        { "event", "changed" },
        { "path", k_offsetPath },
        { "value", offset },
    };
}

IpcProtocol::Result IpcProtocol::handleMessage( const QString& message )
{
    QJsonParseError parseError{};
    const auto document
        = QJsonDocument::fromJson( message.toUtf8(), &parseError );
    if ( parseError.error != QJsonParseError::NoError )
    {
        return { errorReply( QJsonValue::Undefined,
                             "bad_json",
                             parseError.errorString() ),
                 SubscriptionChange::None };
    }
    if ( !document.isObject() )
    {
        return { errorReply( QJsonValue::Undefined,
                             "bad_json",
                             "message must be a JSON object" ),
                 SubscriptionChange::None };
    }
    return handleRequest( document.object() );
}

IpcProtocol::Result IpcProtocol::handleRequest( const QJsonObject& request )
{
    QJsonValue id = QJsonValue::Undefined;
    if ( request.contains( "id" ) )
    {
        const auto rawId = request.value( "id" );
        const double numericId = rawId.toDouble();
        if ( !rawId.isDouble() || std::floor( numericId ) != numericId
             || std::abs( numericId ) > k_maxSafeInteger )
        {
            return { errorReply( QJsonValue::Undefined,
                                 "bad_value",
                                 "id must be an integer" ),
                     SubscriptionChange::None };
        }
        id = rawId;
    }

    const auto opValue = request.value( "op" );
    if ( !opValue.isString() )
    {
        return { errorReply( id, "bad_op", "op must be a string" ),
                 SubscriptionChange::None };
    }
    const auto op = opValue.toString();

    if ( op == "hello" )
    {
        if ( !hasOnlyKeys( request, { "id", "op" } ) )
        {
            return { errorReply( id, "bad_value", "unexpected field" ),
                     SubscriptionChange::None };
        }
        const QJsonObject value{
            { "name", "OVRAS" },
            { "protocol", k_protocolVersion },
            { "version", m_appVersion },
        };
        return { okReply( id, value ), SubscriptionChange::None };
    }

    const bool isWrite = op == "set" || op == "add";
    const bool isKnownOp = isWrite || op == "get" || op == "subscribe"
                           || op == "unsubscribe";
    if ( !isKnownOp )
    {
        return { errorReply( id, "bad_op", "unknown op: " + op ),
                 SubscriptionChange::None };
    }

    const auto pathValue = request.value( "path" );
    if ( !pathValue.isString() )
    {
        return { errorReply( id, "bad_path", "path must be a string" ),
                 SubscriptionChange::None };
    }
    const auto path = pathValue.toString();
    const bool isWholePath = path == k_offsetPath;
    const auto field = fieldFromPath( path );
    if ( !isWholePath && field.isEmpty() )
    {
        return { errorReply( id, "bad_path", "unknown path: " + path ),
                 SubscriptionChange::None };
    }

    const QStringList allowedKeys
        = isWrite ? QStringList{ "id", "op", "path", "value" }
                  : QStringList{ "id", "op", "path" };
    if ( !hasOnlyKeys( request, allowedKeys ) )
    {
        return { errorReply( id, "bad_value", "unexpected field" ),
                 SubscriptionChange::None };
    }

    if ( op == "get" )
    {
        const auto offset = currentOffset();
        return { okReply( id,
                          isWholePath ? QJsonValue( offset )
                                      : offset.value( field ) ),
                 SubscriptionChange::None };
    }

    if ( isWrite )
    {
        return { handleWrite( id, path, request.value( "value" ), op == "add" ),
                 SubscriptionChange::None };
    }

    // subscribe / unsubscribe only work on the whole offset.
    if ( !isWholePath )
    {
        return { errorReply( id,
                             "bad_path",
                             op + " is only supported on /space/offset" ),
                 SubscriptionChange::None };
    }
    if ( op == "subscribe" )
    {
        return { okReply( id, currentOffset() ),
                 SubscriptionChange::Subscribe };
    }
    return { okReply( id, QJsonValue::Null ),
             SubscriptionChange::Unsubscribe };
}

QJsonObject IpcProtocol::handleWrite( const QJsonValue& id,
                                      const QString& path,
                                      const QJsonValue& value,
                                      bool relative )
{
    // Collect and validate everything first so a bad request changes nothing.
    QList<QPair<QString, double>> updates;

    if ( path == k_offsetPath )
    {
        if ( !value.isObject() )
        {
            return errorReply(
                id, "bad_value", "value must be an object for " + path );
        }
        const auto object = value.toObject();
        if ( object.isEmpty() )
        {
            return errorReply(
                id, "bad_value", "value must contain at least one field" );
        }
        if ( !hasOnlyKeys( object, offsetFields() ) )
        {
            return errorReply( id,
                               "bad_value",
                               "value may only contain x, y, z, rotation" );
        }
        // Rotation first: rotating around the headset shifts x/z, and an
        // absolute x/z in the same request must win.
        for ( const auto& field : { QString( "rotation" ),
                                    QString( "x" ),
                                    QString( "y" ),
                                    QString( "z" ) } )
        {
            if ( !object.contains( field ) )
            {
                continue;
            }
            const auto error = validateNumber( field, object.value( field ) );
            if ( !error.isEmpty() )
            {
                return errorReply( id, "bad_value", error );
            }
            updates.append( { field, object.value( field ).toDouble() } );
        }
    }
    else
    {
        const auto field = fieldFromPath( path );
        const auto error = validateNumber( field, value );
        if ( !error.isEmpty() )
        {
            return errorReply( id, "bad_value", error );
        }
        updates.append( { field, value.toDouble() } );
    }

    for ( const auto& update : updates )
    {
        applyField( update.first, update.second, relative );
    }
    return okReply( id, currentOffset() );
}

void IpcProtocol::applyField( const QString& field,
                              double value,
                              bool relative )
{
    if ( field == "rotation" )
    {
        const int centidegrees = qRound( value * 100.0 );
        m_moveCenter.setRotation(
            relative ? m_moveCenter.rotation() + centidegrees : centidegrees );
        return;
    }

    const auto meters = static_cast<float>( value );
    if ( field == "x" )
    {
        relative ? m_moveCenter.modOffsetX( meters )
                 : m_moveCenter.setOffsetX( meters );
    }
    else if ( field == "y" )
    {
        relative ? m_moveCenter.modOffsetY( meters )
                 : m_moveCenter.setOffsetY( meters );
    }
    else if ( field == "z" )
    {
        relative ? m_moveCenter.modOffsetZ( meters )
                 : m_moveCenter.setOffsetZ( meters );
    }
}

} // namespace advsettings
