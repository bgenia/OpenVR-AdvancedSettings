#include "IpcProtocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QStringList>
#include <cmath>
#include <utility>

#include "../tabcontrollers/MoveCenterTabController.h"

namespace advsettings
{
namespace
{
    constexpr auto k_offsetPath = "/space/offset";
    constexpr auto k_totalPath = "/space/total";
    constexpr auto k_stacksPath = "/space/stacks";

    // Input limits (see metersInput/degreesInput in
    // docs/ipc/schemas/offset.schema.json).
    constexpr double k_maxMeters = 10000.0;
    constexpr double k_maxDegrees = 1000000.0;

    // Largest integer exactly representable in a double (2^53).
    constexpr double k_maxSafeInteger = 9007199254740992.0;

    enum class PathKind
    {
        Unknown,
        Offset,
        OffsetField,
        Total,
        TotalField,
        Stacks,
    };

    struct ParsedPath
    {
        PathKind kind = PathKind::Unknown;
        QString field; // for *Field kinds
    };

    const QStringList& offsetFields()
    {
        static const QStringList fields{ "x", "y", "z", "rotation" };
        return fields;
    }

    ParsedPath parsePath( const QString& path )
    {
        if ( path == k_offsetPath )
        {
            return { PathKind::Offset, {} };
        }
        if ( path == k_totalPath )
        {
            return { PathKind::Total, {} };
        }
        if ( path == k_stacksPath )
        {
            return { PathKind::Stacks, {} };
        }
        for ( const auto& [prefix, kind] :
              { std::pair<QString, PathKind>{ QString( k_offsetPath ) + "/",
                                              PathKind::OffsetField },
                std::pair<QString, PathKind>{ QString( k_totalPath ) + "/",
                                              PathKind::TotalField } } )
        {
            if ( path.startsWith( prefix ) )
            {
                const auto field = path.mid( prefix.size() );
                if ( offsetFields().contains( field ) )
                {
                    return { kind, field };
                }
            }
        }
        return {};
    }

    bool isValidStackId( const QString& id )
    {
        static const QRegularExpression pattern(
            QStringLiteral( "^[A-Za-z0-9_.-]{1,64}$" ) );
        return pattern.match( id ).hasMatch();
    }

    // Floats like 0.1f would otherwise appear as 0.10000000149011612.
    double roundForWire( double value )
    {
        return std::round( value * 1e6 ) / 1e6;
    }

    QJsonObject offsetJson( float x, float y, float z, int centidegrees )
    {
        return QJsonObject{
            { "x", roundForWire( static_cast<double>( x ) ) },
            { "y", roundForWire( static_cast<double>( y ) ) },
            { "z", roundForWire( static_cast<double>( z ) ) },
            { "rotation",
              roundForWire( static_cast<double>( centidegrees ) / 100.0 ) },
        };
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

QJsonObject IpcProtocol::userOffset() const
{
    return offsetJson( m_moveCenter.offsetX(),
                       m_moveCenter.offsetY(),
                       m_moveCenter.offsetZ(),
                       m_moveCenter.rotation() );
}

QJsonObject IpcProtocol::totalOffset() const
{
    return offsetJson( m_moveCenter.totalOffsetX(),
                       m_moveCenter.totalOffsetY(),
                       m_moveCenter.totalOffsetZ(),
                       m_moveCenter.totalRotation() );
}

QJsonObject IpcProtocol::stackOffset( const QString& stackId ) const
{
    const auto& stacks = m_moveCenter.offsetStacks();
    const auto it = stacks.constFind( stackId );
    if ( it == stacks.cend() )
    {
        // A missing stack contributes nothing.
        return offsetJson( 0.0f, 0.0f, 0.0f, 0 );
    }
    return offsetJson( it->x, it->y, it->z, it->rotation );
}

QJsonObject IpcProtocol::stackList() const
{
    QJsonArray stacks;
    const auto& all = m_moveCenter.offsetStacks();
    for ( auto it = all.cbegin(); it != all.cend(); ++it )
    {
        stacks.append( QJsonObject{
            { "id", it.key() },
            { "persist", it->persist },
            { "offset", offsetJson( it->x, it->y, it->z, it->rotation ) },
        } );
    }
    return QJsonObject{ { "max", k_maxOffsetStacks }, { "stacks", stacks } };
}

QJsonValue IpcProtocol::snapshot( const QString& path ) const
{
    switch ( parsePath( path ).kind )
    {
    case PathKind::Offset:
        return userOffset();
    case PathKind::Total:
        return totalOffset();
    case PathKind::Stacks:
        return stackList();
    default:
        return QJsonValue::Undefined;
    }
}

QJsonObject IpcProtocol::changedEvent( const QString& path,
                                       const QJsonValue& value )
{
    return QJsonObject{
        { "event", "changed" },
        { "path", path },
        { "value", value },
    };
}

IpcProtocol::Result IpcProtocol::handleMessage( const QString& message,
                                                quintptr client )
{
    QJsonParseError parseError{};
    const auto document
        = QJsonDocument::fromJson( message.toUtf8(), &parseError );
    if ( parseError.error != QJsonParseError::NoError )
    {
        return { errorReply( QJsonValue::Undefined,
                             "bad_json",
                             parseError.errorString() ),
                 SubscriptionChange::None,
                 {} };
    }
    if ( !document.isObject() )
    {
        return { errorReply( QJsonValue::Undefined,
                             "bad_json",
                             "message must be a JSON object" ),
                 SubscriptionChange::None,
                 {} };
    }
    return handleRequest( document.object(), client );
}

IpcProtocol::Result IpcProtocol::handleRequest( const QJsonObject& request,
                                                quintptr client )
{
    const auto error = [&]( const QJsonValue& id,
                            const QString& code,
                            const QString& message ) {
        return Result{ errorReply( id, code, message ),
                       SubscriptionChange::None,
                       {} };
    };
    const auto ok = []( const QJsonValue& id, const QJsonValue& value ) {
        return Result{ okReply( id, value ), SubscriptionChange::None, {} };
    };

    QJsonValue id = QJsonValue::Undefined;
    if ( request.contains( "id" ) )
    {
        const auto rawId = request.value( "id" );
        const double numericId = rawId.toDouble();
        if ( !rawId.isDouble() || std::floor( numericId ) != numericId
             || std::abs( numericId ) > k_maxSafeInteger )
        {
            return error( QJsonValue::Undefined,
                          "bad_value",
                          "id must be an integer" );
        }
        id = rawId;
    }

    const auto opValue = request.value( "op" );
    if ( !opValue.isString() )
    {
        return error( id, "bad_op", "op must be a string" );
    }
    const auto op = opValue.toString();

    if ( op == "hello" )
    {
        if ( !hasOnlyKeys( request, { "id", "op" } ) )
        {
            return error( id, "bad_value", "unexpected field" );
        }
        return ok( id,
                   QJsonObject{
                       { "name", "OVRAS" },
                       { "protocol", k_protocolVersion },
                       { "version", m_appVersion },
                       { "features", QJsonArray{ "stacks" } },
                   } );
    }

    const bool isWrite = op == "set" || op == "add";
    const bool isSubscription = op == "subscribe" || op == "unsubscribe";
    if ( !isWrite && !isSubscription && op != "get" && op != "reset" )
    {
        return error( id, "bad_op", "unknown op: " + op );
    }

    const auto pathValue = request.value( "path" );
    if ( !pathValue.isString() )
    {
        return error( id, "bad_path", "path must be a string" );
    }
    const auto path = pathValue.toString();
    const auto parsed = parsePath( path );
    if ( parsed.kind == PathKind::Unknown )
    {
        return error( id, "bad_path", "unknown path: " + path );
    }
    const bool isOffsetPath = parsed.kind == PathKind::Offset
                              || parsed.kind == PathKind::OffsetField;

    // Which paths each op accepts.
    if ( ( isWrite && !isOffsetPath )
         || ( op == "reset" && parsed.kind != PathKind::Offset )
         || ( isSubscription && parsed.kind != PathKind::Offset
              && parsed.kind != PathKind::Total
              && parsed.kind != PathKind::Stacks ) )
    {
        return error( id, "bad_path", op + " is not supported on " + path );
    }

    // Which fields each op accepts.
    QStringList allowedKeys{ "id", "op", "path" };
    if ( isWrite )
    {
        allowedKeys << "value" << "stack" << "persist" << "pivot";
    }
    else if ( ( op == "get" && isOffsetPath ) || op == "reset" )
    {
        allowedKeys << "stack";
    }
    if ( !hasOnlyKeys( request, allowedKeys ) )
    {
        return error( id, "bad_value", "unexpected field for " + op );
    }

    QString stackId;
    if ( request.contains( "stack" ) )
    {
        const auto stackValue = request.value( "stack" );
        if ( !stackValue.isString() || !isValidStackId( stackValue.toString() ) )
        {
            return error( id,
                          "bad_value",
                          "stack must match ^[A-Za-z0-9_.-]{1,64}$" );
        }
        stackId = stackValue.toString();
    }

    if ( op == "get" )
    {
        switch ( parsed.kind )
        {
        case PathKind::Offset:
            return ok( id,
                       stackId.isEmpty() ? userOffset()
                                         : stackOffset( stackId ) );
        case PathKind::OffsetField:
            return ok( id,
                       ( stackId.isEmpty() ? userOffset()
                                           : stackOffset( stackId ) )
                           .value( parsed.field ) );
        case PathKind::Total:
            return ok( id, totalOffset() );
        case PathKind::TotalField:
            return ok( id, totalOffset().value( parsed.field ) );
        case PathKind::Stacks:
            return ok( id, stackList() );
        case PathKind::Unknown:
            break;
        }
        return error( id, "internal", "unhandled path" );
    }

    if ( isWrite )
    {
        return { handleWrite( id, request, parsed.field, op == "add", client ),
                 SubscriptionChange::None,
                 {} };
    }

    if ( op == "reset" )
    {
        if ( stackId.isEmpty() )
        {
            // Same as the user's Reset Offsets action: stacks are kept.
            m_moveCenter.resetOffsets( true );
        }
        else
        {
            m_moveCenter.removeOffsetStack( stackId );
        }
        return ok( id, QJsonValue::Null );
    }

    if ( op == "subscribe" )
    {
        return { okReply( id, snapshot( path ) ),
                 SubscriptionChange::Subscribe,
                 path };
    }
    return { okReply( id, QJsonValue::Null ),
             SubscriptionChange::Unsubscribe,
             path };
}

QJsonObject IpcProtocol::handleWrite( const QJsonValue& id,
                                      const QJsonObject& request,
                                      const QString& field,
                                      bool relative,
                                      quintptr client )
{
    const QString stackId = request.value( "stack" ).toString();
    const auto value = request.value( "value" );

    // Validate everything first so a bad request changes nothing.
    if ( request.contains( "persist" ) )
    {
        if ( !request.value( "persist" ).isBool() )
        {
            return errorReply( id, "bad_value", "persist must be a boolean" );
        }
        if ( stackId.isEmpty() )
        {
            return errorReply(
                id, "bad_value", "persist is only allowed with stack" );
        }
    }

    bool pivotHmd = !m_moveCenter.universeCenteredRotation();
    if ( request.contains( "pivot" ) )
    {
        const auto pivot = request.value( "pivot" ).toString();
        if ( pivot != "hmd" && pivot != "origin" )
        {
            return errorReply(
                id, "bad_value", "pivot must be \"hmd\" or \"origin\"" );
        }
        pivotHmd = pivot == "hmd";
    }

    QList<QPair<QString, double>> updates;
    if ( field.isEmpty() )
    {
        if ( !value.isObject() )
        {
            return errorReply( id,
                               "bad_value",
                               "value must be an object for /space/offset" );
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
        for ( const auto& name : { QString( "rotation" ),
                                   QString( "x" ),
                                   QString( "y" ),
                                   QString( "z" ) } )
        {
            if ( !object.contains( name ) )
            {
                continue;
            }
            const auto problem = validateNumber( name, object.value( name ) );
            if ( !problem.isEmpty() )
            {
                return errorReply( id, "bad_value", problem );
            }
            updates.append( { name, object.value( name ).toDouble() } );
        }
    }
    else
    {
        const auto problem = validateNumber( field, value );
        if ( !problem.isEmpty() )
        {
            return errorReply( id, "bad_value", problem );
        }
        updates.append( { field, value.toDouble() } );
    }

    // ---- user offset ----
    if ( stackId.isEmpty() )
    {
        for ( const auto& update : updates )
        {
            if ( update.first == "rotation" )
            {
                const int centidegrees = qRound( update.second * 100.0 );
                m_moveCenter.setRotationWithPivot(
                    relative ? m_moveCenter.rotation() + centidegrees
                             : centidegrees,
                    pivotHmd );
                continue;
            }
            // Axis locks apply to the user offset (mod/set respect them).
            const auto meters = static_cast<float>( update.second );
            if ( update.first == "x" )
            {
                relative ? m_moveCenter.modOffsetX( meters )
                         : m_moveCenter.setOffsetX( meters );
            }
            else if ( update.first == "y" )
            {
                relative ? m_moveCenter.modOffsetY( meters )
                         : m_moveCenter.setOffsetY( meters );
            }
            else
            {
                relative ? m_moveCenter.modOffsetZ( meters )
                         : m_moveCenter.setOffsetZ( meters );
            }
        }
        return okReply( id, userOffset() );
    }

    // ---- offset stack ----
    auto* stack = m_moveCenter.acquireOffsetStack( stackId, client );
    if ( stack == nullptr )
    {
        return errorReply(
            id,
            "stack_limit",
            QString( "at most %1 offset stacks" ).arg( k_maxOffsetStacks ) );
    }
    if ( request.contains( "persist" ) )
    {
        stack->persist = request.value( "persist" ).toBool();
    }
    for ( const auto& update : updates )
    {
        if ( update.first == "rotation" )
        {
            const int centidegrees = qRound( update.second * 100.0 );
            m_moveCenter.setOffsetStackRotation(
                stackId,
                relative ? stack->rotation + centidegrees : centidegrees,
                pivotHmd );
            continue;
        }
        // Axis locks do not apply to stacks.
        const auto meters = static_cast<float>( update.second );
        float& target = update.first == "x"   ? stack->x
                        : update.first == "y" ? stack->y
                                              : stack->z;
        target = relative ? target + meters : meters;
    }
    return okReply( id, stackOffset( stackId ) );
}

} // namespace advsettings
