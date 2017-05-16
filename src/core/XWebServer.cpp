/*
    cam2web - streaming camera to web

    BSD 2-Clause License

    Copyright (c) 2017, cvsandbox, cvsandbox@gmail.com
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this
    list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
    FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
    DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
    OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "XWebServer.hpp"
#include "XManualResetEvent.hpp"

#include <map>
#include <list>
#include <mutex>

#include <mongoose.h>

using namespace std;

namespace Private
{
    /* ================================================================= */
    /* Web request implementation using Mangoose APIs                    */
    /* ================================================================= */
    class MangooseWebRequest : public IWebRequest
    {
        friend class XWebServerData;

    private:
        struct http_message* mMessage;

    private:
        MangooseWebRequest( struct http_message* message ) :
            mMessage( message )
        {
        }

    public:
        string Uri( ) const
        {
            return string( mMessage->uri.p, mMessage->uri.p + mMessage->uri.len );
        }
        string Method( ) const
        {
            return string( mMessage->method.p, mMessage->method.p + mMessage->method.len );
        }
        string Proto( ) const
        {
            return string( mMessage->proto.p, mMessage->proto.p + mMessage->proto.len );
        }
        string Query( ) const
        {
            return string( mMessage->query_string.p, mMessage->query_string.p + mMessage->query_string.len );
        }
        string Body( ) const
        {
            return string( mMessage->body.p, mMessage->body.p + mMessage->body.len );
        }
        string GetVariable( const string& name ) const
        {
            string         ret;
            char           value[200];
            struct mg_str* body = mMessage->query_string.len > 0 ? &mMessage->query_string : &mMessage->body;

            if ( body->len < sizeof( value ) )
            {
                mg_get_http_var( body, name.c_str( ), value, sizeof( value ) );
                ret = value;
            }
            else
            {
                char* temp = new char[body->len + 1];
                mg_get_http_var( body, name.c_str( ), temp, sizeof( body->len ) );
                ret = temp;
                delete [] temp;
            }

            return ret;
        }
        map<string, string> Headers( ) const
        {
            map<string, string> headers;

            for ( int i = 0; ( i < MG_MAX_HTTP_HEADERS ) && ( mMessage->header_names[i].len > 0 ); i++ )
            {
                headers.insert( pair<string, string>(
                        string( mMessage->header_names[i].p, mMessage->header_names[i].p + mMessage->header_names[i].len ),
                        string( mMessage->header_values[i].p, mMessage->header_values[i].p + mMessage->header_values[i].len )
                    ) );
            }

            return headers;
        }
    };

    /* ================================================================= */
    /* Web response implementation using Mangoose APIs                   */
    /* ================================================================= */
    class MangooseWebResponse : public IWebResponse
    {
        friend class XWebServerData;

    private:
        struct mg_connection* mConnection;
        IWebRequestHandler*   mHandler;

    private:
        MangooseWebResponse( struct mg_connection* connection, IWebRequestHandler* handler = nullptr ) :
            mConnection( connection ), mHandler( handler )
        {
        }

        // Set handler owning this response
        void SetHandler( IWebRequestHandler* handler )
        {
            mHandler = handler;
        }

    public:
        // Length of data, which is still enqueued for sending
        size_t ToSendDataLength( ) const 
        {
            return mConnection->send_mbuf.len;
        }

        // Send the specified buffer into response
        void Send( const uint8_t* buffer, size_t length )
        {
            mg_send( mConnection, buffer, length );
        }

        // Print formatted response
        void Printf( const char *fmt, ... )
        {
            va_list list;

            va_start( list, fmt );
            mg_vprintf( mConnection, fmt, list );
            va_end( list );
        }

        // Send the specified buffer as a chunk into response
        void SendChunk( const uint8_t* buffer, size_t length )
        {
            mg_send_http_chunk( mConnection, (const char*) buffer , length );
        }

        // Print formatted chunk into response
        void PrintfChunk( const char* fmt, ... )
        {
            char    mem[MG_VPRINTF_BUFFER_SIZE];
            char*   buf = mem;
            int     len;
            va_list list;

            va_start( list, fmt );
            len = mg_avprintf( &buf, sizeof( mem ), fmt, list );
            va_end( list );

            if ( len >= 0 )
            {
                mg_send_http_chunk( mConnection, buf, len );
            }

            if ( ( buf != mem ) && ( buf != nullptr ) )
            {
                free( buf );
            }
        }

        // Send the specified error code as response
        void SendError( int errorCode, const char* reason = nullptr )
        {
            mg_http_send_error( mConnection, errorCode, reason );
        }

        // Close connection associated with the response
        void CloseConnection( )
        {
            // with mongoose we can only signal connection should be closed
            mConnection->flags |= MG_F_CLOSE_IMMEDIATELY;
        }

        // Generate timer event for the connection associated with the response
        // after the specified number of milliseconds
        void SetTimer( uint32_t msec )
        {
            mConnection->user_data = mHandler;
            mg_set_timer( mConnection, mg_time( ) + (double) msec / 1000 );
        }
    };

    /* ================================================================= */
    /* Data associated with request handler                              */
    /* ================================================================= */
    class RequestHandlerData
    {
    public:
        shared_ptr<IWebRequestHandler>  Handler;
        UserGroup                       AllowedUserGroup;

    public:
        RequestHandlerData( ) :
            Handler( ), AllowedUserGroup( UserGroup::Anyone )
        { }

        RequestHandlerData( const shared_ptr<IWebRequestHandler>& handler, UserGroup allowedUserGroup ) :
            Handler( handler), AllowedUserGroup( allowedUserGroup )
        { }
    };

    /* ================================================================= */
    /* Private data/implementation of the web server                     */
    /* ================================================================= */
    class XWebServerData
    {
    public:
        recursive_mutex DataSync;
        string          DocumentRoot;
        string          AuthDomain;
        uint16_t        Port;

    private:
        struct mg_mgr             EventManager;
        struct mg_serve_http_opts ServerOptions;

        char*                     ActiveDocumentRoot;
        string                    ActiveAuthDomain;

        XManualResetEvent         NeedToStop;
        XManualResetEvent         IsStopped;
        recursive_mutex           StartSync;
        bool                      IsRunning;

        typedef map<string, RequestHandlerData> HandlersMap;
        typedef list<RequestHandlerData>        HandlersList;

        HandlersMap  FileHandlers;
        HandlersList FolderHandlers;

        HandlersMap  ActiveFileHandlers;
        HandlersList ActiveFolderHandlers;

        typedef map<string, pair<string, UserGroup>> UsersMap;

        UsersMap Users;

    public:
        XWebServerData( const string& documentRoot, uint16_t port ) :
            DataSync( ), DocumentRoot( documentRoot ), AuthDomain( "cam2web" ), Port( port ),
            EventManager( { 0 } ), ServerOptions( { 0 } ),
            ActiveDocumentRoot( nullptr ), ActiveAuthDomain( ),
            NeedToStop( ), IsStopped( ), StartSync( ), IsRunning( false )
        {
            ServerOptions.enable_directory_listing = "no";
        }

        ~XWebServerData( )
        {
            Stop( );
        }

        bool Start( );
        void Stop( );
        void Cleanup( );
        void AddHandler( const shared_ptr<IWebRequestHandler>& handler, UserGroup userGroup );
        void RemoveHandler( const shared_ptr<IWebRequestHandler>& handler );
        void ClearHandlers( );
        RequestHandlerData FindHandler( const string& uri );

        void AddUser( const string& name, const string& digestHa1, UserGroup group );
        void RemoveUser( const string& name );

        UserGroup CheckDigestAuth( struct http_message* msg );

        static void* pollHandler( void* param );
        static void eventHandler( struct mg_connection* connection, int event, void* param );
    };
}

/* ================================================================= */
/* Implemenetation of the IWebRequestHandler                         */
/* ================================================================= */

IWebRequestHandler::IWebRequestHandler( const string& uri, bool canHandleSubContent ) :
    mUri( uri ), mCanHandleSubContent( canHandleSubContent )
{
    // make sure all URIs start with /
    if ( mUri[0] != '/' )
    {
        mUri = '/' + mUri;
    }

    // make sure nothing finishes with /
    while ( ( mUri.length( ) > 1 ) && ( mUri.back( ) == '/' ) )
    {
        mUri.pop_back( );
    }
}

/* ================================================================= */
/* Implemenetation of the XEmbeddedContentHandler                    */
/* ================================================================= */

XEmbeddedContentHandler::XEmbeddedContentHandler( const string& uri, const XEmbeddedContent* content ) :
    IWebRequestHandler( uri, false ), mContent( content )
{
}

// Handle request providing given embedded content
void XEmbeddedContentHandler::HandleHttpRequest( const IWebRequest& /* request */, IWebResponse& response )
{
    response.Printf( "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %u\r\n"
                     "\r\n", mContent->Type, mContent->Length );
    response.Send( mContent->Body, mContent->Length );
}

/* ================================================================= */
/* Implemenetation of the XWebServer                                 */
/* ================================================================= */

XWebServer::XWebServer( const string& documentRoot, uint16_t port ) :
    mData( new Private::XWebServerData( documentRoot, port ) )
{

}

XWebServer::~XWebServer( )
{
    delete mData;
}

// Get/Set document root
string XWebServer::DocumentRoot( ) const
{
    return mData->DocumentRoot;
}
XWebServer& XWebServer::SetDocumentRoot( const string& documentRoot )
{
    lock_guard<recursive_mutex> lock( mData->DataSync );
    mData->DocumentRoot = documentRoot;
    return *this;
}

// Get/Set digest authentication domain
std::string XWebServer::AuthDomain( ) const
{
    return mData->AuthDomain;
}
XWebServer& XWebServer::SetAuthDomain( const std::string& authDomain )
{
    lock_guard<recursive_mutex> lock( mData->DataSync );
    mData->AuthDomain = authDomain;
    return *this;
}

// We really love Windows.h, indeed
#pragma push_macro( "SetPort" )
#undef SetPort

// Get/Set port
uint16_t XWebServer::Port( ) const
{
    return mData->Port;
}
XWebServer& XWebServer::SetPort( uint16_t port )
{
    lock_guard<recursive_mutex> lock( mData->DataSync );
    mData->Port = port;
    return *this;
}

#pragma pop_macro( "SetPort" )

// Start/Stop the Web server
bool XWebServer::Start( )
{
    return mData->Start( );
}
void XWebServer::Stop( )
{
    mData->Stop( );
}

// Add new web request handler
XWebServer& XWebServer::AddHandler( const shared_ptr<IWebRequestHandler>& handler, UserGroup allowedUserGroup )
{
    mData->AddHandler( handler, allowedUserGroup );
    return *this;
}

// Remove the specified handler
void XWebServer::RemoveHandler( const std::shared_ptr<IWebRequestHandler>& handler )
{
    mData->RemoveHandler( handler );
}

// Remove all handlers
void XWebServer::ClearHandlers( )
{
    mData->ClearHandlers( );
}

// Add/Remove user to access protected request handlers
XWebServer& XWebServer::AddUser( const string& name, const string& digestHa1, UserGroup group )
{
    mData->AddUser( name, digestHa1, group );
    return *this;
}
void XWebServer::RemoveUser( const string& name )
{
    mData->RemoveUser( name );
}

// Calculate HA1 as defined by Digest authentication algorithm, MD5(user:domain:pass).
string XWebServer::CalculateDigestAuthHa1( const string& user, const string& domain, const string& pass )
{
    char ha1[33];

    cs_md5( ha1, user.c_str( ), user.length( ), ":", 1,
                 domain.c_str( ), domain.length( ), ":", 1,
                 pass.c_str( ), pass.length( ), nullptr );

    return string( ha1 );
}

/* ================================================================= */
/* Private implemenetation of the XWebServer                         */
/* ================================================================= */

namespace Private
{

// Start instance of a Web server
bool XWebServerData::Start( )
{
    lock_guard<recursive_mutex> lock( StartSync );
    char strPort[16];

    {
        lock_guard<recursive_mutex> lock( DataSync );

        sprintf( strPort, "%u", Port );

        // set document root
        if ( !DocumentRoot.empty( ) )
        {
            ActiveDocumentRoot = new char[DocumentRoot.length( ) + 1];
            strcpy( ActiveDocumentRoot, DocumentRoot.c_str( ) );

            ServerOptions.document_root = ActiveDocumentRoot;
        }

        // get a copy of handlers, so we don't need to guard it while server is running
        ActiveFileHandlers   = FileHandlers;
        ActiveFolderHandlers = FolderHandlers;
        ActiveAuthDomain     = AuthDomain;
    }

    mg_mgr_init( &EventManager, this );

    NeedToStop.Reset( );
    IsStopped.Reset( );

    struct mg_connection* connection = mg_bind( &EventManager, strPort, eventHandler );

    if ( connection != nullptr )
    {
        mg_set_protocol_http_websocket( connection );

        if ( mg_start_thread( pollHandler, this ) != nullptr )
        {
            IsRunning = true;
        }
    }

    if ( !IsRunning )
    {
        Cleanup( );
    }

    return IsRunning;
}

// Stop running Web server
void XWebServerData::Stop( )
{
    lock_guard<recursive_mutex> lock( StartSync );

    if ( IsRunning )
    {
        NeedToStop.Signal( );
        IsStopped.Wait( );

        Cleanup( );

        IsRunning = false;
    }
}

// Clean-up resources
void XWebServerData::Cleanup( )
{
    mg_mgr_free( &EventManager );

    if ( ActiveDocumentRoot != nullptr )
    {
        delete[] ActiveDocumentRoot;
    }
}

// Add web server request handler
void XWebServerData::AddHandler( const shared_ptr<IWebRequestHandler>& handler, UserGroup allowedUserGroup )
{
    lock_guard<recursive_mutex> lock( DataSync );

    if ( handler->CanHandleSubContent( ) )
    {
        FolderHandlers.push_back( RequestHandlerData( handler, allowedUserGroup ) );
    }
    else
    {
        FileHandlers.insert( pair<string, RequestHandlerData>( handler->Uri( ), RequestHandlerData( handler, allowedUserGroup ) ) );
    }
}

// Remove the specified request handler
void XWebServerData::RemoveHandler( const shared_ptr<IWebRequestHandler>& handler )
{
    lock_guard<recursive_mutex> lock( DataSync );

    FileHandlers.erase( handler->Uri( ) );

    for ( HandlersList::iterator itHandler = FolderHandlers.begin( ); itHandler != FolderHandlers.end( ); itHandler++ )
    {
        if ( itHandler->Handler == handler )
        {
            FolderHandlers.erase( itHandler );
            break;
        }
    }
}

// Remove all handlers
void XWebServerData::ClearHandlers( )
{
    lock_guard<recursive_mutex> lock( DataSync );

    FileHandlers.clear( );
    FolderHandlers.clear( );
}

// Find request handler for the specified URI
RequestHandlerData XWebServerData::FindHandler( const string& uri )
{
    RequestHandlerData          handler;
    HandlersMap::const_iterator fileIt = ActiveFileHandlers.find( uri );

    if ( fileIt != ActiveFileHandlers.end( ) )
    {
        handler = fileIt->second;
    }
    else
    {
        for ( auto& folderHandlerData : ActiveFolderHandlers )
        {
            if ( uri.compare( 0, folderHandlerData.Handler->Uri( ).length( ), folderHandlerData.Handler->Uri( ) ) == 0 )
            {
                handler = folderHandlerData;
                break;
            }
        }
    }

    return handler;
}

// Add/Remove user to/from the list of user who can access protected request handlers
void XWebServerData::AddUser( const string& name, const string& digestHa1, UserGroup group )
{
    lock_guard<recursive_mutex> lock( DataSync );

    Users[name] = pair<string, UserGroup>( digestHa1, group );
}
void XWebServerData::RemoveUser( const string& name )
{
    lock_guard<recursive_mutex> lock( DataSync );

    Users.erase( name );
}

// Thread to poll web events
void* XWebServerData::pollHandler( void* param )
{
    XWebServerData* self = (XWebServerData*) param;

    while ( !self->NeedToStop.Wait( 0 ) )
    {
        mg_mgr_poll( &self->EventManager, 1000 );
    }

    self->IsStopped.Signal( );

    return nullptr;
}

static void http_send_digest_auth_request( struct mg_connection* c, const char *domain )
{
    mg_printf( c,
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Digest qop=\"auth\", "
        "realm=\"%s\", nonce=\"%lx\"\r\n"
        "Content-Length: 0\r\n\r\n",
        domain, (unsigned long) mg_time( ) );
}

static bool check_nonce( const char* nonce )
{
    unsigned long now = (unsigned long) mg_time( );
    unsigned long val = (unsigned long) strtoul( nonce, NULL, 16 );
    return ( now > val ) && ( now - val < 3600 );
}

// Check digest authentication and resolve group of the incoming user
UserGroup XWebServerData::CheckDigestAuth( struct http_message* msg )
{
    UserGroup       userGroup = UserGroup::Anyone;
    struct mg_str*  hdr;
    char            user[50], cnonce[33], response[40], uri[200], qop[20], nc[20], nonce[30];
    char            expected_response[33];

    /* parse "Authorization:" header */
    if ( ( msg != nullptr ) &&
         ( ( hdr = mg_get_http_header( msg, "Authorization" ) ) != nullptr ) &&
         ( mg_http_parse_header( hdr, "username", user, sizeof( user ) ) != 0 ) &&
         ( mg_http_parse_header( hdr, "cnonce", cnonce, sizeof( cnonce ) ) != 0 ) &&
         ( mg_http_parse_header( hdr, "response", response, sizeof( response ) ) != 0 ) &&
         ( mg_http_parse_header( hdr, "uri", uri, sizeof( uri ) ) != 0 ) &&
         ( mg_http_parse_header( hdr, "qop", qop, sizeof( qop ) ) != 0 ) &&
         ( mg_http_parse_header( hdr, "nc", nc, sizeof( nc ) ) != 0 ) &&
         ( mg_http_parse_header( hdr, "nonce", nonce, sizeof( nonce ) ) != 0 ) )
    {
        // got some authentication data to check
        if ( check_nonce( nonce ) )
        {
            lock_guard<recursive_mutex> lock( DataSync );

            // first find the user
            UsersMap::const_iterator itUser = Users.find( user );

            if ( itUser != Users.end( ) )
            {
                char ha2[33];

                // HA2 = MD5( method:digestURI )
                cs_md5( ha2, msg->method.p, msg->method.len, ":", 1,
                             msg->uri.p, msg->uri.len + ( msg->query_string.len ? msg->query_string.len + 1 : 0 ), NULL );
                
                // response = MD5( HA1:nonce:nonceCount:cnonce:qop:HA2 )
                cs_md5( expected_response, 
                        itUser->second.first.c_str( ), itUser->second.first.length( ), ":", 1, // HA1 of the user
                        nonce, strlen( nonce ), ":", 1,
                        nc, strlen( nc ), ":", 1,
                        cnonce, strlen( cnonce ), ":", 1,
                        qop, strlen( qop ), ":", 1,
                        ha2, 32, NULL );

                if ( strcmp( response, expected_response ) == 0 )
                {
                    userGroup = itUser->second.second;
                }
            }
        }
    }

    return userGroup;
}

// Mangoose web server event handler
void XWebServerData::eventHandler( struct mg_connection* connection, int event, void* param )
{
    XWebServerData* self = (XWebServerData*) connection->mgr->user_data;

    static bool isAuth = false;

    if ( event == MG_EV_HTTP_REQUEST )
    {
        struct http_message* message = static_cast<struct http_message*>( param );
        MangooseWebRequest   request( message );
        MangooseWebResponse  response( connection );
        string               uri = request.Uri( );
        UserGroup            authUserGroup = self->CheckDigestAuth( message );

        // make sure nothing finishes with / except the root
        while ( ( uri.back( ) == '/' ) && ( uri.length( ) != 1 ) )
        {
            uri.pop_back( );
        }

        // try finding handler for the URI
        RequestHandlerData handlerData = self->FindHandler( uri );

        if ( handlerData.Handler )
        {
            if ( static_cast<int>( authUserGroup ) < static_cast<int>( handlerData.AllowedUserGroup ) )
            {
                http_send_digest_auth_request( connection, self->ActiveAuthDomain.c_str( ) );
            }
            else
            {
                response.SetHandler( handlerData.Handler.get( ) );
                // handle request with the found handler
                handlerData.Handler->HandleHttpRequest( request, response );
            }
        }
        else if ( self->ActiveDocumentRoot )
        {
            // use static content
            mg_serve_http( connection, message, self->ServerOptions );
        }
        else
        {
            // send 404 error - not found
            response.SendError( 404 );
        }
    }
    else if ( event == MG_EV_TIMER )
    {
        if ( connection->user_data != nullptr )
        {
            IWebRequestHandler* handler = static_cast<IWebRequestHandler*>( connection->user_data );
            MangooseWebResponse response( connection, handler );

            connection->user_data = nullptr;

            handler->HandleTimer( response );
        }
    }
}

} // namespace Private
