/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndConsumerZipf implementation.
 */

#include "ndndsim-consumer-zipf.h"

#include "../model/ndndsim-go-bridge.h"
#include "../model/ndndsim-stack.h"

#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <sstream>
#include <vector>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("NdndConsumerZipf");

namespace ndndsim
{

NS_OBJECT_ENSURE_REGISTERED(NdndConsumerZipf);

TypeId
NdndConsumerZipf::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::ndndsim::NdndConsumerZipf")
            .SetParent<NdndApp>()
            .SetGroupName("NdndSIM")
            .AddConstructor<NdndConsumerZipf>()
            .AddAttribute("Prefix",
                           "NDN name prefix to request",
                           StringValue("/ndn/test"),
                           MakeStringAccessor(&NdndConsumerZipf::m_prefix),
                           MakeStringChecker())
            .AddAttribute("Frequency",
                           "Interest sending frequency in Hz",
                           DoubleValue(1.0),
                           MakeDoubleAccessor(&NdndConsumerZipf::m_frequency),
                           MakeDoubleChecker<double>(0.0))
            .AddAttribute("NumberOfContents",
                           "Number of distinct content items in the catalog",
                           UintegerValue(100),
                           MakeUintegerAccessor(&NdndConsumerZipf::m_numContents),
                           MakeUintegerChecker<uint32_t>(1))
            .AddAttribute("q",
                           "Zipf-Mandelbrot q parameter",
                           DoubleValue(0.0),
                           MakeDoubleAccessor(&NdndConsumerZipf::m_q),
                           MakeDoubleChecker<double>(0.0))
            .AddAttribute("s",
                           "Zipf-Mandelbrot s parameter (exponent)",
                           DoubleValue(0.7),
                           MakeDoubleAccessor(&NdndConsumerZipf::m_s),
                           MakeDoubleChecker<double>(0.0))
            .AddTraceSource("InterestSent",
                             "Trace fired when an Interest is sent",
                             MakeTraceSourceAccessor(&NdndConsumerZipf::m_interestSentTrace),
                             "ns3::ndndsim::NdndConsumerZipf::InterestSentCallback")
            .AddTraceSource("DataReceived",
                             "Trace fired when Data is received from the network",
                             MakeTraceSourceAccessor(&NdndConsumerZipf::m_dataReceivedTrace),
                             "ns3::ndndsim::NdndConsumerZipf::DataReceivedCallback");
    return tid;
}

NdndConsumerZipf::NdndConsumerZipf()
    : m_frequency(1.0),
      m_numContents(100),
      m_q(0.0),
      m_s(0.7)
{
    m_rand = CreateObject<UniformRandomVariable>();
}

NdndConsumerZipf::~NdndConsumerZipf()
{
}

void
NdndConsumerZipf::SetNumberOfContents(uint32_t numOfContents)
{
    m_numContents = numOfContents;

    // Precompute CDF for Zipf-Mandelbrot distribution
    // P(k) = 1/(k+q)^s / sum_{i=1}^{N} 1/(i+q)^s
    m_cdf.resize(m_numContents + 1);
    m_cdf[0] = 0.0;

    double sum = 0.0;
    for (uint32_t k = 1; k <= m_numContents; ++k)
    {
        sum += 1.0 / std::pow(static_cast<double>(k) + m_q, m_s);
        m_cdf[k] = sum;
    }

    // Normalize to [0, 1]
    for (uint32_t k = 1; k <= m_numContents; ++k)
    {
        m_cdf[k] /= sum;
    }

    NS_LOG_DEBUG("Zipf CDF computed for " << m_numContents << " contents, q=" << m_q
                                           << ", s=" << m_s);
}

uint32_t
NdndConsumerZipf::GetNextSeqNo()
{
    // Sample from the precomputed CDF using inverse-transform sampling
    double u = m_rand->GetValue();

    // Binary search for the content index
    uint32_t lo = 1;
    uint32_t hi = m_numContents;
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2;
        if (m_cdf[mid] < u)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    return lo - 1; // 0-based content ID
}

// ─── TLV encoding (same as NdndConsumer) ───────────────────────────

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
    components.push_back(std::to_string(seqNo));

    std::vector<uint8_t> nameValue;
    for (const auto& comp : components)
    {
        auto encoded = EncodeNameComponent(8, comp);
        nameValue.insert(nameValue.end(), encoded.begin(), encoded.end());
    }

    std::vector<uint8_t> nameTlv;
    auto nameType = EncodeTlvVarNum(7);
    auto nameLen = EncodeTlvVarNum(nameValue.size());
    nameTlv.insert(nameTlv.end(), nameType.begin(), nameType.end());
    nameTlv.insert(nameTlv.end(), nameLen.begin(), nameLen.end());
    nameTlv.insert(nameTlv.end(), nameValue.begin(), nameValue.end());

    std::vector<uint8_t> nonceTlv;
    nonceTlv.push_back(10);
    nonceTlv.push_back(4);
    static std::mt19937 rng(42);
    uint32_t nonce = rng();
    nonceTlv.push_back(static_cast<uint8_t>((nonce >> 24) & 0xFF));
    nonceTlv.push_back(static_cast<uint8_t>((nonce >> 16) & 0xFF));
    nonceTlv.push_back(static_cast<uint8_t>((nonce >> 8) & 0xFF));
    nonceTlv.push_back(static_cast<uint8_t>(nonce & 0xFF));

    std::vector<uint8_t> lifetimeTlv;
    lifetimeTlv.push_back(12);
    lifetimeTlv.push_back(2);
    lifetimeTlv.push_back(0x0F);
    lifetimeTlv.push_back(0xA0);

    std::vector<uint8_t> interestValue;
    interestValue.insert(interestValue.end(), nameTlv.begin(), nameTlv.end());
    interestValue.insert(interestValue.end(), nonceTlv.begin(), nonceTlv.end());
    interestValue.insert(interestValue.end(), lifetimeTlv.begin(), lifetimeTlv.end());

    std::vector<uint8_t> interest;
    auto intType = EncodeTlvVarNum(5);
    auto intLen = EncodeTlvVarNum(interestValue.size());
    interest.insert(interest.end(), intType.begin(), intType.end());
    interest.insert(interest.end(), intLen.begin(), intLen.end());
    interest.insert(interest.end(), interestValue.begin(), interestValue.end());

    return interest;
}

// ─── Application lifecycle ─────────────────────────────────────────

void
NdndConsumerZipf::OnStart()
{
    NS_LOG_FUNCTION(this);

    // Build the CDF now that all attributes are set
    SetNumberOfContents(m_numContents);

    NS_LOG_INFO("ConsumerZipf starting on node " << GetNode()->GetId()
                                                   << " prefix=" << m_prefix
                                                   << " freq=" << m_frequency
                                                   << " N=" << m_numContents
                                                   << " q=" << m_q << " s=" << m_s);

    // Register for Data received notifications from the Go bridge
    std::string prefix = m_prefix;
    RegisterDataReceivedCallback(GetNode()->GetId(),
        [this, prefix](uint32_t dataSize, const std::string& dataName) {
            // Only count Data matching our application prefix
            if (dataName.rfind(prefix, 0) != 0)
            {
                return;
            }
            NS_LOG_INFO("t=" << Simulator::Now().GetSeconds() << "s node="
                        << GetNode()->GetId() << " Received Data (" << dataSize << " bytes)");
            m_dataReceivedTrace(dataSize);
        });

    SendInterest();
}

void
NdndConsumerZipf::OnStop()
{
    NS_LOG_FUNCTION(this);
    Simulator::Cancel(m_sendEvent);
}

void
NdndConsumerZipf::SendInterest()
{
    Ptr<NdndStack> stack = GetStack();
    if (!stack)
    {
        NS_LOG_WARN("No NDNd stack on this node");
        return;
    }

    uint32_t seqNo = GetNextSeqNo();

    auto wire = EncodeInterest(m_prefix, seqNo);
    NS_LOG_INFO("t=" << Simulator::Now().GetSeconds() << "s node="
                << GetNode()->GetId() << " Sending Interest " << m_prefix << "/" << seqNo
                << " (" << wire.size() << " bytes)");

    NdndSimReceivePacket(GetNode()->GetId(), UINT32_MAX, wire.data(), wire.size());

    m_interestSentTrace(seqNo);

    if (m_frequency > 0)
    {
        m_sendEvent = Simulator::Schedule(Seconds(1.0 / m_frequency),
                                           &NdndConsumerZipf::SendInterest,
                                           this);
    }
}

} // namespace ndndsim
} // namespace ns3
