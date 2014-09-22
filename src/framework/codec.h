/*
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License Version 2 as
** published by the Free Software Foundation.  You may not use, modify or
** distribute this program under any other version of the GNU General
** Public License.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
// codec.h author Josh Rosenbaum <jrosenba@cisco.com>

#ifndef FRAMEWORK_CODEC_H
#define FRAMEWORK_CODEC_H

#include <vector>
#include <cstdint>
#include <cstddef>
#include <type_traits> // static_assert

#include "main/snort_types.h"
#include "framework/base_api.h"

// unfortunately necessary due to use of Ipapi in struct
#include "protocols/ip.h"
#include "protocols/mpls.h"  // FIXIT-M remove MPLS from Convenience pointers

struct TextLog;
struct Packet;
struct Layer;

namespace ip
{
    class IpApi;
}
namespace tcp
{
    struct TCPHdr;
}
namespace udp
{
    struct UDPHdr;
}
namespace icmp
{
    struct ICMPHdr;
}

// Used by root codecs to add their DLT to their HELP string
#define STRINGIFY(x) #x
#define ARG_STRINGIFY(x) STRINGIFY(x)
#define ADD_DLT(help, x) help " (DLT " ARG_STRINGIFY(x) ")"


constexpr uint8_t MIN_TTL = 64;
constexpr uint8_t MAX_TTL = 255;


typedef uint64_t EncodeFlags;
constexpr EncodeFlags ENC_FLAG_FWD = 0x8000000000000000;  // send in forward direction
constexpr EncodeFlags ENC_FLAG_SEQ = 0x4000000000000000;  // VAL bits contain seq adj
constexpr EncodeFlags ENC_FLAG_ID  = 0x2000000000000000;  // use randomized IP ID
constexpr EncodeFlags ENC_FLAG_NET = 0x1000000000000000;  // stop after innermost network (ip4/6) layer
constexpr EncodeFlags ENC_FLAG_DEF = 0x0800000000000000;  // stop before innermost ip4 opts or ip6 frag header
constexpr EncodeFlags ENC_FLAG_RAW = 0x0400000000000000;  // don't encode outer eth header (this is raw ip)
constexpr EncodeFlags ENC_FLAG_PAY = 0x0200000000000000;  // set to when a TCP payload is attached
constexpr EncodeFlags ENC_FLAG_PSH = 0x0100000000000000;  // set by PacketManager when TCP should set PUSH flag
constexpr EncodeFlags ENC_FLAG_FIN = 0x0080000000000000;  // set by PacketManager when TCP should set FIN flag
constexpr EncodeFlags ENC_FLAG_TTL = 0x0040000000000000;  // set by PacketManager when TCP should set FIN flag
constexpr EncodeFlags ENC_FLAG_INLINE = 0x0020000000000000;  // set by PacketManager when TCP should set FIN flag
constexpr EncodeFlags ENC_FLAG_VAL = 0x00000000FFFFFFFF;  // bits for adjusting seq and/or ack


static inline bool forward (const EncodeFlags f)
{ return f & ENC_FLAG_FWD; }

static inline bool reverse (const EncodeFlags f)
{ return !forward(f); }

constexpr uint8_t ENC_PROTO_UNSET = 0xFF;
struct EncState
{
    const ip::IpApi& ip_api; /* IP related information. Good for checksums */
    EncodeFlags flags;
    const uint16_t dsize; /* for non-inline, TCP sequence numbers */
    uint16_t next_ethertype; /*  set the next encoder 'proto' field to this value. */
    uint8_t next_proto; /*  set the next encoder 'proto' field to this value. */
    const uint8_t ttl;

    EncState(const ip::IpApi& api, EncodeFlags f, uint16_t pr,
        uint8_t t, uint16_t data_size) :
        ip_api(api),
        flags(f),
        dsize(data_size),
        next_ethertype(0),
        next_proto(pr),
        ttl(t)

    { }

    inline bool next_proto_set() const
    { return (next_proto != ENC_PROTO_UNSET); }

    inline bool ethertype_set() const
    { return next_ethertype != 0; }

    inline uint8_t get_ttl(uint8_t lyr_ttl) const
    {
        if (forward(flags))
        {
            if (flags & ENC_FLAG_TTL)
                return ttl;
            else
                return lyr_ttl;
        }
        else
        {
            uint8_t new_ttl;

            if (flags & ENC_FLAG_TTL)
                new_ttl = ttl;
            else
                new_ttl = MAX_TTL - lyr_ttl;

            if (new_ttl < MIN_TTL)
                new_ttl = MIN_TTL;

            return new_ttl;
        }
    }
};


// Copied from dnet/blob.h
// * base+off is start of packet
// * base+end is start of current layer
// * base+size-1 is last byte of packet (in) / buffer (out)
struct Buffer
{
    uint8_t* base; /* start of data */ /* FIXIT-L J - make this private. Ppl should be to access, not manipulate */
    uint32_t off;       /* offset into data */
private:
    uint32_t end;       /* end of data */
    const uint32_t max_len;   /* size of allocation */
public:

    /* Logic behind 'buf + size + 1' -- we're encoding the
     * packet from the inside out.  So, whenever we add
     * data, 'allocating' N bytes means moving the pointer
     * N characters farther from the end. For this scheme
     * to work, an empty Buffer means the data pointer is
     * invalid and is actually one byte past the end of the
     * array
     */
    Buffer(uint8_t* buf, uint32_t size) :
        base(buf + size + 1),
        off(0),
        end(0),
        max_len(size)
    { }

    uint32_t size() const
    { return end; }

    inline bool allocate(uint32_t len)
    {
        if ( (end + len) > max_len )
            return false;

        end += len;
        base -= len;
        return true;
    }

    inline void clear()
    {
        base = base + end;
        end = 0;
        off = 0;
    }
};


struct RawData
{
    const uint8_t* data;
    uint32_t len;
};


enum DecodeFlags : std::uint16_t
{
    /*
     * DO NOT USE PKT_TYPE_* directly!! Use PktType enum and
     *      access methods to get/set.
     */
    PKT_TYPE_UNKOWN = 0x00,
    PKT_TYPE_IP = 0x01,
    PKT_TYPE_TCP = 0x02,
    PKT_TYPE_UDP = 0x03,
    PKT_TYPE_ICMP4 = 0x04,
    PKT_TYPE_ICMP6 = 0x05,
    PKT_TYPE_ARP = 0x06,
    PKT_TYPE_FREE = 0x07, /* If protocol is added, update enum class PktType below. */
    PKT_TYPE_MASK = 0x07,

    /* error flags */
    DECODE_ERR_CKSUM_IP = 0x0008,
    DECODE_ERR_CKSUM_TCP = 0x0010,
    DECODE_ERR_CKSUM_UDP = 0x0020,
    DECODE_ERR_CKSUM_ICMP = 0x0040,
    DECODE_ERR_CKSUM_ANY = 0x0080,
    DECODE_ERR_BAD_TTL = 0x0100,
    DECODE_ERR_FLAGS = (DECODE_ERR_CKSUM_IP | DECODE_ERR_CKSUM_TCP |
                        DECODE_ERR_CKSUM_UDP | DECODE_ERR_CKSUM_UDP |
                        DECODE_ERR_CKSUM_ICMP | DECODE_ERR_CKSUM_ANY |
                        DECODE_ERR_BAD_TTL),


    DECODE_PKT_TRUST = 0x0200,    /* Tell Snort++ to whitelist this packet */
    DECODE_FRAG = 0x0400,  /* flag to indicate a fragmented packet */
    DECODE_MF = 0x0800,
};

/* NOTE: if A protocol is added, update DecodeFlags! */
enum class PktType : std::uint8_t
{
    UNKOWN = PKT_TYPE_UNKOWN,
    IP = PKT_TYPE_IP,
    TCP = PKT_TYPE_TCP,
    UDP = PKT_TYPE_UDP,
    ICMP4 = PKT_TYPE_ICMP4,
    ICMP6 = PKT_TYPE_ICMP6,
    ARP = PKT_TYPE_ARP,
};

struct SnortData
{
    /*  Pointers which will be used by Snort++. (starting with uint16_t so tcph is 64 bytes from start*/

    /*
     * these four pounters are each referenced literally
     * dozens if not hundreds of times.  NOTHING else should be added!!
     */
    const tcp::TCPHdr* tcph;
    const udp::UDPHdr* udph;
    const icmp::ICMPHdr* icmph;
    uint16_t sp;            /* source port (TCP/UDP) */
    uint16_t dp;            /* dest port (TCP/UDP) */
    uint16_t decode_flags;  /* First bits (currently 3), which are masked using the constant
                             * DECODE_PKT_TYPE_MASK defined above, are specifically
                             * for the PktType. Everything else is fair game flag.
                             *
                             */

    ip::IpApi ip_api;
    mpls::MplsHdr mplsHdr;

    inline void reset()
    {
        static_assert(PKT_TYPE_UNKOWN == 0,
            "The Packets 'type' gets resets to zero - "
            "which means zero is unkown");
        memset((char*)&tcph, '\0', offsetof(SnortData, ip_api));
        ip_api.reset();
    }

    inline void set_pkt_type(PktType pkt_type)
    { decode_flags = (decode_flags & ~PKT_TYPE_MASK) | static_cast<uint16_t>(pkt_type); }

    inline PktType get_pkt_type() const
    { return static_cast<PktType>(decode_flags & PKT_TYPE_MASK); }
};


struct CodecData
{
    /* This section will get reset before every decode() function call */
    uint16_t next_prot_id;      /* protocol type of the next layer */
    uint16_t lyr_len;           /* The length of the valid part layer */
    uint16_t invalid_bytes;     /* the length of the INVALID part of this layer */

    /* Reset before each decode of packet begins */

    /*  Codec specific fields.  These fields are only relevent to codecs. */
    uint16_t proto_bits;    /* protocols contained within this packet */
                            /*   -- will be propogated to Snort++ Packet struct*/
    uint16_t codec_flags;   /* flags used while decoding */
    uint8_t ip_layer_cnt;

    /*  The following values have junk values after initialization */
    uint8_t ip6_extension_count; /* initialized in cd_ipv6.cc */
    uint8_t curr_ip6_extension;  /* initialized in cd_ipv6.cc */
    uint8_t ip6_csum_proto;      /* initalized in cd_ipv6.cc.  Used for IPv6 checksums */

    // FIXIT-H-J - most of these don't needs to be zeroed
    CodecData(uint16_t init_prot) : lyr_len(0),
                                    invalid_bytes(0),
                                    proto_bits(0),
                                    codec_flags(0),
                                    ip_layer_cnt(0)
    { next_prot_id = init_prot; }
};

#define PROTO_BIT__NONE     0x0000
#define PROTO_BIT__IP       0x0001
#define PROTO_BIT__ARP      0x0002
#define PROTO_BIT__TCP      0x0004
#define PROTO_BIT__UDP      0x0008
#define PROTO_BIT__ICMP     0x0010
#define PROTO_BIT__TEREDO   0x0020
#define PROTO_BIT__GTP      0x0040
#define PROTO_BIT__MPLS     0x0080
#define PROTO_BIT__VLAN     0x0100
#define PROTO_BIT__ETH      0x0200
#define PROTO_BIT__TCP_EMBED_ICMP  0x0400
#define PROTO_BIT__UDP_EMBED_ICMP  0x0800
#define PROTO_BIT__ICMP_EMBED_ICMP 0x1000
#define PROTO_BIT__IP6_EXT  0x2000
#define PROTO_BIT__FREE     0x4000
#define PROTO_BIT__OTHER    0x8000
#define PROTO_BIT__ALL      0xffff




/*  Decode Flags */
constexpr uint16_t CODEC_DF = 0x0001;    /* don't fragment flag */
constexpr uint16_t CODEC_UNSURE_ENCAP = 0x0002; /* packet may have incorrect encapsulation layer.
                                                 * don't alert if "next layer" is invalid.
                                                 * If decode fails with this bit set, PacketManager
                                                 *          will back out to the previous layer.
                                                 * IMPORTANT:  This bit can ONLY be set if the
                                                 *              DECODE_ENCAP_LAYER flag was
                                                 *              was previously set.
                                                 */
constexpr uint16_t CODEC_SAVE_LAYER = 0x0004;   /* DO NOT USE THIS LAYER!!
                                                 *  --  use DECODE_ENCAP_LAYER
                                                 */
constexpr uint16_t CODEC_ENCAP_LAYER = (CODEC_SAVE_LAYER | CODEC_UNSURE_ENCAP );
                                            /* If encapsulation decode fails, back out to this layer
                                             * This will be cleared by PacketManager between decodes
                                             * This flag automatically sets DECODE_ENCAP_LAYER for
                                             *      the next layer (and only the next layer).
                                             */
constexpr uint16_t CODEC_ROUTING_SEEN = 0x0008; /* used to check ip6 extensino order */
constexpr uint16_t CODEC_IPOPT_RR_SEEN = 0x0010; /* used by icmp4 for alerting */
constexpr uint16_t CODEC_IPOPT_RTRALT_SEEN = 0x0020;  /* used by IGMP for alerting */
constexpr uint16_t CODEC_IPOPT_LEN_THREE = 0x0040; /* used by IGMP for alerting */
constexpr uint16_t CODEC_TEREDO_SEEN = 0x0080; /* used in IPv6 Codec */
constexpr uint16_t CODEC_STREAM_REBUILT = 0x0100; /* Set by PacketManager. used by codec_event */

constexpr uint16_t CODEC_IPOPT_FLAGS = (CODEC_IPOPT_RR_SEEN |
                                        CODEC_IPOPT_RTRALT_SEEN |
                                        CODEC_IPOPT_LEN_THREE);

/*  Codec Class */

class SO_PUBLIC Codec
{
public:
    virtual ~Codec() { };

    // PKT_MAX = ETHERNET_HEADER_LEN () + VLAN_HEADER (4) + ETHERNET_MTU () + IP_MAXPACKET()

    /* PKT_MAX is sized to ensure that any reassembled packet
     * can accommodate a full datagram at innermost layer
     *
     * ETHERNET_HEADER_LEN == 14
     * VLAN_HEADER == 4
     * ETHERNET_MTU == 1500
     * IP_MAXPACKET ==  65535
     */
    static const uint32_t PKT_MAX = 14 + 4 + 1500 + 65535;

    /*  Codec Initialization */

    // Get the codec's name
    inline const char* get_name() const
    {return name; }
    // Registers this Codec's data link type (as defined by libpcap)
    virtual void get_data_link_type(std::vector<int>&)
    { }
    // Register the code's protocol ID's and Ethertypes
    virtual void get_protocol_ids(std::vector<uint16_t>&)
    { }

    /*
     * Main decoding function!  Will get called when decoding a packet.
     *
     * PARAMS:
     *      const RawData& = struct containing informatin about the
     *                      current packet's raw data
     *
     *      CodecData& = Pass information the PacketManager and other
     *              codecs. IMPORTANT FIELDS TO SET IN YOUR CODEC -
     *         next_prot_id   = protocol type of the next layer
     *         lyr_len        = The length of the valid part layer
     *         invalid bytes  = number of invalid bytes between the end of
     *                          the this layer's valid length and the next
     *                           layer. For instance,
     *                          when decoding IP, if there are 20 bytes of
     *                          options but only 12 bytes are valid.
     *
     *                          data.lyr_len = MIN_IP_HEADER_LEN + 12;
     *                          data.invalid_bytes = 8    === 20 - 12
     *
     *      SnortData& = Data which will be sent to the rest of Snort++.
     *                      contains convenience pointers and information
     *                      about this packet.
     **/
    virtual bool decode(const RawData&, CodecData&, SnortData&)=0;

    /*
     *  Log this layer's information
     *  PARAMS:
     *          TextLog* = the logger. Defined in "text_log.h"
     *          const uint8_t *raw_pkt = the same data seen during decode
     *          Packet *p = pointer to the packet struct.
     */
    virtual void log(TextLog* const, const uint8_t* /*raw_pkt*/,
                    const Packet* const)
    { }


    /*
     * Encoding -- active response!!
     *
     * Encode the current packet. Encoding starts with the innermost
     * layer and working outwards.  All encoders MUST call the
     * Buffer.allocate() function before writing to output buffer.
     * PARAMS:
     *        uint8_t* raw_in =  A pointer to the raw input which was decoded.
     *              This is the same pointer which was given to decode().
     *        uint16_t len == the value to which '(CodecData&).lyr_len' was set during decode.
     *              I.e., the validated length of this layer. Some protocols,
     *              like IPv4 (original ipv4 header may contain invalid options
     *              which we don't want to copy) and GTP have dynamic lengths.
     *              So, this parameter ensure the encode() function doesn't
     *              need to revalidatae and recalculate the length.
     *        EncState& = The current EncState struct
     *        Buffer& = the packet which will be sent. All inward layers will already
     *              be set.
     *
     * NOTE:  all funtions MUST call the Buffer.allocate() function before
     *          memory.
     */
    virtual bool encode(const uint8_t* const /*raw_in */,
                        const uint16_t /*raw_len*/,
                        EncState&,
                        Buffer&)
    { return true; }

    // update function
    virtual bool update(Packet*, Layer*, uint32_t* /*len*/)
    { return true; }

    // formatter
    virtual void format(EncodeFlags, const Packet* /*orig*/, Packet* /*clone*/, Layer*)
    { }


protected:
    Codec(const char* s)
    { name = s; }

private:
    const char* name;
};



//-------------------------------------------------------------------------
// api
//-------------------------------------------------------------------------


// this is the current version of the api
#define CDAPI_VERSION 0

// this is the version of the api the plugins are using
// to be useful, these must be explicit (*_V0, *_V1, ...)
#define CDAPI_PLUGIN_V0 0

typedef Codec* (*CdNewFunc)(Module*);
typedef void (*CdDelFunc)(Codec *);
typedef void (*CdAuxFunc)();

struct CodecApi
{
    BaseApi base;

    // these may be nullptr
    CdAuxFunc pinit;  // initialize global plugin data
    CdAuxFunc pterm;  // clean-up pinit()

    CdAuxFunc tinit;  // initialize thread-local plugin data
    CdAuxFunc tterm;  // clean-up tinit()

    // these must be set
    CdNewFunc ctor;   // get eval optional instance data
    CdDelFunc dtor;   // clean up instance data
};

#endif

