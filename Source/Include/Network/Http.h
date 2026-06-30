#pragma once

#include <Network/Tcp.h>
#include <Network/Dns.h>
#include <Kernel/List.h>
#include <Kernel/SpinLock.h>

/*
 * ============================================================================
 * HTTP Constants
 * ============================================================================
 */

/* HTTP Methods */
#define HTTP_METHOD_GET     "GET"
#define HTTP_METHOD_POST    "POST"
#define HTTP_METHOD_PUT     "PUT"
#define HTTP_METHOD_DELETE  "DELETE"
#define HTTP_METHOD_HEAD    "HEAD"
#define HTTP_METHOD_OPTIONS "OPTIONS"
#define HTTP_METHOD_PATCH   "PATCH"

/* HTTP Versions */
#define HTTP_VERSION_11     "HTTP/1.1"
#define HTTP_VERSION_10     "HTTP/1.0"

/* Default ports */
#define HTTP_PORT           80
#define HTTPS_PORT          443

/* Limits */
#define HTTP_MAX_URL        2048
#define HTTP_MAX_HEADERS    16384
#define HTTP_MAX_COOKIES    64
#define HTTP_MAX_REDIRECTS  10
#define HTTP_MAX_BODY       64 * 1024 * 1024  /* 64MB */

/* Status Codes */
#define HTTP_STATUS_CONTINUE                    100
#define HTTP_STATUS_SWITCHING_PROTOCOLS         101
#define HTTP_STATUS_PROCESSING                  102

#define HTTP_STATUS_OK                          200
#define HTTP_STATUS_CREATED                     201
#define HTTP_STATUS_ACCEPTED                    202
#define HTTP_STATUS_NON_AUTHORITATIVE_INFO      203
#define HTTP_STATUS_NO_CONTENT                  204
#define HTTP_STATUS_RESET_CONTENT               205
#define HTTP_STATUS_PARTIAL_CONTENT             206

#define HTTP_STATUS_MULTIPLE_CHOICES            300
#define HTTP_STATUS_MOVED_PERMANENTLY           301
#define HTTP_STATUS_FOUND                       302
#define HTTP_STATUS_SEE_OTHER                   303
#define HTTP_STATUS_NOT_MODIFIED                304
#define HTTP_STATUS_USE_PROXY                   305
#define HTTP_STATUS_TEMPORARY_REDIRECT          307
#define HTTP_STATUS_PERMANENT_REDIRECT          308

#define HTTP_STATUS_BAD_REQUEST                 400
#define HTTP_STATUS_UNAUTHORIZED                401
#define HTTP_STATUS_PAYMENT_REQUIRED            402
#define HTTP_STATUS_FORBIDDEN                   403
#define HTTP_STATUS_NOT_FOUND                   404
#define HTTP_STATUS_METHOD_NOT_ALLOWED          405
#define HTTP_STATUS_NOT_ACCEPTABLE              406
#define HTTP_STATUS_PROXY_AUTH_REQUIRED         407
#define HTTP_STATUS_REQUEST_TIMEOUT             408
#define HTTP_STATUS_CONFLICT                    409
#define HTTP_STATUS_GONE                        410
#define HTTP_STATUS_LENGTH_REQUIRED             411
#define HTTP_STATUS_PRECONDITION_FAILED         412
#define HTTP_STATUS_PAYLOAD_TOO_LARGE           413
#define HTTP_STATUS_URI_TOO_LONG                414
#define HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE      415
#define HTTP_STATUS_RANGE_NOT_SATISFIABLE       416
#define HTTP_STATUS_EXPECTATION_FAILED          417
#define HTTP_STATUS_IM_A_TEAPOT                 418
#define HTTP_STATUS_MISDIRECTED_REQUEST         421
#define HTTP_STATUS_UNPROCESSABLE_ENTITY        422
#define HTTP_STATUS_LOCKED                      423
#define HTTP_STATUS_FAILED_DEPENDENCY           424
#define HTTP_STATUS_TOO_EARLY                   425
#define HTTP_STATUS_UPGRADE_REQUIRED            426
#define HTTP_STATUS_PRECONDITION_REQUIRED       428
#define HTTP_STATUS_TOO_MANY_REQUESTS           429
#define HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE 431
#define HTTP_STATUS_UNAVAILABLE_FOR_LEGAL_REASONS 451

#define HTTP_STATUS_INTERNAL_SERVER_ERROR       500
#define HTTP_STATUS_NOT_IMPLEMENTED             501
#define HTTP_STATUS_BAD_GATEWAY                 502
#define HTTP_STATUS_SERVICE_UNAVAILABLE         503
#define HTTP_STATUS_GATEWAY_TIMEOUT             504
#define HTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED  505
#define HTTP_STATUS_VARIANT_ALSO_NEGOTIATES     506
#define HTTP_STATUS_INSUFFICIENT_STORAGE        507
#define HTTP_STATUS_LOOP_DETECTED               508
#define HTTP_STATUS_NOT_EXTENDED                510
#define HTTP_STATUS_NETWORK_AUTH_REQUIRED       511

/*
 * ============================================================================
 * HTTP Structures
 * ============================================================================
 */

/* Cookie */
typedef struct HttpCookie {
    CHAR Name[128];
    CHAR Value[256];
    CHAR Domain[256];
    CHAR Path[256];
    UINT64 Expires;
    BOOL Secure;
    BOOL HttpOnly;
    UINT8 SameSite;      /* 0=None, 1=Lax, 2=Strict */
    UINT8 Priority;      /* 0=Low, 1=Medium, 2=High */
    BOOL Partitioned;
    struct ListHead Node;
} HttpCookie;

/* HTTP Header */
typedef struct HttpHeader {
    CHAR Name[128];
    CHAR Value[512];
    struct ListHead Node;
} HttpHeader;

/* HTTP Request */
typedef struct HttpRequest {
    CHAR Method[16];
    CHAR Url[HTTP_MAX_URL];
    CHAR Host[256];
    CHAR Path[HTTP_MAX_URL];
    CHAR Query[512];
    CHAR Version[16];
    
    ListHead Headers;
    ListHead Cookies;
    
    UINT8 *Body;
    UINT32 BodyLen;
    UINT32 BodyCapacity;
    
    /* Options */
    BOOL FollowRedirects;
    UINT32 MaxRedirects;
    UINT32 TimeoutMs;
    BOOL KeepAlive;
    
    /* Callbacks */
    NOPTR (*OnHeader)(struct HttpRequest *Req, const CHAR *Name, const CHAR *Value);
    NOPTR (*OnData)(struct HttpRequest *Req, UINT8 *Data, UINT32 Len);
    NOPTR (*OnComplete)(struct HttpRequest *Req);
    NOPTR (*OnError)(struct HttpRequest *Req, INT Error);
} HttpRequest;

/* HTTP Response */
typedef struct HttpResponse {
    UINT16 StatusCode;
    CHAR StatusText[64];
    CHAR Version[16];
    
    ListHead Headers;
    ListHead Cookies;
    
    UINT8 *Body;
    UINT32 BodyLen;
    UINT32 BodyCapacity;
    
    BOOL Chunked;
    BOOL ConnectionClose;
    BOOL HeadersComplete;
    
    /* Convenience fields */
    UINT32 ContentLength;
    CHAR ContentType[128];
    CHAR Location[HTTP_MAX_URL];
    CHAR Server[128];
    UINT64 Date;
    UINT64 LastModified;
    
    /* Internal state for parsing */
    struct {
        BOOL StatusParsed;
        UINT32 HeadersParsed;
        UINT32 ChunkSize;
        BOOL ChunkRemaining;
        UINT32 BodyRead;
    } State;
} HttpResponse;

/* HTTP Client */
typedef struct HttpClient {
    TcpSocket *Sock;
    IpV4Addr RemoteAddr;
    UINT16 RemotePort;
    
    HttpRequest *Request;
    HttpResponse *Response;
    
    UINT8 *RecvBuffer;
    UINT32 RecvLen;
    UINT32 RecvCapacity;
    
    BOOL Connected;
    BOOL Running;
    BOOL Completed;
    BOOL Error;
    INT ErrorCode;
    
    /* Cookie jar */
    ListHead CookieJar;
    SpinLock CookieLock;
    
    /* User data */
    NOPTR *UserData;
} HttpClient;

/* HTTP Client Config */
typedef struct HttpClientConfig {
    UINT32 TimeoutMs;
    BOOL FollowRedirects;
    UINT32 MaxRedirects;
    BOOL KeepAlive;
    CHAR UserAgent[128];
    CHAR Accept[256];
    CHAR AcceptEncoding[128];
} HttpClientConfig;

/* Cookie attribute types */
typedef enum {
    COOKIE_ATTR_NONE = 0,
    COOKIE_ATTR_DOMAIN,
    COOKIE_ATTR_PATH,
    COOKIE_ATTR_EXPIRES,
    COOKIE_ATTR_MAX_AGE,
    COOKIE_ATTR_SECURE,
    COOKIE_ATTR_HTTP_ONLY,
    COOKIE_ATTR_SAME_SITE,
    COOKIE_ATTR_PRIORITY,
    COOKIE_ATTR_PARTITIONED
} CookieAttrType;

/* SameSite values */
typedef enum {
    SAME_SITE_NONE = 0,
    SAME_SITE_LAX,
    SAME_SITE_STRICT
} SameSiteValue;

/* Cookie priority */
typedef enum {
    COOKIE_PRIORITY_LOW = 0,
    COOKIE_PRIORITY_MEDIUM,
    COOKIE_PRIORITY_HIGH
} CookiePriority;

/* Cookie validation result */
typedef struct {
    BOOL Valid;
    const CHAR *Error;
} CookieValidationResult;

/*
 * ============================================================================
 * HTTP API
 * ============================================================================
 */

/* Initialization */
INT HttpInit(NOPTR);
NOPTR HttpShutdown(NOPTR);

/* Client management */
HttpClient* HttpClientCreate(NOPTR);
NOPTR HttpClientDestroy(HttpClient *Client);
INT HttpClientSetConfig(HttpClient *Client, const HttpClientConfig *Config);
INT HttpClientSetCookieJar(HttpClient *Client, ListHead *Jar);

/* Request building */
HttpRequest* HttpRequestCreate(const CHAR *Method, const CHAR *Url);
NOPTR HttpRequestDestroy(HttpRequest *Req);
INT HttpRequestSetHeader(HttpRequest *Req, const CHAR *Name, const CHAR *Value);
INT HttpRequestSetBody(HttpRequest *Req, const UINT8 *Data, UINT32 Len);
INT HttpRequestSetCookie(HttpRequest *Req, const CHAR *Name, const CHAR *Value);
INT HttpRequestClearCookies(HttpRequest *Req);

/* Synchronous requests */
INT HttpClientDo(HttpClient *Client, HttpRequest *Req, HttpResponse **Resp);
INT HttpGet(const CHAR *Url, HttpResponse **Resp);
INT HttpPost(const CHAR *Url, const UINT8 *Body, UINT32 BodyLen, HttpResponse **Resp);
INT HttpPut(const CHAR *Url, const UINT8 *Body, UINT32 BodyLen, HttpResponse **Resp);
INT HttpDelete(const CHAR *Url, HttpResponse **Resp);
INT HttpHead(const CHAR *Url, HttpResponse **Resp);

/* Asynchronous requests */
INT HttpClientDoAsync(HttpClient *Client, HttpRequest *Req,
                      NOPTR (*Callback)(HttpResponse *Resp, NOPTR *UserData),
                      NOPTR *UserData);
INT HttpGetAsync(const CHAR *Url, 
                 NOPTR (*Callback)(HttpResponse *Resp, NOPTR *UserData),
                 NOPTR *UserData);

/* Response handling */
INT HttpResponseParse(HttpResponse *Resp, const UINT8 *Data, UINT32 Len);
INT HttpResponseGetHeader(HttpResponse *Resp, const CHAR *Name, CHAR *Value, UINT32 ValueSize);
INT HttpResponseGetCookie(HttpResponse *Resp, const CHAR *Name, HttpCookie *Cookie);
NOPTR HttpResponseFree(HttpResponse *Resp);

/* Cookie management */
INT HttpCookieJarAdd(ListHead *Jar, HttpCookie *Cookie);
INT HttpCookieJarGet(ListHead *Jar, const CHAR *Domain, const CHAR *Path,
                      const CHAR *Name, HttpCookie *Out);
NOPTR HttpCookieJarClear(ListHead *Jar);
NOPTR HttpCookieJarApply(HttpRequest *Req, ListHead *Jar);

/* URL parsing */
INT HttpParseUrl(const CHAR *Url, CHAR *Scheme, UINT32 SchemeSize,
                 CHAR *Host, UINT32 HostSize, UINT16 *Port,
                 CHAR *Path, UINT32 PathSize, CHAR *Query, UINT32 QuerySize);

/* Utility */
const CHAR* HttpStatusText(UINT16 Code);
BOOL HttpIsRedirect(UINT16 Code);

/*
 * ============================================================================
 * HTTP Server (для будущего расширения)
 * ============================================================================
 */

typedef struct HttpServer HttpServer;
typedef struct HttpConnection HttpConnection;

typedef NOPTR (*HttpHandler)(HttpConnection *Conn, HttpRequest *Req, HttpResponse *Resp);
typedef NOPTR (*HttpCallback)(HttpResponse *Resp, NOPTR *UserData);

struct HttpConnection {
    TcpSocket *Sock;
    HttpServer *Server;
    UINT8 *RecvBuffer;
    UINT32 RecvLen;
    HttpRequest *Request;
    HttpResponse *Response;
    BOOL KeepAlive;
    ListHead Node;
};

struct HttpServer {
    TcpSocket *ListenSock;
    UINT16 Port;
    ListHead Connections;
    HttpHandler Handler;
    BOOL Running;
    SpinLock Lock;
};

INT HttpServerCreate(UINT16 Port, HttpHandler Handler, HttpServer **Out);
NOPTR HttpServerDestroy(HttpServer *Server);
NOPTR HttpServerRun(HttpServer *Server);
NOPTR HttpServerStop(HttpServer *Server);
INT HttpServerSendResponse(HttpConnection *Conn, HttpResponse *Resp);
