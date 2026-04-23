/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndLinkTracer implementation — classified link traffic.
 */

#include "ndndsim-link-tracer.h"

#include "ns3/log.h"
#include "ns3/simulator.h"

#include <cstring>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("NdndLinkTracer");

namespace ndndsim
{

// ─── NdnPayloadTag ─────────────────────────────────────────────────

NS_OBJECT_ENSURE_REGISTERED(NdnPayloadTag);

TypeId
NdnPayloadTag::GetTypeId()
{
    static TypeId tid = TypeId("ns3::ndndsim::NdnPayloadTag")
                            .SetParent<Tag>()
                            .AddConstructor<NdnPayloadTag>();
    return tid;
}

TypeId
NdnPayloadTag::GetInstanceTypeId() const
{
    return GetTypeId();
}

void
NdnPayloadTag::Serialize(TagBuffer i) const
{
    i.WriteU32(m_size);
}

void
NdnPayloadTag::Deserialize(TagBuffer i)
{
    m_size = i.ReadU32();
}

uint32_t
NdnPayloadTag::GetSerializedSize() const
{
    return 4;
}

void
NdnPayloadTag::Print(std::ostream& os) const
{
    os << "NdnPayload=" << m_size;
}

// ─── NDN TLV constants ─────────────────────────────────────────────

static constexpr uint8_t kTlvInterest = 0x05;
static constexpr uint8_t kTlvData = 0x06;
static constexpr uint8_t kTlvName = 0x07;
static constexpr uint8_t kTlvGenericComponent = 0x08;
static constexpr uint8_t kTlvKeywordComponent = 0x20;
static constexpr uint8_t kTlvLpPacket = 0x64;   // NDNLPv2 LpPacket
static constexpr uint8_t kTlvLpFragment = 0x50;  // NDNLPv2 Fragment

// ─── Tiny TLV helpers (read type / length from a buffer) ───────────

/// Read a TLV-TYPE or TLV-LENGTH varint.  Returns 0 on failure.
static uint64_t
ReadVarNum(const uint8_t*& p, const uint8_t* end)
{
    if (p >= end)
        return 0;
    uint8_t first = *p++;
    if (first < 253)
        return first;
    if (first == 253)
    {
        if (p + 2 > end)
            return 0;
        uint16_t v = (static_cast<uint16_t>(p[0]) << 8) | p[1];
        p += 2;
        return v;
    }
    if (first == 254)
    {
        if (p + 4 > end)
            return 0;
        uint32_t v = (static_cast<uint32_t>(p[0]) << 24) |
                     (static_cast<uint32_t>(p[1]) << 16) |
                     (static_cast<uint32_t>(p[2]) << 8) | p[3];
        p += 4;
        return v;
    }
    // first == 255: 8-byte — unlikely for names, skip
    if (p + 8 > end)
        return 0;
    p += 8;
    return 0;
}

/// Compare a name component value against a literal string.
static bool
ComponentEquals(const uint8_t* val, uint64_t valLen, const char* str)
{
    size_t sLen = std::strlen(str);
    return valLen == sLen && std::memcmp(val, str, sLen) == 0;
}

// ─── Classifier ────────────────────────────────────────────────────

TrafficCategory
NdndLinkTracer::Classify(const uint8_t* buf, uint32_t len)
{
    const uint8_t* p = buf;
    const uint8_t* end = buf + len;

    // 1. Read outer TLV type
    uint64_t outerType = ReadVarNum(p, end);

    // If wrapped in NDNLPv2, unwrap to find the Fragment
    if (outerType == kTlvLpPacket)
    {
        uint64_t lpLen = ReadVarNum(p, end);
        const uint8_t* lpEnd = p + lpLen;
        if (lpEnd > end)
            return TrafficCategory::Other;

        // Walk LP header fields until we find Fragment (0x50)
        while (p < lpEnd)
        {
            uint64_t fieldType = ReadVarNum(p, lpEnd);
            uint64_t fieldLen = ReadVarNum(p, lpEnd);
            if (p + fieldLen > lpEnd)
                return TrafficCategory::Other;

            if (fieldType == kTlvLpFragment)
            {
                // Fragment value is the inner Interest/Data TLV
                buf = p;
                len = static_cast<uint32_t>(fieldLen);
                end = buf + len;
                p = buf;
                outerType = ReadVarNum(p, end);
                break;
            }
            p += fieldLen;
        }

        // If we didn't find a Fragment, classify as Other
        if (outerType == kTlvLpPacket)
            return TrafficCategory::Other;
    }

    bool isInterest = (outerType == kTlvInterest);
    bool isData = (outerType == kTlvData);
    if (!isInterest && !isData)
        return TrafficCategory::Other;

    // Skip outer TLV-LENGTH
    ReadVarNum(p, end);

    // 2. First inner element must be Name (type 0x07)
    if (p >= end)
        return TrafficCategory::Other;
    uint64_t nameType = ReadVarNum(p, end);
    if (nameType != kTlvName)
        return TrafficCategory::Other;
    uint64_t nameLen = ReadVarNum(p, end);
    if (p + nameLen > end)
        return TrafficCategory::Other;

    const uint8_t* nameEnd = p + nameLen;

    // 3. Walk name components to classify.
    //
    // Categories:
    //   /localhop/.../32=DV/...           → DvAdvert
    //   /.../32=DV/32=PFS/...             → PrefixSync (onephase)
    //   /.../32=DV/32=PES/...             → PrefixSync (twophase PrefixEgreState)
    //   /localhost/nlsr/...               → Mgmt
    //   else                              → UserInterest or UserData

    bool firstIsLocalhost = false;
    bool seenDvKeyword = false;
    bool seenPfxSyncKeyword = false;
    bool secondIsNlsr = false;
    int compIdx = 0;

    const uint8_t* cp = p;
    while (cp < nameEnd)
    {
        uint64_t cType = ReadVarNum(cp, nameEnd);
        uint64_t cLen = ReadVarNum(cp, nameEnd);
        if (cp + cLen > nameEnd)
            break;
        const uint8_t* cVal = cp;
        cp += cLen;

        if (compIdx == 0 && cType == kTlvGenericComponent)
        {
            if (ComponentEquals(cVal, cLen, "localhost"))
                firstIsLocalhost = true;
        }
        else if (compIdx == 1 && firstIsLocalhost && cType == kTlvGenericComponent)
        {
            if (ComponentEquals(cVal, cLen, "nlsr"))
                secondIsNlsr = true;
        }

        if (cType == kTlvKeywordComponent)
        {
            if (ComponentEquals(cVal, cLen, "DV"))
                seenDvKeyword = true;
            if (ComponentEquals(cVal, cLen, "PFS") || ComponentEquals(cVal, cLen, "PES"))
                seenPfxSyncKeyword = true;
        }

        compIdx++;
    }

    // Classify
    if (firstIsLocalhost && secondIsNlsr)
        return TrafficCategory::Mgmt;
    if (seenDvKeyword && seenPfxSyncKeyword)
        return TrafficCategory::PrefixSync;
    if (seenDvKeyword)
        return TrafficCategory::DvAdvert;

    return isInterest ? TrafficCategory::UserInterest : TrafficCategory::UserData;
}

// ─── Tracer lifecycle ──────────────────────────────────────────────

static const char* kCategoryNames[] = {
    "DvAdvert", "PrefixSync", "Mgmt", "UserInterest", "UserData", "Other",
};

NdndLinkTracer::NdndLinkTracer(const std::string& file, Time period)
    : m_out(file),
      m_period(period),
      m_perPacket(false),
      m_counters{}
{
    m_out << "Time";
    for (size_t i = 0; i < kNumCategories; ++i)
        m_out << "," << kCategoryNames[i] << "_Pkts," << kCategoryNames[i] << "_Bytes";
    m_out << "\n";
}

NdndLinkTracer::NdndLinkTracer(const std::string& file)
    : m_out(file),
      m_period(Seconds(0)),
      m_perPacket(true),
      m_counters{}
{
    m_out << "Time,Category,Bytes\n";
}

NdndLinkTracer::~NdndLinkTracer()
{
    Stop();
}

std::shared_ptr<NdndLinkTracer>
NdndLinkTracer::Create(const std::string& file, Time period)
{
    auto tracer = std::shared_ptr<NdndLinkTracer>(new NdndLinkTracer(file, period));
    tracer->ScheduleNext();
    return tracer;
}

std::shared_ptr<NdndLinkTracer>
NdndLinkTracer::CreatePerPacket(const std::string& file)
{
    return std::shared_ptr<NdndLinkTracer>(new NdndLinkTracer(file));
}

void
NdndLinkTracer::ConnectLink(NetDeviceContainer devices)
{
    for (uint32_t i = 0; i < devices.GetN(); ++i)
    {
        ConnectDevice(devices.Get(i));
    }
}

void
NdndLinkTracer::ConnectDevice(Ptr<NetDevice> dev)
{
    if (dev)
    {
        dev->TraceConnectWithoutContext(
            "MacTx",
            MakeCallback(&NdndLinkTracer::MacTxCallback, this));
    }
}

void
NdndLinkTracer::MacTxCallback(Ptr<const Packet> packet)
{
    uint32_t sz = packet->GetSize();
    std::vector<uint8_t> buf(sz);
    packet->CopyData(buf.data(), sz);

    // Read the tag set by OnSendPacket to find where NDN TLV starts.
    NdnPayloadTag tag;
    TrafficCategory cat = TrafficCategory::Other;
    if (packet->PeekPacketTag(tag))
    {
        uint32_t ndnSize = tag.GetPayloadSize();
        uint32_t offset = (sz >= ndnSize) ? sz - ndnSize : 0;
        cat = Classify(buf.data() + offset, sz - offset);
    }

    uint32_t lpBytes = tag.GetPayloadSize();

    if (m_perPacket)
    {
        m_out << Simulator::Now().GetNanoSeconds() / 1e9
              << "," << kCategoryNames[static_cast<size_t>(cat)]
              << "," << lpBytes << "\n";
        return;
    }

    auto idx = static_cast<size_t>(cat);
    m_counters[idx].packets++;
    // Count LP-encoded packet bytes (from the tag), excluding L2 headers.
    // This matches emulation's UDP-payload accounting (UDP payload = LP pkt).
    m_counters[idx].bytes += lpBytes;
}

void
NdndLinkTracer::WriteStats()
{
    double t = Simulator::Now().GetSeconds();
    m_out << t;
    for (size_t i = 0; i < kNumCategories; ++i)
    {
        m_out << "," << m_counters[i].packets << "," << m_counters[i].bytes;
        m_counters[i].packets = 0;
        m_counters[i].bytes = 0;
    }
    m_out << "\n";
    ScheduleNext();
}

void
NdndLinkTracer::ScheduleNext()
{
    m_event = Simulator::Schedule(m_period, &NdndLinkTracer::WriteStats, this);
}

void
NdndLinkTracer::Stop()
{
    if (m_event.IsPending())
    {
        Simulator::Cancel(m_event);
    }
    if (m_out.is_open())
    {
        m_out.close();
    }
}

} // namespace ndndsim
} // namespace ns3
