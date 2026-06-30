#include <Network/Dhcp.h>
#include <Network/Udp.h>
#include <Network/IpV4.h>
#include <Network/Dns.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Memory/Allocator.h>
#include <Crypto/Rng.h>
#include <Kernel/Return.h>

/*
 * List of active DHCP clients
 */
typedef struct DhcpClientEntry {
    DhcpClient Client;
    struct DhcpClientEntry *Next;
} DhcpClientEntry;

static DhcpClientEntry *GDhcpClients = NULLPTR;
static UINT32 GDhcpClientsCount = 0;

EXTERN(ListHead, GNetworkDevices);

/*
 * =============================================================================== Auxiliary functions ================================================================================
 */

static UINT32 DhcpGenerateXid(NOPTR) {
    UINT32 Xid;
    RngGetRandomBytes((UINT8*)&Xid, sizeof(Xid));
    return Xid;
}

static NOPTR DhcpAddOption(UINT8 *Options, INT *Offset, UINT8 Type, UINT8 Len, NOPTR *Data) {
    Options[(*Offset)++] = Type;
    Options[(*Offset)++] = Len;
    MemCpy(Options + *Offset, Data, Len);
    *Offset += Len;
}

static NOPTR DhcpAddOptionByte(UINT8 *Options, INT *Offset, UINT8 Type, UINT8 Value) {
    Options[(*Offset)++] = Type;
    Options[(*Offset)++] = 1;
    Options[(*Offset)++] = Value;
}

static NOPTR DhcpAddOptionDword(UINT8 *Options, INT *Offset, UINT8 Type, UINT32 Value) {
    Options[(*Offset)++] = Type;
    Options[(*Offset)++] = 4;
    *(UINT32*)(Options + *Offset) = Htonl(Value);
    *Offset += 4;
}

static NOPTR DhcpAddOptionEnd(UINT8 *Options, INT *Offset) {
    Options[(*Offset)++] = DHCP_OPT_END;
}

static UINT8* DhcpFindOption(DhcpHeader *Dhcp, UINT8 OptType, INT *Len) {
    UINT8 *Opt = Dhcp->Options;
    INT Offset = 0;
    
    while (Offset < 300) {
        UINT8 Type = Opt[Offset++];
        if (Type == DHCP_OPT_END) break;
        if (Type == DHCP_OPT_PAD) continue;
        
        UINT8 OptLen = Opt[Offset++];
        
        if (Type == OptType) {
            if (Len) *Len = OptLen;
            return Opt + Offset;
        }
        
        Offset += OptLen;
    }
    
    return NULLPTR;
}

/*
 * ============================================================================= Sending a DHCP message ================================================================================ Sending a DHCP message
 */

static INT DhcpSendMessage(DhcpClient *Client, UINT8 MsgType, UINT32 RequestedIp) {
    NetworkBuf *Buf = NetworkAllocBuf(sizeof(DhcpHeader) + 312);
    if (!Buf) return -1;
    
    DhcpHeader *Dhcp = (DhcpHeader*)Buf->Data;
    MemSet(Dhcp, 0, sizeof(DhcpHeader));
    
    Dhcp->Op = 1;
    Dhcp->HType = 1;
    Dhcp->HLen = 6;
    Dhcp->Xid = Htonl(Client->Xid);
    Dhcp->Secs = Htons(0);
    Dhcp->Flags = Htons(0x8000);
    
    MemCpy(Dhcp->ChAddr, Client->Dev->MacAddr, 6);
    Dhcp->Magic = Htonl(DHCP_MAGIC_COOKIE);
    
    INT OptOffset = 0;
    UINT8 *Options = Dhcp->Options;
    
    DhcpAddOptionByte(Options, &OptOffset, DHCP_OPT_MSG_TYPE, MsgType);
    
    UINT8 ClientId[7] = {1};
    MemCpy(ClientId + 1, Client->Dev->MacAddr, 6);
    DhcpAddOption(Options, &OptOffset, DHCP_OPT_CLIENT_ID, 7, ClientId);
    
    if (RequestedIp != 0) {
        DhcpAddOptionDword(Options, &OptOffset, DHCP_OPT_REQUESTED_IP, RequestedIp);
    }
    
    UINT8 ParamList[] = {1, 3, 6, 51};
    DhcpAddOption(Options, &OptOffset, DHCP_OPT_PARAM_LIST, sizeof(ParamList), ParamList);
    
    CHAR Hostname[] = "TOSPC";
    DhcpAddOption(Options, &OptOffset, DHCP_OPT_HOSTNAME, sizeof(Hostname) - 1, Hostname);
    
    DhcpAddOptionEnd(Options, &OptOffset);
    
    Buf->Len = sizeof(DhcpHeader) + OptOffset;
    
    IpV4Addr Dest = { .Addr = 0xFFFFFFFF };
    IpV4Addr Src = { .Addr = 0 };
    
    const CHAR *MsgNames[] = {"", "DISCOVER", "OFFER", "REQUEST", "DECLINE", "ACK", "NAK", "RELEASE", "INFORM"};
    const CHAR *MsgName = (MsgType < 9) ? MsgNames[MsgType] : "UNKNOWN";
    
    return UdpOutput(Buf, Dest, Src, DHCP_SERVER_PORT, DHCP_CLIENT_PORT);
}

/*
 * ============================================================================== Processing the DHCP response =================================================================================
 */

static UINT64 DhcpNowSeconds(NOPTR) {
    UINT32 Tpm = TimerTicksPerMs();
    if (Tpm == 0) {
        Tpm = 1;
    }
    return TimerTicks() / Tpm / 1000;
}

static NOPTR DhcpProcessResponse(DhcpClient *Client, DhcpHeader *Dhcp) {
    UINT32 Xid = Ntohl(Dhcp->Xid);
    if (Xid != Client->Xid) return;
    
    INT OptLen;
    UINT8 *Opt = DhcpFindOption(Dhcp, DHCP_OPT_MSG_TYPE, &OptLen);
    if (!Opt || OptLen < 1) return;
    
    UINT8 MsgType = Opt[0];
    
    switch (MsgType) {
        case DHCP_OFFER: {
            CHAR IpStr[16];

            Client->YIAddr = Ntohl(Dhcp->YIAddr);
            
            Opt = DhcpFindOption(Dhcp, DHCP_OPT_SERVER_ID, &OptLen);
            if (Opt && OptLen >= 4) {
                Client->ServerIp = Ntohl(*(UINT32*)Opt);
            }
            
            Opt = DhcpFindOption(Dhcp, DHCP_OPT_SUBNET_MASK, &OptLen);
            if (Opt && OptLen >= 4) {
                Client->SubnetMask = Ntohl(*(UINT32*)Opt);
            }
            
            Opt = DhcpFindOption(Dhcp, DHCP_OPT_ROUTER, &OptLen);
            if (Opt && OptLen >= 4) {
                Client->Gateway = Ntohl(*(UINT32*)Opt);
            }
            
            Opt = DhcpFindOption(Dhcp, DHCP_OPT_DNS_SERVER, &OptLen);
            if (Opt && OptLen >= 4) {
                Client->DnsServer = Ntohl(*(UINT32*)Opt);
            }
            
            Opt = DhcpFindOption(Dhcp, DHCP_OPT_LEASE_TIME, &OptLen);
            if (Opt && OptLen >= 4) {
                Client->LeaseTime = Ntohl(*(UINT32*)Opt);
            }

            IpV4NTop((IpV4Addr){ .Addr = Client->YIAddr }, IpStr, 16);
            
            DhcpSendMessage(Client, DHCP_REQUEST, Client->YIAddr);
            break;
        }
            
        case DHCP_ACK: {
            Client->Configured = TRUE;
            
            MemCpy(Client->Dev->IpAddr, &Client->YIAddr, 4);
            MemCpy(Client->Dev->Netmask, &Client->SubnetMask, 4);
            MemCpy(Client->Dev->Gateway, &Client->Gateway, 4);
            
            IpV4RouteAdd((IpV4Addr){ .Addr = Client->YIAddr & Client->SubnetMask },
                          (IpV4Addr){ .Addr = Client->SubnetMask },
                          (IpV4Addr){0},
                          (IpV4Addr){ .Addr = Client->YIAddr },
                          Client->Dev, 0);
            
            if (Client->Gateway != 0) {
                IpV4RouteAdd((IpV4Addr){0}, (IpV4Addr){0},
                              (IpV4Addr){ .Addr = Client->Gateway },
                              (IpV4Addr){ .Addr = Client->YIAddr },
                              Client->Dev, 0);
            }
            
            if (Client->DnsServer != 0) {
                DnsSetNameserver((IpV4Addr){ .Addr = Client->DnsServer });
            }
            
            {
                UINT64 Now = DhcpNowSeconds();
                Client->LeaseExpires = Now + Client->LeaseTime;
                Client->RenewTime = Now + Client->LeaseTime / 2;
                Client->RebindTime = Now + Client->LeaseTime * 7 / 8;
            }
            
            CHAR IpStr[16], MaskStr[16], GwStr[16];
            IpV4NTop((IpV4Addr){ .Addr = Client->YIAddr }, IpStr, 16);
            IpV4NTop((IpV4Addr){ .Addr = Client->SubnetMask }, MaskStr, 16);
            IpV4NTop((IpV4Addr){ .Addr = Client->Gateway }, GwStr, 16);
            
            if (Client->Callback) {
                Client->Callback(Client, TRUE);
            }
            break;
        }
            
        case DHCP_NAK: {
            Client->Configured = FALSE;
            if (Client->Callback) {
                Client->Callback(Client, FALSE);
            }
            break;
        }
    }
}

/*
 * =============================================================================== Incoming DHCP packet handler ================================================================================
 */

NOPTR DhcpInput(NetworkBuf *Buf, IpV4Addr Src, IpV4Addr Dst,
                UINT16 SrcPort, UINT16 DstPort) {
    (NOPTR)Src;
    (NOPTR)Dst;
    (NOPTR)SrcPort;
    
    if (Buf->Len < sizeof(DhcpHeader)) return;
    
    DhcpHeader *Dhcp = (DhcpHeader*)Buf->Data;
    
    if (Ntohl(Dhcp->Magic) != DHCP_MAGIC_COOKIE) return;
    
    /*
 * Looking for a client by MAC address
 */
    DhcpClientEntry *Entry = GDhcpClients;
    while (Entry) {
        if (MemCmp(Entry->Client.Dev->MacAddr, Dhcp->ChAddr, 6) == 0) {
            DhcpProcessResponse(&Entry->Client, Dhcp);
            break;
        }
        Entry = Entry->Next;
    }
}

/*
 * =============================================================================== DHCP client API (universal) ===============================================================================
 */

INT DhcpInit(DhcpClient *Client, NetworkDevice *Dev) {
    if (!Client || !Dev) RETURN(NO_OBJECT);
    
    MemSet(Client, 0, sizeof(DhcpClient));
    Client->Dev = Dev;
    Client->Xid = DhcpGenerateXid();
    
    /*
 * Add to the global list
 */
    DhcpClientEntry *Entry = (DhcpClientEntry*)MemoryAllocate(sizeof(DhcpClientEntry));
    if (!Entry) RETURN(NO_OBJECT);
    
    MemCpy(&Entry->Client, Client, sizeof(DhcpClient));
    Entry->Next = GDhcpClients;
    GDhcpClients = Entry;
    GDhcpClientsCount++;
    
    RETURN(SUCCESS);
}

INT DhcpStart(DhcpClient *Client) {
    if (!Client) RETURN(NO_OBJECT);
    
    Client->Running = TRUE;
    Client->Configured = FALSE;
    Client->Xid = DhcpGenerateXid();
    
    return DhcpSendMessage(Client, DHCP_DISCOVER, 0);
}

INT DhcpRenew(DhcpClient *Client) {
    if (!Client || !Client->Configured) RETURN(NO_OBJECT);
    
    Client->Xid = DhcpGenerateXid();
    return DhcpSendMessage(Client, DHCP_REQUEST, Client->YIAddr);
}

INT DhcpRelease(DhcpClient *Client) {
    if (!Client || !Client->Configured) RETURN(NO_OBJECT);
    
    DhcpSendMessage(Client, DHCP_RELEASE, Client->YIAddr);
    Client->Configured = FALSE;
    
    RETURN(SUCCESS);
}

/*
 * ============================================================================== Start DHCP on all interfaces ===============================================================================
 */

NOPTR DhcpStartOnAllInterfaces(DhcpCallback Callback) {
    ListHead *Pos;
    INT Started = 0;
    
    ListForEach(Pos, &GNetworkDevices) {
        NetworkDevice *Dev = ListEntry(Pos, NetworkDevice, Node);
        
        /*
 * We configure only interfaces that can work with DHCP
 */
        if (Dev->Type == NETWORK_TYPE_ETHERNET || Dev->Type == NETWORK_TYPE_WIFI) {
            if (Dev->Up && Dev->Running) {
                DhcpClient *Client = (DhcpClient*)MemoryAllocate(sizeof(DhcpClient));
                if (Client) {
                    DhcpInit(Client, Dev);
                    Client->Callback = Callback;
                    DhcpStart(Client);
                    Started++;
                }
            }
        }
    }
}

/*
 * =============================================================================== Initializing the DHCP subsystem ==============================================================================
 */

NOPTR DhcpSubsystemInit(NOPTR) {
    GDhcpClients = NULLPTR;
    GDhcpClientsCount = 0;
    
    UdpRegisterHandler(DHCP_CLIENT_PORT, DhcpInput);
}

NOPTR DhcpTimerHandler(NOPTR) {
    UINT64 Now = DhcpNowSeconds();
    DhcpClientEntry *Entry = GDhcpClients;
    
    while (Entry) {
        DhcpClient *Client = &Entry->Client;
        
        if (Client->Configured && Client->Running && Client->LeaseTime > 0) {
            if (Now >= Client->LeaseExpires) {
                Client->Configured = FALSE;
                DhcpStart(Client);
            } else if (Now >= Client->RebindTime) {
                DhcpRenew(Client);
                Client->RebindTime = Now + Client->LeaseTime / 8;
            } else if (Now >= Client->RenewTime) {
                DhcpRenew(Client);
                Client->RenewTime = Now + Client->LeaseTime / 2;
            }
        }
        
        Entry = Entry->Next;
    }
}
