#include <Network/Http.h>
#include <Network/IpV4.h>
#include <Memory/Allocator.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Time/Timer.h>
#include <Kernel/Return.h>
#include <Kernel/Scheduler.h>
#include <Console.h>
#include <Crypto/Rng.h>

/*
 * ============================================================================
 * Global state
 * ============================================================================
 */

static BOOL GHttpInitialized = FALSE;
static HttpClientConfig GDefaultConfig;

/*
 * ============================================================================
 * Utility functions
 * ============================================================================
 */

const CHAR* HttpStatusText(UINT16 Code) {
    switch (Code) {
        case HTTP_STATUS_OK:                    return "OK";
        case HTTP_STATUS_CREATED:               return "Created";
        case HTTP_STATUS_ACCEPTED:              return "Accepted";
        case HTTP_STATUS_NO_CONTENT:            return "No Content";
        case HTTP_STATUS_MOVED_PERMANENTLY:     return "Moved Permanently";
        case HTTP_STATUS_FOUND:                 return "Found";
        case HTTP_STATUS_SEE_OTHER:             return "See Other";
        case HTTP_STATUS_NOT_MODIFIED:          return "Not Modified";
        case HTTP_STATUS_TEMPORARY_REDIRECT:    return "Temporary Redirect";
        case HTTP_STATUS_BAD_REQUEST:           return "Bad Request";
        case HTTP_STATUS_UNAUTHORIZED:          return "Unauthorized";
        case HTTP_STATUS_FORBIDDEN:             return "Forbidden";
        case HTTP_STATUS_NOT_FOUND:             return "Not Found";
        case HTTP_STATUS_METHOD_NOT_ALLOWED:    return "Method Not Allowed";
        case HTTP_STATUS_REQUEST_TIMEOUT:       return "Request Timeout";
        case HTTP_STATUS_CONFLICT:              return "Conflict";
        case HTTP_STATUS_GONE:                  return "Gone";
        case HTTP_STATUS_PAYLOAD_TOO_LARGE:     return "Payload Too Large";
        case HTTP_STATUS_URI_TOO_LONG:          return "URI Too Long";
        case HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE: return "Unsupported Media Type";
        case HTTP_STATUS_IM_A_TEAPOT:           return "I'm a teapot";
        case HTTP_STATUS_INTERNAL_SERVER_ERROR: return "Internal Server Error";
        case HTTP_STATUS_NOT_IMPLEMENTED:       return "Not Implemented";
        case HTTP_STATUS_BAD_GATEWAY:           return "Bad Gateway";
        case HTTP_STATUS_SERVICE_UNAVAILABLE:   return "Service Unavailable";
        case HTTP_STATUS_GATEWAY_TIMEOUT:       return "Gateway Timeout";
        default:                                return "Unknown";
    }
}

BOOL HttpIsRedirect(UINT16 Code) {
    return (Code >= 300 && Code < 400 && Code != 304);
}

static UINT32 HttpParseChunkSize(const CHAR *Line) {
    UINT32 Size = 0;
    while (*Line) {
        CHAR C = *Line++;
        if (C >= '0' && C <= '9') {
            Size = Size * 16 + (C - '0');
        } else if (C >= 'a' && C <= 'f') {
            Size = Size * 16 + (C - 'a' + 10);
        } else if (C >= 'A' && C <= 'F') {
            Size = Size * 16 + (C - 'A' + 10);
        } else {
            break;
        }
    }
    return Size;
}

/*
 * ============================================================================
 * URL parsing
 * ============================================================================
 */

INT HttpParseUrl(const CHAR *Url, CHAR *Scheme, UINT32 SchemeSize,
                 CHAR *Host, UINT32 HostSize, UINT16 *Port,
                 CHAR *Path, UINT32 PathSize, CHAR *Query, UINT32 QuerySize) {
    const CHAR *Ptr = Url;
    const CHAR *HostStart;
    const CHAR *PathStart;
    const CHAR *QueryStart;
    const CHAR *PortStart;
    USIZE Len;
    
    if (!Url) RETURN(NO_OBJECT);
    
    /* Scheme: http:// or https:// */
    if (StrnCmp(Url, "http://", 7) == 0) {
        Ptr = Url + 7;
        if (Scheme) StrnCpy(Scheme, "http", SchemeSize - 1);
        if (Port) *Port = 80;
    } else if (StrnCmp(Url, "https://", 8) == 0) {
        Ptr = Url + 8;
        if (Scheme) StrnCpy(Scheme, "https", SchemeSize - 1);
        if (Port) *Port = 443;
    } else {
        Ptr = Url;
        if (Scheme) StrnCpy(Scheme, "http", SchemeSize - 1);
        if (Port) *Port = 80;
    }
    
    HostStart = Ptr;
    
    /* Find host end (port or path) */
    PortStart = NULLPTR;
    PathStart = NULLPTR;
    QueryStart = NULLPTR;
    
    while (*Ptr) {
        if (*Ptr == ':') {
            PortStart = Ptr + 1;
        } else if (*Ptr == '/') {
            PathStart = Ptr;
            break;
        } else if (*Ptr == '?' && !PathStart) {
            QueryStart = Ptr + 1;
            break;
        }
        Ptr++;
    }
    
    /* Host */
    Len = (PathStart ? (USIZE)(PathStart - HostStart) : 
           (QueryStart ? (USIZE)(QueryStart - HostStart - 1) : (USIZE)(Ptr - HostStart)));
    if (PortStart && PortStart <= HostStart + Len) {
        Len = (USIZE)(PortStart - HostStart - 1);
    }
    if (Host && Len < HostSize) {
        StrnCpy(Host, HostStart, Len);
    }
    
    /* Port */
    if (PortStart && Port) {
        *Port = (UINT16)AToI(PortStart);
    }
    
    /* Path */
    if (PathStart && Path) {
        Len = (QueryStart ? (USIZE)(QueryStart - PathStart - 1) : (USIZE)(Ptr - PathStart));
        if (Len < PathSize) {
            StrnCpy(Path, PathStart, Len);
        }
    } else if (Path) {
        StrCpy(Path, "/");
    }
    
    /* Query */
    if (QueryStart && Query) {
        Len = (USIZE)(Ptr - QueryStart);
        if (Len < QuerySize) {
            StrnCpy(Query, QueryStart, Len);
        }
    } else if (Query) {
        Query[0] = '\0';
    }
    
    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * Cookie management
 * ============================================================================
 */

INT HttpCookieJarAdd(ListHead *Jar, HttpCookie *Cookie) {
    ListHead *Pos;
    HttpCookie *C;
    
    if (!Jar || !Cookie) RETURN(NO_OBJECT);
    
    /* Check for existing cookie */
    ListForEach(Pos, Jar) {
        C = ListEntry(Pos, HttpCookie, Node);
        if (StrCmp(C->Name, Cookie->Name) == 0 &&
            StrCmp(C->Domain, Cookie->Domain) == 0 &&
            StrCmp(C->Path, Cookie->Path) == 0) {
            /* Replace existing - copy string properly */
            StrnCpy(C->Value, Cookie->Value, sizeof(C->Value) - 1);
            C->Value[sizeof(C->Value) - 1] = '\0';
            C->Expires = Cookie->Expires;
            C->Secure = Cookie->Secure;
            C->HttpOnly = Cookie->HttpOnly;
            C->SameSite = Cookie->SameSite;
            C->Priority = Cookie->Priority;
            C->Partitioned = Cookie->Partitioned;
            MemoryFree(Cookie);
            RETURN(SUCCESS);
        }
    }
    
    ListAddTail(Jar, &Cookie->Node);
    RETURN(SUCCESS);
}

INT HttpCookieJarGet(ListHead *Jar, const CHAR *Domain, const CHAR *Path,
                      const CHAR *Name, HttpCookie *Out) {
    ListHead *Pos;
    HttpCookie *C;
    UINT64 Now = TimerTicks() / TimerFreq() * 1000;
    
    if (!Jar || !Out) RETURN(NO_OBJECT);
    
    ListForEach(Pos, Jar) {
        C = ListEntry(Pos, HttpCookie, Node);
        
        /* Check expiry */
        if (C->Expires > 0 && Now >= C->Expires) {
            continue;
        }
        
        /* Check domain match */
        if (C->Domain[0]) {
            if (StrCmp(C->Domain, Domain) != 0 &&
                !StrStr(Domain, C->Domain)) {
                continue;
            }
        }
        
        /* Check path match */
        if (C->Path[0] && Path) {
            if (StrnCmp(Path, C->Path, StrLen(C->Path)) != 0) {
                continue;
            }
        }
        
        /* Check name match */
        if (Name) {
            if (StrCmp(C->Name, Name) != 0) continue;
            MemCpy(Out, C, sizeof(HttpCookie));
            RETURN(SUCCESS);
        }
    }
    
    RETURN(NOT_FOUND);
}

NOPTR HttpCookieJarClear(ListHead *Jar) {
    ListHead *Pos, *Tmp;
    HttpCookie *C;
    
    if (!Jar) return;
    
    ListForEachSafe(Pos, Tmp, Jar) {
        C = ListEntry(Pos, HttpCookie, Node);
        ListDel(&C->Node);
        MemoryFree(C);
    }
}

static const CHAR *GMonthNames[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const CHAR *GDayNames[] = {
    "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
};

static INT HttpParseMonth(const CHAR *Str) {
    for (INT I = 0; I < 12; I++) {
        if (StrnCmp(Str, GMonthNames[I], 3) == 0) {
            return I + 1;
        }
    }
    return -1;
}

static UINT64 HttpParseDate(const CHAR *Str) {
    UINT32 Day = 0, Month = 0, Year = 0;
    UINT32 Hour = 0, Min = 0, Sec = 0;
    UINT64 Timestamp = 0;
    UINT64 Days = 0;
    UINT32 I;
    
    if (!Str) return 0;
    
    /* Skip leading whitespace */
    while (*Str == ' ' || *Str == '\t') Str++;
    
    /* Try RFC 1123: "Sun, 06 Nov 1994 08:49:37 GMT" */
    /* Try RFC 850: "Sunday, 06-Nov-94 08:49:37 GMT" */
    /* Try ANSI C: "Sun Nov  6 08:49:37 1994" */
    
    /* Find year */
    const CHAR *YearPtr = StrStr(Str, "20");
    if (!YearPtr) YearPtr = StrStr(Str, "19");
    if (!YearPtr) {
        /* Try to find 2-digit year */
        for (I = 0; Str[I]; I++) {
            if (Str[I] == '-' && Str[I+1] >= '0' && Str[I+1] <= '9' &&
                Str[I+2] >= '0' && Str[I+2] <= '9') {
                Year = (Str[I+1] - '0') * 10 + (Str[I+2] - '0');
                Year += (Year >= 70) ? 1900 : 2000;
                break;
            }
        }
    } else {
        Year = (YearPtr[0] - '0') * 1000 + (YearPtr[1] - '0') * 100 +
               (YearPtr[2] - '0') * 10 + (YearPtr[3] - '0');
    }
    
    if (Year == 0) return 0;
    
    /* Find time HH:MM:SS */
    const CHAR *TimePtr = StrStr(Str, ":");
    if (TimePtr && TimePtr >= Str + 2) {
        const CHAR *H = TimePtr - 2;
        if (*H >= '0' && *H <= '2') {
            Hour = (*H - '0') * 10 + (*(H+1) - '0');
            Min = (*(TimePtr+1) - '0') * 10 + (*(TimePtr+2) - '0');
            if (TimePtr[4] && TimePtr[4] >= '0' && TimePtr[4] <= '9') {
                Sec = (*(TimePtr+4) - '0') * 10 + (*(TimePtr+5) - '0');
            }
        }
    }
    
    /* Find month */
    for (I = 0; I < 12; I++) {
        if (StrStr(Str, GMonthNames[I])) {
            Month = I + 1;
            break;
        }
    }
    if (Month == 0) return 0;
    
    /* Find day */
    for (I = 0; Str[I]; I++) {
        if (Str[I] >= '0' && Str[I] <= '9' &&
            (I == 0 || Str[I-1] == ' ' || Str[I-1] == ',' || Str[I-1] == '-')) {
            Day = 0;
            while (Str[I] >= '0' && Str[I] <= '9') {
                Day = Day * 10 + (Str[I] - '0');
                I++;
            }
            if (Day > 0 && Day <= 31) break;
        }
    }
    
    if (Day == 0) return 0;
    
    /* Convert to timestamp */
    for (UINT32 Y = 1970; Y < Year; Y++) {
        Days += 365 + ((Y % 4 == 0 && (Y % 100 != 0 || Y % 400 == 0)) ? 1 : 0);
    }
    for (UINT32 M = 1; M < Month; M++) {
        UINT8 Dims[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        Days += Dims[M-1];
        if (M == 2 && (Year % 4 == 0 && (Year % 100 != 0 || Year % 400 == 0))) {
            Days++;
        }
    }
    Days += (Day - 1);
    
    Timestamp = (Days * 86400 + Hour * 3600 + Min * 60 + Sec) * 1000;
    return Timestamp;
}

/* ============================================================================
 * Cookie validation (RFC 6265 Section 4.1.1)
 * ============================================================================
 */

static BOOL HttpCookieValidName(const CHAR *Name) {
    UINT32 I;
    
    if (!Name || Name[0] == '\0') return FALSE;
    
    /* Cookie name must not contain control characters or separators */
    for (I = 0; Name[I]; I++) {
        CHAR C = Name[I];
        if ((UINT8)C < 0x21 || (UINT8)C > 0x7E) return FALSE;
        if (C == ';' || C == ',' || C == ' ' || C == '\t') return FALSE;
        if (C == '=' || C == '(' || C == ')' || C == '<' || C == '>') return FALSE;
        if (C == '@' || C == ':' || C == '\\' || C == '"' || C == '/') return FALSE;
        if (C == '[' || C == ']' || C == '?' || C == '{' || C == '}') return FALSE;
    }
    
    return TRUE;
}

static BOOL HttpCookieValidValue(const CHAR *Value) {
    UINT32 I;
    
    if (!Value) return TRUE;  /* Empty value is allowed */
    
    /* Cookie value must not contain control characters */
    for (I = 0; Value[I]; I++) {
        CHAR C = Value[I];
        if ((UINT8)C < 0x21 || (UINT8)C > 0x7E) {
            /* Space and tab are allowed in quoted strings only */
            if (C != ' ' && C != '\t') return FALSE;
        }
        if (C == ';' || C == ',' || C == '\r' || C == '\n') return FALSE;
    }
    
    return TRUE;
}

static BOOL HttpCookieValidDomain(const CHAR *Domain, const CHAR *Host) {
    UINT32 I;
    
    if (!Domain || Domain[0] == '\0') return TRUE;  /* No domain specified */
    
    /* Domain must start with a dot for old-style cookies */
    /* RFC 6265: Domain must not start with a dot */
    if (Domain[0] == '.') return FALSE;
    
    /* Domain must not contain control characters */
    for (I = 0; Domain[I]; I++) {
        if ((UINT8)Domain[I] < 0x21 || (UINT8)Domain[I] > 0x7E) return FALSE;
    }
    
    /* Domain must have at least one dot */
    if (!StrChr(Domain, '.')) return FALSE;
    
    /* Domain should match host (simplified) */
    if (Host) {
        USIZE DomainLen = StrLen(Domain);
        USIZE HostLen = StrLen(Host);
        
        /* Domain must be a suffix of host */
        if (DomainLen > HostLen) return FALSE;
        
        const CHAR *HostSuffix = Host + HostLen - DomainLen;
        if (StrCmp(HostSuffix, Domain) != 0) return FALSE;
        
        /* Host must end with domain or have a dot before domain */
        if (HostLen > DomainLen && Host[HostLen - DomainLen - 1] != '.') {
            return FALSE;
        }
    }
    
    return TRUE;
}

static BOOL HttpCookieValidPath(const CHAR *Path, const CHAR *ReqPath) {
    if (!Path || Path[0] == '\0') return TRUE;  /* No path specified */
    
    /* Path must start with '/' */
    if (Path[0] != '/') return FALSE;
    
    /* Path must not contain control characters */
    for (UINT32 I = 0; Path[I]; I++) {
        if ((UINT8)Path[I] < 0x20 || (UINT8)Path[I] > 0x7E) return FALSE;
    }
    
    /* Path should match request path (simplified) */
    if (ReqPath) {
        USIZE PathLen = StrLen(Path);
        USIZE ReqPathLen = StrLen(ReqPath);
        
        if (PathLen > ReqPathLen) return FALSE;
        
        /* Request path must start with cookie path */
        if (StrnCmp(ReqPath, Path, PathLen) != 0) return FALSE;
        
        /* If cookie path is not a prefix match, check boundaries */
        if (PathLen < ReqPathLen && ReqPath[PathLen] != '/' && Path[PathLen-1] != '/') {
            return FALSE;
        }
    }
    
    return TRUE;
}

/* ============================================================================
 * Cookie attribute parsing
 * ============================================================================
 */

static CookieAttrType HttpParseCookieAttr(const CHAR *Attr, USIZE *AttrLen) {
    static const struct {
        const CHAR *Name;
        USIZE Len;
        CookieAttrType Type;
    } Attrs[] = {
        {"Domain", 6, COOKIE_ATTR_DOMAIN},
        {"Path", 4, COOKIE_ATTR_PATH},
        {"Expires", 7, COOKIE_ATTR_EXPIRES},
        {"Max-Age", 7, COOKIE_ATTR_MAX_AGE},
        {"Secure", 6, COOKIE_ATTR_SECURE},
        {"HttpOnly", 8, COOKIE_ATTR_HTTP_ONLY},
        {"SameSite", 8, COOKIE_ATTR_SAME_SITE},
        {"Priority", 8, COOKIE_ATTR_PRIORITY},
        {"Partitioned", 11, COOKIE_ATTR_PARTITIONED}
    };
    
    for (UINT32 I = 0; I < sizeof(Attrs) / sizeof(Attrs[0]); I++) {
        if (StrnCmp(Attr, Attrs[I].Name, Attrs[I].Len) == 0) {
            if (AttrLen) *AttrLen = Attrs[I].Len;
            return Attrs[I].Type;
        }
    }
    
    return COOKIE_ATTR_NONE;
}

static SameSiteValue HttpParseSameSite(const CHAR *Value) {
    if (!Value) return SAME_SITE_NONE;
    
    if (StrCaseCmp(Value, "Strict") == 0) return SAME_SITE_STRICT;
    if (StrCaseCmp(Value, "Lax") == 0) return SAME_SITE_LAX;
    if (StrCaseCmp(Value, "None") == 0) return SAME_SITE_NONE;
    
    return SAME_SITE_NONE;
}

static CookiePriority HttpParsePriority(const CHAR *Value) {
    if (!Value) return COOKIE_PRIORITY_MEDIUM;
    
    if (StrCaseCmp(Value, "Low") == 0) return COOKIE_PRIORITY_LOW;
    if (StrCaseCmp(Value, "High") == 0) return COOKIE_PRIORITY_HIGH;
    
    return COOKIE_PRIORITY_MEDIUM;
}

/* ============================================================================
 * Main Set-Cookie parser (RFC 6265 compliant)
 * ============================================================================
 */

static NOPTR HttpParseSetCookie(HttpResponse *Resp, const CHAR *Value) {
    HttpCookie *Cookie;
    CHAR *Ptr;
    CHAR *Save;
    CHAR *Token;
    CHAR CookieLine[512];
    CHAR Name[128];
    CHAR Val[256];
    UINT64 Now;
    BOOL FirstToken = TRUE;
    
    if (!Resp || !Value) return;
    
    Now = TimerTicks() / TimerFreq() * 1000;
    StrnCpy(CookieLine, Value, sizeof(CookieLine) - 1);
    
    Cookie = (HttpCookie*)MemoryAllocate(sizeof(HttpCookie));
    if (!Cookie) return;
    MemSet(Cookie, 0, sizeof(HttpCookie));
    Cookie->Path[0] = '\0';
    Cookie->Priority = 1;  /* Medium by default */
    
    /* Parse cookie-pair and attributes */
    Ptr = CookieLine;
    Save = NULLPTR;
    
    while ((Token = StrTokR(Ptr, ";", &Save))) {
        CHAR *EqualSign;
        CHAR *AttrName;
        CHAR *AttrValue = NULLPTR;
        USIZE AttrLen;
        CookieAttrType AttrType;
        
        /* Trim leading/trailing spaces */
        while (*Token == ' ' || *Token == '\t') Token++;
        {
            CHAR *End = Token + StrLen(Token) - 1;
            while (End > Token && (*End == ' ' || *End == '\t')) {
                *End = '\0';
                End--;
            }
        }
        
        if (FirstToken) {
            /* Cookie-pair: NAME=VALUE */
            EqualSign = StrChr(Token, '=');
            if (EqualSign) {
                *EqualSign = '\0';
                StrnCpy(Name, Token, sizeof(Name) - 1);
                StrnCpy(Val, EqualSign + 1, sizeof(Val) - 1);
                
                /* Trim value */
                while (Val[0] == ' ' || Val[0] == '\t') {
                    for (INT I = 0; Val[I]; I++) Val[I] = Val[I+1];
                }
                {
                    CHAR *End = Val + StrLen(Val) - 1;
                    while (End > Val && (*End == ' ' || *End == '\t')) {
                        *End = '\0';
                        End--;
                    }
                }
                
                /* Remove surrounding quotes */
                if (Val[0] == '"' && Val[StrLen(Val)-1] == '"') {
                    Val[StrLen(Val)-1] = '\0';
                    CHAR *Unquoted = Val + 1;
                    StrnCpy(Val, Unquoted, sizeof(Val) - 1);
                }
                
                StrnCpy(Cookie->Name, Name, sizeof(Cookie->Name) - 1);
                StrnCpy(Cookie->Value, Val, sizeof(Cookie->Value) - 1);
            } else {
                /* Cookie without value */
                StrnCpy(Cookie->Name, Token, sizeof(Cookie->Name) - 1);
                Cookie->Value[0] = '\0';
            }
            FirstToken = FALSE;
            Ptr = NULLPTR;
            continue;
        }
        
        /* Parse attribute */
        EqualSign = StrChr(Token, '=');
        if (EqualSign) {
            *EqualSign = '\0';
            AttrName = Token;
            AttrValue = EqualSign + 1;
            
            /* Trim attribute value */
            while (*AttrValue == ' ' || *AttrValue == '\t') AttrValue++;
            {
                CHAR *End = AttrValue + StrLen(AttrValue) - 1;
                while (End > AttrValue && (*End == ' ' || *End == '\t')) {
                    *End = '\0';
                    End--;
                }
            }
            
            /* Remove quotes from value */
            if (AttrValue[0] == '"' && AttrValue[StrLen(AttrValue)-1] == '"') {
                AttrValue[StrLen(AttrValue)-1] = '\0';
                AttrValue++;
            }
        } else {
            AttrName = Token;
            AttrValue = NULLPTR;
        }
        
        /* Trim attribute name */
        while (*AttrName == ' ' || *AttrName == '\t') AttrName++;
        {
            CHAR *End = AttrName + StrLen(AttrName) - 1;
            while (End > AttrName && (*End == ' ' || *End == '\t')) {
                *End = '\0';
                End--;
            }
        }
        
        AttrType = HttpParseCookieAttr(AttrName, &AttrLen);
        
        switch (AttrType) {
            case COOKIE_ATTR_DOMAIN:
                if (AttrValue) {
                    if (AttrValue[0] == '.') AttrValue++;
                    for (CHAR *P = AttrValue; *P; P++) {
                        if (*P >= 'A' && *P <= 'Z') *P += ('a' - 'A');
                    }
                    StrnCpy(Cookie->Domain, AttrValue, sizeof(Cookie->Domain) - 1);
                }
                break;
                
            case COOKIE_ATTR_PATH:
                if (AttrValue) {
                    if (AttrValue[0] != '/') {
                        CHAR Tmp[256];
                        SnPrintf(Tmp, sizeof(Tmp), "/%s", AttrValue);
                        StrnCpy(Cookie->Path, Tmp, sizeof(Cookie->Path) - 1);
                    } else {
                        StrnCpy(Cookie->Path, AttrValue, sizeof(Cookie->Path) - 1);
                    }
                }
                break;
                
            case COOKIE_ATTR_EXPIRES:
                if (AttrValue) {
                    Cookie->Expires = HttpParseDate(AttrValue);
                }
                break;
                
            case COOKIE_ATTR_MAX_AGE:
                if (AttrValue) {
                    INT MaxAge = AToI(AttrValue);
                    if (MaxAge >= 0) {
                        Cookie->Expires = Now + (UINT64)MaxAge * 1000;
                    } else {
                        Cookie->Expires = 1;
                    }
                }
                break;
                
            case COOKIE_ATTR_SECURE:
                Cookie->Secure = TRUE;
                break;
                
            case COOKIE_ATTR_HTTP_ONLY:
                Cookie->HttpOnly = TRUE;
                break;
                
            case COOKIE_ATTR_SAME_SITE:
                if (AttrValue) {
                    SameSiteValue SS = HttpParseSameSite(AttrValue);
                    Cookie->SameSite = (UINT8)SS;
                }
                break;
                
            case COOKIE_ATTR_PRIORITY:
                if (AttrValue) {
                    CookiePriority P = HttpParsePriority(AttrValue);
                    Cookie->Priority = (UINT8)P;
                }
                break;
                
            case COOKIE_ATTR_PARTITIONED:
                Cookie->Partitioned = TRUE;
                break;
                
            default:
                break;
        }
        
        Ptr = NULLPTR;
    }
    
    /* Set default path if not specified */
    if (Cookie->Path[0] == '\0') {
        StrCpy(Cookie->Path, "/");
    }
    
    /* Validate cookie */
    if (!HttpCookieValidName(Cookie->Name)) {
        MemoryFree(Cookie);
        return;
    }
    
    if (!HttpCookieValidValue(Cookie->Value)) {
        MemoryFree(Cookie);
        return;
    }
    
    ListAddTail(&Resp->Cookies, &Cookie->Node);
}

/* ============================================================================
 * Cookie string formatting (for Cookie header)
 * ============================================================================
 */

NOPTR HttpCookieJarApply(HttpRequest *Req, ListHead *Jar) {
    ListHead *Pos;
    HttpCookie *C;
    CHAR CookieStr[4096];
    CHAR *Ptr = CookieStr;
    UINT32 Remaining = sizeof(CookieStr);
    BOOL First = TRUE;
    UINT64 Now = TimerTicks() / TimerFreq() * 1000;
    CHAR Host[256];
    CHAR Path[256];
    
    if (!Req || !Jar) return;
    
    /* Get host and path from request */
    HttpParseUrl(Req->Url, NULLPTR, 0,
                 Host, sizeof(Host), NULLPTR,
                 Path, sizeof(Path), NULLPTR, 0);
    
    CookieStr[0] = '\0';
    
    ListForEach(Pos, Jar) {
        C = ListEntry(Pos, HttpCookie, Node);
        
        /* Check expiry */
        if (C->Expires > 0 && Now >= C->Expires) {
            continue;
        }
        
        /* Check domain match */
        if (C->Domain[0]) {
            USIZE DomainLen = StrLen(C->Domain);
            USIZE HostLen = StrLen(Host);
            
            if (DomainLen > HostLen) continue;
            const CHAR *HostSuffix = Host + HostLen - DomainLen;
            if (StrCmp(HostSuffix, C->Domain) != 0) continue;
            if (HostLen > DomainLen && Host[HostLen - DomainLen - 1] != '.') {
                continue;
            }
        }
        
        /* Check path match */
        if (C->Path[0]) {
            USIZE PathLen = StrLen(C->Path);
            USIZE ReqPathLen = StrLen(Path);
            
            if (PathLen > ReqPathLen) continue;
            if (StrnCmp(Path, C->Path, PathLen) != 0) continue;
            if (PathLen < ReqPathLen && Path[PathLen] != '/') continue;
        }
        
        /* Secure flag - only send over HTTPS */
        if (C->Secure) {
            /* TODO: Check if connection is HTTPS */
            continue;
        }
        
        /* SameSite check - skip SameSite cookies for now */
        if (C->SameSite != 0) {
            /* TODO: Implement SameSite checks */
        }
        
        if (!First) {
            *Ptr++ = ';';
            *Ptr++ = ' ';
            Remaining -= 2;
        }
        First = FALSE;
        
        SnPrintf(Ptr, Remaining, "%s=%s", C->Name, C->Value);
        Ptr += StrLen(Ptr);
        Remaining -= StrLen(Ptr);
    }
    
    if (!First) {
        HttpRequestSetHeader(Req, "Cookie", CookieStr);
    }
}

/*
 * ============================================================================
 * HTTP Request
 * ============================================================================
 */

HttpRequest* HttpRequestCreate(const CHAR *Method, const CHAR *Url) {
    HttpRequest *Req;
    CHAR Scheme[16];
    CHAR Host[256];
    UINT16 Port;
    CHAR Path[HTTP_MAX_URL];
    CHAR Query[512];
    
    Req = (HttpRequest*)MemoryAllocate(sizeof(HttpRequest));
    if (!Req) return NULLPTR;
    
    MemSet(Req, 0, sizeof(HttpRequest));
    ListInit(&Req->Headers);
    ListInit(&Req->Cookies);
    
    StrnCpy(Req->Method, Method ? Method : "GET", sizeof(Req->Method) - 1);
    StrnCpy(Req->Url, Url ? Url : "/", sizeof(Req->Url) - 1);
    StrCpy(Req->Version, HTTP_VERSION_11);
    
    /* Parse URL */
    if (HttpParseUrl(Url, Scheme, sizeof(Scheme),
                     Host, sizeof(Host), &Port,
                     Path, sizeof(Path), Query, sizeof(Query)) == SUCCESS) {
        StrCpy(Req->Host, Host);
        StrCpy(Req->Path, Path);
        if (Query[0]) {
            SnPrintf(Req->Query, sizeof(Req->Query), "?%s", Query);
        }
    }
    
    Req->FollowRedirects = TRUE;
    Req->MaxRedirects = HTTP_MAX_REDIRECTS;
    Req->TimeoutMs = 30000;
    Req->KeepAlive = FALSE;
    
    return Req;
}

NOPTR HttpRequestDestroy(HttpRequest *Req) {
    ListHead *Pos, *Tmp;
    HttpHeader *Hdr;
    
    if (!Req) return;
    
    ListForEachSafe(Pos, Tmp, &Req->Headers) {
        Hdr = ListEntry(Pos, HttpHeader, Node);
        ListDel(&Hdr->Node);
        MemoryFree(Hdr);
    }
    
    ListForEachSafe(Pos, Tmp, &Req->Cookies) {
        HttpCookie *C = ListEntry(Pos, HttpCookie, Node);
        ListDel(&C->Node);
        MemoryFree(C);
    }
    
    if (Req->Body) {
        MemoryFree(Req->Body);
    }
    
    MemoryFree(Req);
}

INT HttpRequestSetHeader(HttpRequest *Req, const CHAR *Name, const CHAR *Value) {
    HttpHeader *Hdr;
    ListHead *Pos;
    
    if (!Req || !Name || !Value) RETURN(NO_OBJECT);
    
    /* Check if header exists and update it */
    ListForEach(Pos, &Req->Headers) {
        Hdr = ListEntry(Pos, HttpHeader, Node);
        if (StrCaseCmp(Hdr->Name, Name) == 0) {
            StrnCpy(Hdr->Value, Value, sizeof(Hdr->Value) - 1);
            RETURN(SUCCESS);
        }
    }
    
    Hdr = (HttpHeader*)MemoryAllocate(sizeof(HttpHeader));
    if (!Hdr) RETURN(NO_MEMORY);
    
    StrnCpy(Hdr->Name, Name, sizeof(Hdr->Name) - 1);
    StrnCpy(Hdr->Value, Value, sizeof(Hdr->Value) - 1);
    ListAddTail(&Req->Headers, &Hdr->Node);
    
    RETURN(SUCCESS);
}

INT HttpRequestSetBody(HttpRequest *Req, const UINT8 *Data, UINT32 Len) {
    if (!Req) RETURN(NO_OBJECT);
    
    if (Req->Body) {
        MemoryFree(Req->Body);
        Req->Body = NULLPTR;
        Req->BodyLen = 0;
    }
    
    if (Data && Len > 0) {
        Req->Body = (UINT8*)MemoryAllocate(Len);
        if (!Req->Body) RETURN(NO_MEMORY);
        MemCpy(Req->Body, Data, Len);
        Req->BodyLen = Len;
    }
    
    RETURN(SUCCESS);
}

INT HttpRequestSetCookie(HttpRequest *Req, const CHAR *Name, const CHAR *Value) {
    HttpCookie *Cookie;
    ListHead *Pos;
    
    if (!Req || !Name) RETURN(NO_OBJECT);
    
    /* Update existing */
    ListForEach(Pos, &Req->Cookies) {
        Cookie = ListEntry(Pos, HttpCookie, Node);
        if (StrCmp(Cookie->Name, Name) == 0) {
            StrnCpy(Cookie->Value, Value ? Value : "", sizeof(Cookie->Value) - 1);
            RETURN(SUCCESS);
        }
    }
    
    Cookie = (HttpCookie*)MemoryAllocate(sizeof(HttpCookie));
    if (!Cookie) RETURN(NO_MEMORY);
    
    StrnCpy(Cookie->Name, Name, sizeof(Cookie->Name) - 1);
    StrnCpy(Cookie->Value, Value ? Value : "", sizeof(Cookie->Value) - 1);
    Cookie->Domain[0] = '\0';
    Cookie->Path[0] = '\0';
    Cookie->Expires = 0;
    Cookie->Secure = FALSE;
    Cookie->HttpOnly = FALSE;
    ListAddTail(&Req->Cookies, &Cookie->Node);
    
    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * HTTP Response
 * ============================================================================
 */

INT HttpResponseParse(HttpResponse *Resp, const UINT8 *Data, UINT32 Len) {
    const CHAR *Ptr = (const CHAR*)Data;
    const CHAR *End = (const CHAR*)Data + Len;
    const CHAR *LineStart = (const CHAR*)Data;
    BOOL StatusLineDone = FALSE;
    UINT32 I;
    
    if (!Resp || !Data) RETURN(NO_OBJECT);
    
    if (Len == 0) RETURN(SUCCESS);
    
    /* If response already has data, append */
    if (Resp->Body) {
        UINT32 NewLen = Resp->BodyLen + Len;
        if (NewLen > Resp->BodyCapacity) {
            Resp->BodyCapacity = NewLen + 4096;
            UINT8 *NewBody = (UINT8*)MemoryReallocate(Resp->Body, Resp->BodyCapacity);
            if (!NewBody) RETURN(NO_MEMORY);
            Resp->Body = NewBody;
        }
        MemCpy(Resp->Body + Resp->BodyLen, Data, Len);
        Resp->BodyLen = NewLen;
        RETURN(SUCCESS);
    }
    
    /* Parse headers line by line */
    while (Ptr < End) {
        const CHAR *LineEnd = Ptr;
        while (LineEnd < End && *LineEnd != '\r' && *LineEnd != '\n') {
            LineEnd++;
        }
        
        if (LineEnd == Ptr) {
            /* Empty line - end of headers */
            if (LineEnd + 1 < End && *(LineEnd + 1) == '\n') {
                Resp->HeadersComplete = TRUE;
                Ptr = LineEnd + 2;
                break;
            }
            Ptr++;
            continue;
        }
        
        /* Copy line */
        USIZE LineLen = (USIZE)(LineEnd - Ptr);
        CHAR Line[512];
        if (LineLen >= sizeof(Line)) LineLen = sizeof(Line) - 1;
        StrnCpy(Line, Ptr, LineLen);
        Line[LineLen] = '\0';
        
        if (!StatusLineDone) {
            /* Parse status line: HTTP/1.1 200 OK */
            UINT16 Code = 0;
            const CHAR *CodePtr = Line;
            
            /* Skip "HTTP/1.1 " */
            while (*CodePtr && *CodePtr != ' ') CodePtr++;
            if (*CodePtr) CodePtr++;
            
            /* Parse status code */
            while (*CodePtr >= '0' && *CodePtr <= '9') {
                Code = Code * 10 + (*CodePtr - '0');
                CodePtr++;
            }
            Resp->StatusCode = Code;
            
            /* Parse status text */
            while (*CodePtr == ' ') CodePtr++;
            StrnCpy(Resp->StatusText, CodePtr, sizeof(Resp->StatusText) - 1);
            
            StatusLineDone = TRUE;
        } else {
            /* Parse header: Name: Value */
            const CHAR *Colon = StrChr(Line, ':');
            if (Colon) {
                CHAR Name[128];
                USIZE NameLen = (USIZE)(Colon - Line);
                if (NameLen >= sizeof(Name)) NameLen = sizeof(Name) - 1;
                StrnCpy(Name, Line, NameLen);
                Name[NameLen] = '\0';
                
                const CHAR *Value = Colon + 1;
                while (*Value == ' ') Value++;
                
                /* Store common headers */
                if (StrCaseCmp(Name, "Content-Length") == 0) {
                    Resp->ContentLength = (UINT32)AToI(Value);
                } else if (StrCaseCmp(Name, "Content-Type") == 0) {
                    StrnCpy(Resp->ContentType, Value, sizeof(Resp->ContentType) - 1);
                } else if (StrCaseCmp(Name, "Location") == 0) {
                    StrnCpy(Resp->Location, Value, sizeof(Resp->Location) - 1);
                } else if (StrCaseCmp(Name, "Server") == 0) {
                    StrnCpy(Resp->Server, Value, sizeof(Resp->Server) - 1);
                } else if (StrCaseCmp(Name, "Transfer-Encoding") == 0 &&
                           StrStr(Value, "chunked") != NULLPTR) {
                    Resp->Chunked = TRUE;
                } else if (StrCaseCmp(Name, "Connection") == 0 &&
                           StrStr(Value, "close") != NULLPTR) {
                    Resp->ConnectionClose = TRUE;
                } else if (StrCaseCmp(Name, "Set-Cookie") == 0) {
                    /* TODO: Parse cookie */
                }
            }
        }
        
        Ptr = LineEnd + 1;
        if (Ptr < End && *Ptr == '\n') Ptr++;
    }
    
    /* If headers complete, store remaining as body */
    if (Resp->HeadersComplete && Ptr < End) {
        UINT32 BodyLen = (UINT32)(End - Ptr);
        Resp->Body = (UINT8*)MemoryAllocate(BodyLen + 1);
        if (Resp->Body) {
            MemCpy(Resp->Body, Ptr, BodyLen);
            Resp->Body[BodyLen] = '\0';
            Resp->BodyLen = BodyLen;
            Resp->BodyCapacity = BodyLen + 1;
        }
    }
    
    RETURN(SUCCESS);
}

INT HttpResponseGetHeader(HttpResponse *Resp, const CHAR *Name, CHAR *Value, UINT32 ValueSize) {
    ListHead *Pos;
    HttpHeader *Hdr;
    
    if (!Resp || !Name || !Value || ValueSize == 0) {
        RETURN(NO_OBJECT);
    }
    
    /* Search through headers */
    ListForEach(Pos, &Resp->Headers) {
        Hdr = ListEntry(Pos, HttpHeader, Node);
        if (StrCaseCmp(Hdr->Name, Name) == 0) {
            StrnCpy(Value, Hdr->Value, ValueSize - 1);
            Value[ValueSize - 1] = '\0';
            RETURN(SUCCESS);
        }
    }
    
    RETURN(NOT_FOUND);
}

INT HttpResponseGetCookie(HttpResponse *Resp, const CHAR *Name, HttpCookie *Cookie) {
    ListHead *Pos;
    HttpCookie *C;
    UINT64 Now = TimerTicks() / TimerFreq() * 1000;
    
    if (!Resp || !Name || !Cookie) {
        RETURN(NO_OBJECT);
    }
    
    ListForEach(Pos, &Resp->Cookies) {
        C = ListEntry(Pos, HttpCookie, Node);
        
        /* Check expiry */
        if (C->Expires > 0 && Now >= C->Expires) {
            continue;
        }
        
        if (StrCaseCmp(C->Name, Name) == 0) {
            MemCpy(Cookie, C, sizeof(HttpCookie));
            RETURN(SUCCESS);
        }
    }
    
    RETURN(NOT_FOUND);
}

NOPTR HttpResponseFree(HttpResponse *Resp) {
    ListHead *Pos, *Tmp;
    HttpHeader *Hdr;
    HttpCookie *Cookie;
    
    if (!Resp) return;
    
    /* Free headers */
    ListForEachSafe(Pos, Tmp, &Resp->Headers) {
        Hdr = ListEntry(Pos, HttpHeader, Node);
        ListDel(&Hdr->Node);
        MemoryFree(Hdr);
    }
    
    /* Free cookies */
    ListForEachSafe(Pos, Tmp, &Resp->Cookies) {
        Cookie = ListEntry(Pos, HttpCookie, Node);
        ListDel(&Cookie->Node);
        MemoryFree(Cookie);
    }
    
    /* Free body */
    if (Resp->Body) {
        MemoryFree(Resp->Body);
        Resp->Body = NULLPTR;
    }
    
    MemoryFree(Resp);
}

/*
 * ============================================================================
 * HTTP Client
 * ============================================================================
 */

HttpClient* HttpClientCreate(NOPTR) {
    HttpClient *Client = (HttpClient*)MemoryAllocate(sizeof(HttpClient));
    if (!Client) return NULLPTR;
    
    MemSet(Client, 0, sizeof(HttpClient));
    ListInit(&Client->CookieJar);
    SpinLockInit(&Client->CookieLock);
    Client->RecvCapacity = 64 * 1024;
    Client->RecvBuffer = (UINT8*)MemoryAllocate(Client->RecvCapacity);
    if (!Client->RecvBuffer) {
        MemoryFree(Client);
        return NULLPTR;
    }
    Client->Connected = FALSE;
    
    return Client;
}

NOPTR HttpClientDestroy(HttpClient *Client) {
    if (!Client) return;
    
    if (Client->Sock) {
        TcpClose(Client->Sock);
        TcpSocketDestroy(Client->Sock);
    }
    
    if (Client->RecvBuffer) {
        MemoryFree(Client->RecvBuffer);
    }
    
    if (Client->Response) {
        HttpResponseFree(Client->Response);
    }
    
    if (Client->Request) {
        HttpRequestDestroy(Client->Request);
    }
    
    HttpCookieJarClear(&Client->CookieJar);
    MemoryFree(Client);
}

INT HttpClientSetConfig(HttpClient *Client, const HttpClientConfig *Config) {
    if (!Client || !Config) RETURN(NO_OBJECT);
    
    if (Client->Request) {
        Client->Request->TimeoutMs = Config->TimeoutMs;
        Client->Request->FollowRedirects = Config->FollowRedirects;
        Client->Request->MaxRedirects = Config->MaxRedirects;
        Client->Request->KeepAlive = Config->KeepAlive;
    }
    
    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * HTTP Request building and sending
 * ============================================================================
 */

static INT HttpBuildRequest(HttpRequest *Req, UINT8 **OutBuf, UINT32 *OutLen) {
    CHAR *Buffer;
    UINT32 BufferSize = HTTP_MAX_HEADERS + Req->BodyLen + 1024;
    CHAR *Ptr;
    UINT32 Remaining;
    ListHead *Pos;
    HttpHeader *Hdr;
    INT Written;
    
    Buffer = (CHAR*)MemoryAllocate(BufferSize);
    if (!Buffer) RETURN(NO_MEMORY);
    Ptr = Buffer;
    Remaining = BufferSize;
    
    /* Request line */
    Written = SnPrintf(Ptr, Remaining, "%s %s%s %s\r\n",
                       Req->Method, Req->Path, Req->Query, Req->Version);
    if (Written < 0) goto error;
    Ptr += Written;
    Remaining -= Written;
    
    /* Host header */
    Written = SnPrintf(Ptr, Remaining, "Host: %s\r\n", Req->Host);
    if (Written < 0) goto error;
    Ptr += Written;
    Remaining -= Written;
    
    /* User-Agent */
    Written = SnPrintf(Ptr, Remaining, "User-Agent: TOS/0.04\r\n");
    if (Written < 0) goto error;
    Ptr += Written;
    Remaining -= Written;
    
    /* Accept */
    Written = SnPrintf(Ptr, Remaining, "Accept: */*\r\n");
    if (Written < 0) goto error;
    Ptr += Written;
    Remaining -= Written;
    
    /* Connection */
    Written = SnPrintf(Ptr, Remaining, "Connection: %s\r\n",
                       Req->KeepAlive ? "keep-alive" : "close");
    if (Written < 0) goto error;
    Ptr += Written;
    Remaining -= Written;
    
    /* Content-Length */
    if (Req->Body && Req->BodyLen > 0) {
        Written = SnPrintf(Ptr, Remaining, "Content-Length: %u\r\n", Req->BodyLen);
        if (Written < 0) goto error;
        Ptr += Written;
        Remaining -= Written;
    }
    
    /* Custom headers */
    ListForEach(Pos, &Req->Headers) {
        Hdr = ListEntry(Pos, HttpHeader, Node);
        Written = SnPrintf(Ptr, Remaining, "%s: %s\r\n", Hdr->Name, Hdr->Value);
        if (Written < 0) goto error;
        Ptr += Written;
        Remaining -= Written;
    }
    
    /* End of headers */
    Written = SnPrintf(Ptr, Remaining, "\r\n");
    if (Written < 0) goto error;
    Ptr += Written;
    Remaining -= Written;
    
    /* Body */
    if (Req->Body && Req->BodyLen > 0) {
        MemCpy(Ptr, Req->Body, Req->BodyLen);
        Ptr += Req->BodyLen;
    }
    
    *OutBuf = (UINT8*)Buffer;
    *OutLen = (UINT32)(Ptr - Buffer);
    RETURN(SUCCESS);
    
error:
    MemoryFree(Buffer);
    RETURN(NO_MEMORY);
}

static INT HttpSendRequest(HttpClient *Client, HttpRequest *Req) {
    UINT8 *Buffer;
    UINT32 BufferLen;
    INT Result;
    
    Result = HttpBuildRequest(Req, &Buffer, &BufferLen);
    if (Result != SUCCESS) RETURN(Result);
    
    Result = TcpSend(Client->Sock, Buffer, BufferLen);
    MemoryFree(Buffer);
    
    if (Result < 0) RETURN(IO_ERROR);
    
    return SUCCESS;
}

static INT HttpProcessChunkedResponse(HttpResponse *Resp, const UINT8 *Data, UINT32 Len) {
    const UINT8 *Ptr = Data;
    const UINT8 *End = Data + Len;
    UINT8 *Body = NULLPTR;
    UINT32 BodyLen = 0;
    UINT32 BodyCapacity = 0;
    UINT32 ChunkSize = 0;
    BOOL InChunk = FALSE;
    
    while (Ptr < End) {
        if (!InChunk) {
            /* Read chunk size (hex) */
            ChunkSize = 0;
            while (Ptr < End) {
                CHAR C = (CHAR)*Ptr;
                if (C >= '0' && C <= '9') {
                    ChunkSize = ChunkSize * 16 + (C - '0');
                } else if (C >= 'a' && C <= 'f') {
                    ChunkSize = ChunkSize * 16 + (C - 'a' + 10);
                } else if (C >= 'A' && C <= 'F') {
                    ChunkSize = ChunkSize * 16 + (C - 'A' + 10);
                } else if (C == '\r' || C == '\n') {
                    Ptr++;
                    if (Ptr < End && *Ptr == '\n') Ptr++;
                    break;
                } else {
                    break;
                }
                Ptr++;
            }
            
            if (ChunkSize == 0) {
                /* Last chunk, skip trailing \r\n */
                if (Ptr < End && *Ptr == '\r') {
                    Ptr++;
                    if (Ptr < End && *Ptr == '\n') Ptr++;
                }
                break;
            }
            
            InChunk = TRUE;
        }
        
        /* Read chunk data */
        UINT32 ReadLen = (UINT32)(End - Ptr);
        if (ReadLen > ChunkSize) {
            ReadLen = ChunkSize;
        }
        
        if (ReadLen > 0) {
            if (BodyLen + ReadLen > BodyCapacity) {
                BodyCapacity = BodyLen + ReadLen + 4096;
                UINT8 *NewBody = (UINT8*)MemoryReallocate(Body, BodyCapacity);
                if (!NewBody) {
                    if (Body) MemoryFree(Body);
                    RETURN(NO_MEMORY);
                }
                Body = NewBody;
            }
            MemCpy(Body + BodyLen, Ptr, ReadLen);
            BodyLen += ReadLen;
            Ptr += ReadLen;
            ChunkSize -= ReadLen;
        }
        
        if (ChunkSize == 0) {
            /* Skip trailing \r\n after chunk */
            if (Ptr < End && *Ptr == '\r') {
                Ptr++;
                if (Ptr < End && *Ptr == '\n') Ptr++;
            }
            InChunk = FALSE;
        }
    }
    
    /* Replace body with reassembled data */
    if (Resp->Body) {
        MemoryFree(Resp->Body);
    }
    Resp->Body = Body;
    Resp->BodyLen = BodyLen;
    Resp->BodyCapacity = BodyCapacity;
    
    RETURN(SUCCESS);
}

static INT HttpReceiveResponse(HttpClient *Client, HttpResponse **OutResp) {
    UINT8 RecvBuf[8192];
    INT RecvLen;
    UINT32 Timeout = Client->Request ? Client->Request->TimeoutMs : 30000;
    HttpResponse *Resp = NULLPTR;
    UINT32 TotalRecv = 0;
    BOOL HeadersComplete = FALSE;
    BOOL FirstPacket = TRUE;
    INT Result;
    
    Resp = (HttpResponse*)MemoryAllocate(sizeof(HttpResponse));
    if (!Resp) RETURN(NO_MEMORY);
    MemSet(Resp, 0, sizeof(HttpResponse));
    ListInit(&Resp->Headers);
    ListInit(&Resp->Cookies);
    
    /* Allocate body buffer */
    Resp->BodyCapacity = 64 * 1024;
    Resp->Body = (UINT8*)MemoryAllocate(Resp->BodyCapacity);
    if (!Resp->Body) {
        MemoryFree(Resp);
        RETURN(NO_MEMORY);
    }
    
    while (Timeout > 0) {
        RecvLen = TcpRecv(Client->Sock, RecvBuf, sizeof(RecvBuf));
        
        if (RecvLen > 0) {
            if (!HeadersComplete) {
                /* Parse headers */
                Result = HttpResponseParse(Resp, RecvBuf, (UINT32)RecvLen);
                if (Result != SUCCESS) {
                    HttpResponseFree(Resp);
                    RETURN(Result);
                }
                
                if (Resp->HeadersComplete) {
                    HeadersComplete = TRUE;
                    
                    /* If Content-Length is 0, we're done */
                    if (Resp->ContentLength == 0 && !Resp->Chunked) {
                        *OutResp = Resp;
                        RETURN(SUCCESS);
                    }
                }
            } else {
                /* Append body data */
                if (Resp->BodyLen + RecvLen > Resp->BodyCapacity) {
                    Resp->BodyCapacity = Resp->BodyLen + RecvLen + 4096;
                    UINT8 *NewBody = (UINT8*)MemoryReallocate(Resp->Body, Resp->BodyCapacity);
                    if (!NewBody) {
                        HttpResponseFree(Resp);
                        RETURN(NO_MEMORY);
                    }
                    Resp->Body = NewBody;
                }
                MemCpy(Resp->Body + Resp->BodyLen, RecvBuf, RecvLen);
                Resp->BodyLen += RecvLen;
                
                /* Check if we have all data */
                if (!Resp->Chunked && Resp->ContentLength > 0) {
                    if (Resp->BodyLen >= Resp->ContentLength) {
                        break;
                    }
                }
            }
            
            TotalRecv += RecvLen;
            
        } else if (RecvLen == 0) {
            /* Connection closed by server */
            break;
        } else if (RecvLen < 0) {
            if (RecvLen == -16) { /* -TIMEOUT */
                HttpResponseFree(Resp);
                RETURN(TIMEOUT);
            }
            HttpResponseFree(Resp);
            RETURN(IO_ERROR);
        }
        
        TimerSleep(10);
        Timeout -= 10;
    }
    
    /* Handle chunked encoding */
    if (Resp->Chunked) {
        /* We need to process chunked data from the body buffer */
        Result = HttpProcessChunkedResponse(Resp, Resp->Body, Resp->BodyLen);
        if (Result != SUCCESS) {
            HttpResponseFree(Resp);
            RETURN(Result);
        }
    }
    
    /* Trim body to actual size */
    if (Resp->BodyLen < Resp->BodyCapacity) {
        UINT8 *NewBody = (UINT8*)MemoryReallocate(Resp->Body, Resp->BodyLen + 1);
        if (NewBody) {
            Resp->Body = NewBody;
            Resp->BodyCapacity = Resp->BodyLen + 1;
        }
    }
    
    /* Null-terminate for convenience */
    if (Resp->Body && Resp->BodyCapacity > Resp->BodyLen) {
        Resp->Body[Resp->BodyLen] = '\0';
    }
    
    *OutResp = Resp;
    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * Synchronous HTTP requests
 * ============================================================================
 */

INT HttpClientDo(HttpClient *Client, HttpRequest *Req, HttpResponse **Resp) {
    IpV4Addr Ip;
    UINT16 Port;
    INT Result;
    UINT32 Redirects = 0;
    HttpRequest *CurrentReq = Req;
    HttpResponse *CurrentResp = NULLPTR;
    CHAR RedirectUrl[HTTP_MAX_URL];
    ListHead *Pos;
    HttpHeader *Hdr;
    
    if (!Client || !Req || !Resp) RETURN(NO_OBJECT);
    
    Client->Request = Req;
    
    /* Resolve DNS */
    if (HttpParseUrl(Req->Url, NULLPTR, 0,
                     NULLPTR, 0, &Port,
                     NULLPTR, 0, NULLPTR, 0) != SUCCESS) {
        Port = 80;
    }
    
    Result = DnsResolve(Req->Host, &Ip);
    if (Result != SUCCESS) {
        RETURN(NOT_FOUND);
    }
    
    /* Connect */
    Client->Sock = TcpSocketCreate();
    if (!Client->Sock) RETURN(NO_MEMORY);
    
    Result = TcpConnect(Client->Sock, Ip, Port);
    if (Result != SUCCESS) {
        TcpSocketDestroy(Client->Sock);
        Client->Sock = NULLPTR;
        RETURN(Result);
    }
    
    /* Wait for connection */
    TimerSleep(50);
    
    /* Send request */
    Result = HttpSendRequest(Client, CurrentReq);
    if (Result != SUCCESS) {
        TcpClose(Client->Sock);
        RETURN(Result);
    }
    
    /* Receive response */
    Result = HttpReceiveResponse(Client, &CurrentResp);
    if (Result != SUCCESS) {
        TcpClose(Client->Sock);
        RETURN(Result);
    }
    
    /* Handle redirects */
    if (Req->FollowRedirects && HttpIsRedirect(CurrentResp->StatusCode)) {
        while (HttpIsRedirect(CurrentResp->StatusCode) && Redirects < Req->MaxRedirects) {
            if (CurrentResp->Location[0] == '\0') {
                break;
            }
            
            /* Close old connection */
            TcpClose(Client->Sock);
            Client->Sock = NULLPTR;
            
            /* Parse redirect URL */
            StrCpy(RedirectUrl, CurrentResp->Location);
            HttpResponseFree(CurrentResp);
            CurrentResp = NULLPTR;
            
            /* Create new request */
            HttpRequest *NewReq = HttpRequestCreate(Req->Method, RedirectUrl);
            if (!NewReq) {
                RETURN(NO_MEMORY);
            }
            
            /* Copy custom headers (except Host, Connection, Content-Length) */
            ListForEach(Pos, &Req->Headers) {
                Hdr = ListEntry(Pos, HttpHeader, Node);
                if (StrCaseCmp(Hdr->Name, "Host") != 0 &&
                    StrCaseCmp(Hdr->Name, "Connection") != 0 &&
                    StrCaseCmp(Hdr->Name, "Content-Length") != 0) {
                    HttpRequestSetHeader(NewReq, Hdr->Name, Hdr->Value);
                }
            }
            
            /* Copy cookies */
            ListForEach(Pos, &Req->Cookies) {
                HttpCookie *Cookie = ListEntry(Pos, HttpCookie, Node);
                HttpRequestSetCookie(NewReq, Cookie->Name, Cookie->Value);
            }
            
            /* Copy body if method is POST or PUT */
            if (Req->Body && Req->BodyLen > 0) {
                if (StrCaseCmp(Req->Method, HTTP_METHOD_POST) == 0 ||
                    StrCaseCmp(Req->Method, HTTP_METHOD_PUT) == 0) {
                    HttpRequestSetBody(NewReq, Req->Body, Req->BodyLen);
                }
            }
            
            CurrentReq = NewReq;
            Redirects++;
            
            /* Reconnect and send */
            Result = DnsResolve(NewReq->Host, &Ip);
            if (Result != SUCCESS) {
                HttpRequestDestroy(NewReq);
                RETURN(NOT_FOUND);
            }
            
            Client->Sock = TcpSocketCreate();
            if (!Client->Sock) {
                HttpRequestDestroy(NewReq);
                RETURN(NO_MEMORY);
            }
            
            Result = TcpConnect(Client->Sock, Ip, Port);
            if (Result != SUCCESS) {
                TcpSocketDestroy(Client->Sock);
                Client->Sock = NULLPTR;
                HttpRequestDestroy(NewReq);
                RETURN(Result);
            }
            
            TimerSleep(50);
            
            Result = HttpSendRequest(Client, NewReq);
            if (Result != SUCCESS) {
                TcpClose(Client->Sock);
                HttpRequestDestroy(NewReq);
                RETURN(Result);
            }
            
            Result = HttpReceiveResponse(Client, &CurrentResp);
            if (Result != SUCCESS) {
                TcpClose(Client->Sock);
                HttpRequestDestroy(NewReq);
                RETURN(Result);
            }
            
            Client->Request = NewReq;
        }
    }
    
    /* Close connection if not keep-alive */
    if (!Req->KeepAlive || (CurrentResp && CurrentResp->ConnectionClose)) {
        if (Client->Sock) {
            TcpClose(Client->Sock);
            Client->Sock = NULLPTR;
        }
    }
    
    *Resp = CurrentResp;
    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * Convenience HTTP functions
 * ============================================================================
 */

INT HttpGet(const CHAR *Url, HttpResponse **Resp) {
    HttpClient *Client;
    HttpRequest *Req;
    INT Result;
    
    Client = HttpClientCreate();
    if (!Client) RETURN(NO_MEMORY);
    
    Req = HttpRequestCreate(HTTP_METHOD_GET, Url);
    if (!Req) {
        HttpClientDestroy(Client);
        RETURN(NO_MEMORY);
    }
    
    Result = HttpClientDo(Client, Req, Resp);
    
    if (Result != SUCCESS) {
        HttpRequestDestroy(Req);
        HttpClientDestroy(Client);
        RETURN(Result);
    }
    
    /* Client will be destroyed later, but keep request */
    Client->Request = NULLPTR;
    HttpClientDestroy(Client);
    
    RETURN(SUCCESS);
}

INT HttpPost(const CHAR *Url, const UINT8 *Body, UINT32 BodyLen, HttpResponse **Resp) {
    HttpClient *Client;
    HttpRequest *Req;
    INT Result;
    
    Client = HttpClientCreate();
    if (!Client) RETURN(NO_MEMORY);
    
    Req = HttpRequestCreate(HTTP_METHOD_POST, Url);
    if (!Req) {
        HttpClientDestroy(Client);
        RETURN(NO_MEMORY);
    }
    
    if (Body && BodyLen > 0) {
        HttpRequestSetBody(Req, Body, BodyLen);
        HttpRequestSetHeader(Req, "Content-Type", "application/octet-stream");
    }
    
    Result = HttpClientDo(Client, Req, Resp);
    
    if (Result != SUCCESS) {
        HttpRequestDestroy(Req);
        HttpClientDestroy(Client);
        RETURN(Result);
    }
    
    Client->Request = NULLPTR;
    HttpClientDestroy(Client);
    
    RETURN(SUCCESS);
}

INT HttpPut(const CHAR *Url, const UINT8 *Body, UINT32 BodyLen, HttpResponse **Resp) {
    HttpClient *Client;
    HttpRequest *Req;
    INT Result;
    
    Client = HttpClientCreate();
    if (!Client) RETURN(NO_MEMORY);
    
    Req = HttpRequestCreate(HTTP_METHOD_PUT, Url);
    if (!Req) {
        HttpClientDestroy(Client);
        RETURN(NO_MEMORY);
    }
    
    if (Body && BodyLen > 0) {
        HttpRequestSetBody(Req, Body, BodyLen);
        HttpRequestSetHeader(Req, "Content-Type", "application/octet-stream");
    }
    
    Result = HttpClientDo(Client, Req, Resp);
    
    if (Result != SUCCESS) {
        HttpRequestDestroy(Req);
        HttpClientDestroy(Client);
        RETURN(Result);
    }
    
    Client->Request = NULLPTR;
    HttpClientDestroy(Client);
    
    RETURN(SUCCESS);
}

INT HttpDelete(const CHAR *Url, HttpResponse **Resp) {
    HttpClient *Client;
    HttpRequest *Req;
    INT Result;
    
    Client = HttpClientCreate();
    if (!Client) RETURN(NO_MEMORY);
    
    Req = HttpRequestCreate(HTTP_METHOD_DELETE, Url);
    if (!Req) {
        HttpClientDestroy(Client);
        RETURN(NO_MEMORY);
    }
    
    Result = HttpClientDo(Client, Req, Resp);
    
    if (Result != SUCCESS) {
        HttpRequestDestroy(Req);
        HttpClientDestroy(Client);
        RETURN(Result);
    }
    
    Client->Request = NULLPTR;
    HttpClientDestroy(Client);
    
    RETURN(SUCCESS);
}

INT HttpHead(const CHAR *Url, HttpResponse **Resp) {
    HttpClient *Client;
    HttpRequest *Req;
    INT Result;
    
    Client = HttpClientCreate();
    if (!Client) RETURN(NO_MEMORY);
    
    Req = HttpRequestCreate(HTTP_METHOD_HEAD, Url);
    if (!Req) {
        HttpClientDestroy(Client);
        RETURN(NO_MEMORY);
    }
    
    Result = HttpClientDo(Client, Req, Resp);
    
    if (Result != SUCCESS) {
        HttpRequestDestroy(Req);
        HttpClientDestroy(Client);
        RETURN(Result);
    }
    
    Client->Request = NULLPTR;
    HttpClientDestroy(Client);
    
    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * Async HTTP requests
 * ============================================================================
 */

typedef struct AsyncHttpContext {
    HttpClient *Client;
    HttpRequest *Req;
    HttpCallback Callback;
    NOPTR *UserData;
    HttpResponse *Resp;
    INT Result;
    BOOL Done;
} AsyncHttpContext;

static NOPTR HttpAsyncWorker(NOPTR *Arg) {
    AsyncHttpContext *Ctx = (AsyncHttpContext*)Arg;
    
    Ctx->Result = HttpClientDo(Ctx->Client, Ctx->Req, &Ctx->Resp);
    Ctx->Done = TRUE;
    
    if (Ctx->Callback) {
        Ctx->Callback(Ctx->Resp, Ctx->UserData);
    }
    
    return;
}

INT HttpClientDoAsync(HttpClient *Client, HttpRequest *Req,
                      NOPTR (*Callback)(HttpResponse *Resp, NOPTR *UserData),
                      NOPTR *UserData) {
    AsyncHttpContext *Ctx;
    
    if (!Client || !Req) RETURN(NO_OBJECT);
    
    Ctx = (AsyncHttpContext*)MemoryAllocate(sizeof(AsyncHttpContext));
    if (!Ctx) RETURN(NO_MEMORY);
    
    MemSet(Ctx, 0, sizeof(AsyncHttpContext));
    Ctx->Client = Client;
    Ctx->Req = Req;
    Ctx->Callback = Callback;
    Ctx->UserData = UserData;
    
    Client->Request = Req;
    
    return SchedulerCreateTask("HttpWorker", HttpAsyncWorker, (NOPTR*)Ctx,
                                SCHED_PRIORITY_NORMAL, TASK_DEFAULT_QUANTUM);
}

INT HttpGetAsync(const CHAR *Url,
                 NOPTR (*Callback)(HttpResponse *Resp, NOPTR *UserData),
                 NOPTR *UserData) {
    HttpClient *Client;
    HttpRequest *Req;
    
    Client = HttpClientCreate();
    if (!Client) RETURN(NO_MEMORY);
    
    Req = HttpRequestCreate(HTTP_METHOD_GET, Url);
    if (!Req) {
        HttpClientDestroy(Client);
        RETURN(NO_MEMORY);
    }
    
    return HttpClientDoAsync(Client, Req, Callback, UserData);
}

/*
 * ============================================================================
 * HTTP Server
 * ============================================================================
 */

static NOPTR HttpServerAcceptThread(NOPTR *Arg) {
    HttpServer *Server = (HttpServer*)Arg;
    TcpSocket *ClientSock;
    HttpConnection *Conn;
    
    while (Server->Running) {
        ClientSock = TcpAccept(Server->ListenSock);
        if (!ClientSock) {
            TimerSleep(10);
            continue;
        }
        
        Conn = (HttpConnection*)MemoryAllocate(sizeof(HttpConnection));
        if (!Conn) {
            TcpClose(ClientSock);
            TcpSocketDestroy(ClientSock);
            continue;
        }
        
        MemSet(Conn, 0, sizeof(HttpConnection));
        Conn->Sock = ClientSock;
        Conn->Server = Server;
        Conn->KeepAlive = FALSE;
        Conn->RecvBuffer = (UINT8*)MemoryAllocate(16 * 1024);
        
        ListAddTail(&Server->Connections, &Conn->Node);
    }
    
    return;
}

INT HttpServerCreate(UINT16 Port, HttpHandler Handler, HttpServer **Out) {
    HttpServer *Server;
    IpV4Addr Any = { .Addr = 0 };
    
    if (!Out) RETURN(NO_OBJECT);
    
    Server = (HttpServer*)MemoryAllocate(sizeof(HttpServer));
    if (!Server) RETURN(NO_MEMORY);
    
    MemSet(Server, 0, sizeof(HttpServer));
    Server->Port = Port;
    Server->Handler = Handler;
    Server->Running = FALSE;
    ListInit(&Server->Connections);
    SpinLockInit(&Server->Lock);
    
    Server->ListenSock = TcpSocketCreate();
    if (!Server->ListenSock) {
        MemoryFree(Server);
        RETURN(NO_MEMORY);
    }
    
    if (TcpBind(Server->ListenSock, Any, Port) != SUCCESS) {
        TcpSocketDestroy(Server->ListenSock);
        MemoryFree(Server);
        RETURN(IO_ERROR);
    }
    
    if (TcpListen(Server->ListenSock, 10) != SUCCESS) {
        TcpSocketDestroy(Server->ListenSock);
        MemoryFree(Server);
        RETURN(IO_ERROR);
    }
    
    *Out = Server;
    RETURN(SUCCESS);
}

NOPTR HttpServerDestroy(HttpServer *Server) {
    ListHead *Pos, *Tmp;
    HttpConnection *Conn;
    
    if (!Server) return;
    
    Server->Running = FALSE;
    
    if (Server->ListenSock) {
        TcpClose(Server->ListenSock);
        TcpSocketDestroy(Server->ListenSock);
    }
    
    ListForEachSafe(Pos, Tmp, &Server->Connections) {
        Conn = ListEntry(Pos, HttpConnection, Node);
        ListDel(&Conn->Node);
        if (Conn->Sock) {
            TcpClose(Conn->Sock);
            TcpSocketDestroy(Conn->Sock);
        }
        if (Conn->RecvBuffer) MemoryFree(Conn->RecvBuffer);
        MemoryFree(Conn);
    }
    
    MemoryFree(Server);
}

NOPTR HttpServerRun(HttpServer *Server) {
    if (!Server) return;
    
    Server->Running = TRUE;
    SchedulerCreateTask("HttpServer", HttpServerAcceptThread, (NOPTR*)Server,
                        SCHED_PRIORITY_NORMAL, TASK_DEFAULT_QUANTUM);
}

NOPTR HttpServerStop(HttpServer *Server) {
    if (!Server) return;
    Server->Running = FALSE;
}

INT HttpServerSendResponse(HttpConnection *Conn, HttpResponse *Resp) {
    CHAR HeaderBuf[4096];
    CHAR *Ptr = HeaderBuf;
    UINT32 Remaining = sizeof(HeaderBuf);
    INT Written;
    
    if (!Conn || !Resp) RETURN(NO_OBJECT);
    
    /* Status line */
    Written = SnPrintf(Ptr, Remaining, "%s %u %s\r\n",
                       HTTP_VERSION_11,
                       Resp->StatusCode,
                       HttpStatusText(Resp->StatusCode));
    if (Written < 0) RETURN(NO_MEMORY);
    Ptr += Written;
    Remaining -= Written;
    
    /* Server header */
    Written = SnPrintf(Ptr, Remaining, "Server: TOS/0.04\r\n");
    if (Written < 0) RETURN(NO_MEMORY);
    Ptr += Written;
    Remaining -= Written;
    
    /* Content-Length */
    Written = SnPrintf(Ptr, Remaining, "Content-Length: %u\r\n", Resp->BodyLen);
    if (Written < 0) RETURN(NO_MEMORY);
    Ptr += Written;
    Remaining -= Written;
    
    /* Content-Type */
    Written = SnPrintf(Ptr, Remaining, "Content-Type: %s\r\n",
                       Resp->ContentType[0] ? Resp->ContentType : "text/plain");
    if (Written < 0) RETURN(NO_MEMORY);
    Ptr += Written;
    Remaining -= Written;
    
    /* Connection */
    Written = SnPrintf(Ptr, Remaining, "Connection: %s\r\n",
                       Conn->KeepAlive ? "keep-alive" : "close");
    if (Written < 0) RETURN(NO_MEMORY);
    Ptr += Written;
    Remaining -= Written;
    
    /* End of headers */
    Written = SnPrintf(Ptr, Remaining, "\r\n");
    if (Written < 0) RETURN(NO_MEMORY);
    Ptr += Written;
    Remaining -= Written;
    
    /* Send headers */
    if (TcpSend(Conn->Sock, (UINT8*)HeaderBuf, (UINT32)(Ptr - HeaderBuf)) < 0) {
        RETURN(IO_ERROR);
    }
    
    /* Send body */
    if (Resp->Body && Resp->BodyLen > 0) {
        if (TcpSend(Conn->Sock, Resp->Body, Resp->BodyLen) < 0) {
            RETURN(IO_ERROR);
        }
    }
    
    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * Initialization
 * ============================================================================
 */

INT HttpInit(NOPTR) {
    if (GHttpInitialized) {
        RETURN(SUCCESS);
    }
    
    /* Set default config */
    GDefaultConfig.TimeoutMs = 30000;
    GDefaultConfig.FollowRedirects = TRUE;
    GDefaultConfig.MaxRedirects = HTTP_MAX_REDIRECTS;
    GDefaultConfig.KeepAlive = FALSE;
    StrCpy(GDefaultConfig.UserAgent, "TOS/0.04");
    StrCpy(GDefaultConfig.Accept, "*/*");
    StrCpy(GDefaultConfig.AcceptEncoding, "gzip, deflate");
    
    GHttpInitialized = TRUE;
    
    RETURN(SUCCESS);
}

NOPTR HttpShutdown(NOPTR) {
    GHttpInitialized = FALSE;
}