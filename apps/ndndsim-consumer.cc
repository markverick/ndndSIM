/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndConsumer implementation.
 *
 * This consumer sends Interests for <prefix>/<seqno> and demonstrates how
 * Go-side NDNd processes and forwards them. Since the Interest encoding
 * and forwarding happen on the Go side, the C++ consumer constructs
 * a raw TLV Interest and delivers it to the NDNd stack.
 */

#include "ndndsim-consumer.h"

#include "../model/ndndsim-go-bridge.h"
#include "../model/ndndsim-stack.h"

#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/double.h"
#include "ns3/uinteger.h"

#include <cstdint>
#include <cstring>
#include <random>
#include <sstream>
#include <vector>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("NdndConsumer");

namespace ndndsim
{

NS_OBJECT_ENSURE_REGISTERED(NdndConsumer);

TypeId
NdndConsumer::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::ndndsim::NdndConsumer")
            .SetParent<NdndApp>()
            .SetGroupName("NdndSIM")
            .AddConstructor<NdndConsumer>()
            .AddAttribute("Prefix",
                           "NDN name prefix to request",
                           StringValue("/ndn/test"),
                           MakeStringAccessor(&NdndConsumer::m_prefix),
                           MakeStringChecker())
            .AddAttribute("Frequency",
                           "Interest sending frequency in Hz",
                           DoubleValue(1.0),
                           MakeDoubleAccessor(&NdndConsumer::m_frequency),
                           MakeDoubleChecker<double>(0.0))
            .AddTraceSource("InterestSent",
                             "Trace fired when an Interest is sent",
                             MakeTraceSourceAccessor(&NdndConsumer::m_interestSentTrace),
                             "ns3::ndndsim::NdndConsumer::InterestSentCallback");
    return tid;
}

NdndConsumer::NdndConsumer()
    : m_frequency(1.0),
      m_seqNo(0)
{
}

NdndConsumer::~NdndConsumer()
{
}

/**
 * Encode a minimal NDN Interest TLV packet.
 *
 * TLV format:
 *   Interest = INTEREST-TYPE TLV-LENGTH
 *     Name = NAME-TYPE TLV-LENGTH
 *       GenericNameComponent = GENERIC-NAME-COMPONENT-TYPE TLV-LENGTH BYTE+
 *       ...
 *     Nonce = NONCE-TYPE TLV-LENGTH BYTE{4}
 *     InterestLifetime = INTEREST-LIFETIME-TYPE TLV-LENGTH NonNegativeInteger
 */
static std::vector<uint8_t>
EncodeTlvVarNum(uint64_t val)
{
    std::vector<uint8_t> out;
    if (val < 253)
    {
        out.push_back(static_cast<uint8_t>(val));
    }
    else if (val <= 0xFFFF)
    {
        out.push_back(253);
        out.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(val & 0xFF));
    }
    else if (val <= 0xFFFFFFFF)
    {
        out.push_back(254);
        out.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(val & 0xFF));
    }
    return out;
}

static std::vector<uint8_t>
EncodeNameComponent(uint32_t type, const std::string& value)
{
    std::vector<uint8_t> out;
    auto typeBytes = EncodeTlvVarNum(type);
    auto lenBytes = EncodeTlvVarNum(value.size());
    out.insert(out.end(), typeBytes.begin(), typeBytes.end());
    out.insert(out.end(), lenBytes.begin(), lenBytes.end());
    out.insert(out.end(), value.begin(), value.end());
    return out;
}

static std::vector<uint8_t>
EncodeInterest(const std::string& prefix, uint32_t seqNo)
{
    // Parse prefix into components
    std::vector<std::string> components;
    std::istringstream ss(prefix);
    std::string token;
    while (std::getline(ss, token, '/'))
    {
        if (!token.empty())
        {
            components.push_back(token);
        }
    }
    // Add sequence number component
    components.push_back(std::to_string(seqNo));

    // Encode Name
    std::vector<uint8_t> nameValue;
    for (const auto& comp : components)
    {
        auto encoded = EncodeNameComponent(8, comp); // 8 = GenericNameComponent
        nameValue.insert(nameValue.end(), encoded.begin(), encoded.end());
    }

    // Name TLV
    std::vector<uint8_t> nameTlv;
    auto nameType = EncodeTlvVarNum(7); // 7 = Name
    auto nameLen = EncodeTlvVarNum(nameValue.size());
    nameTlv.insert(nameTlv.end(), nameType.begin(), nameType.end());
    nameTlv.insert(nameTlv.end(), nameLen.begin(), nameLen.end());
    nameTlv.insert(nameTlv.end(), nameValue.begin(), nameValue.end());

    // Nonce TLV (random 4 bytes)
    std::vector<uint8_t> nonceTlv;
    nonceTlv.push_back(10); // Nonce type
    nonceTlv.push_back(4);  // Length
    static std::mt19937 rng(42);
    uint32_t nonce = rng();
    nonceTlv.push_back(static_cast<uint8_t>((nonce >> 24) & 0xFF));
    nonceTlv.push_back(static_cast<uint8_t>((nonce >> 16) & 0xFF));
    nonceTlv.push_back(static_cast<uint8_t>((nonce >> 8) & 0xFF));
    nonceTlv.push_back(static_cast<uint8_t>(nonce & 0xFF));

    // InterestLifetime TLV (4000ms = 4 bytes NonNegativeInteger)
    std::vector<uint8_t> lifetimeTlv;
    lifetimeTlv.push_back(12); // InterestLifetime type
    lifetimeTlv.push_back(2);  // Length (2 bytes)
    lifetimeTlv.push_back(0x0F); // 4000 = 0x0FA0
    lifetimeTlv.push_back(0xA0);

    // Interest TLV
    std::vector<uint8_t> interestValue;
    interestValue.insert(interestValue.end(), nameTlv.begin(), nameTlv.end());
    interestValue.insert(interestValue.end(), nonceTlv.begin(), nonceTlv.end());
    interestValue.insert(interestValue.end(), lifetimeTlv.begin(), lifetimeTlv.end());

    std::vector<uint8_t> interest;
    auto intType = EncodeTlvVarNum(5); // 5 = Interest
    auto intLen = EncodeTlvVarNum(interestValue.size());
    interest.insert(interest.end(), intType.begin(), intType.end());
    interest.insert(interest.end(), intLen.begin(), intLen.end());
    interest.insert(interest.end(), interestValue.begin(), interestValue.end());

    return interest;
}

void
NdndConsumer::OnStart()
{
    NS_LOG_FUNCTION(this);
    NS_LOG_INFO("Consumer starting on node " << GetNode()->GetId()
                                              << " prefix=" << m_prefix
                                              << " freq=" << m_frequency);
    SendInterest();
}

void
NdndConsumer::OnStop()
{
    NS_LOG_FUNCTION(this);
    Simulator::Cancel(m_sendEvent);
}

void
NdndConsumer::SendInterest()
{
    Ptr<NdndStack> stack = GetStack();
    if (!stack)
    {
        NS_LOG_WARN("No NDNd stack on this node");
        return;
    }

    // Encode and inject Interest into the local forwarder
    auto wire = EncodeInterest(m_prefix, m_seqNo);
    NS_LOG_INFO("Sending Interest " << m_prefix << "/" << m_seqNo
                                     << " (" << wire.size() << " bytes)");

    // Deliver to the node's internal app face (face 1)
    NdndSimReceivePacket(GetNode()->GetId(), UINT32_MAX, wire.data(), wire.size());

    m_interestSentTrace(m_seqNo);
    m_seqNo++;

    // Schedule next Interest
    if (m_frequency > 0)
    {
        m_sendEvent = Simulator::Schedule(Seconds(1.0 / m_frequency),
                                           &NdndConsumer::SendInterest, this);
    }
}

} // namespace ndndsim
} // namespace ns3
