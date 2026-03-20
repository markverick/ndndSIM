/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndConsumer implementation.
 *
 * Delegates Interest encoding and sending to the Go-side NDNd engine via
 * NdndSimRegisterConsumer, ensuring identical wire format between sim and emu.
 */

#include "ndndsim-consumer.h"

#include "../model/ndndsim-go-bridge.h"
#include "../model/ndndsim-stack.h"

#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/double.h"

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
            .AddAttribute("LifeTime",
                           "Interest lifetime",
                           TimeValue(MilliSeconds(4000)),
                           MakeTimeAccessor(&NdndConsumer::m_lifetime),
                           MakeTimeChecker())
            .AddTraceSource("InterestSent",
                             "Trace fired when an Interest is sent",
                             MakeTraceSourceAccessor(&NdndConsumer::m_interestSentTrace),
                             "ns3::ndndsim::NdndConsumer::InterestSentCallback")
            .AddTraceSource("DataReceived",
                             "Trace fired when Data is received from the network",
                             MakeTraceSourceAccessor(&NdndConsumer::m_dataReceivedTrace),
                             "ns3::ndndsim::NdndConsumer::DataReceivedCallback");
    return tid;
}

NdndConsumer::NdndConsumer()
    : m_frequency(1.0),
      m_lifetime(MilliSeconds(4000))
{
}

NdndConsumer::~NdndConsumer()
{
}

void
NdndConsumer::OnStart()
{
    NS_LOG_FUNCTION(this);
    NS_LOG_INFO("Consumer starting on node " << GetNode()->GetId()
                                              << " prefix=" << m_prefix
                                              << " freq=" << m_frequency);

    // Register for Data received notifications from the Go bridge
    std::string prefix = m_prefix;
    RegisterDataReceivedCallback(GetNode()->GetId(),
        [this, prefix](uint32_t dataSize, const std::string& dataName) {
            if (dataName.rfind(prefix, 0) != 0)
            {
                return;
            }
            NS_LOG_INFO("t=" << Simulator::Now().GetSeconds() << "s node="
                        << GetNode()->GetId() << " Received Data (" << dataSize << " bytes)");
            m_dataReceivedTrace(dataSize);
        });

    // Delegate Interest encoding and scheduling entirely to the Go side.
    // NdndSimRegisterConsumer uses engine.Spec().MakeInterest() -- identical
    // wire format to the emu Go consumer.
    uint32_t lifetimeMs = static_cast<uint32_t>(m_lifetime.GetMilliSeconds());
    std::string prefix_copy = m_prefix;
    int ret = NdndSimRegisterConsumer(
        GetNode()->GetId(),
        const_cast<char*>(prefix_copy.c_str()),
        static_cast<int>(prefix_copy.size()),
        m_frequency,
        lifetimeMs);

    NS_ABORT_MSG_IF(ret != 0,
                    "NdndSimRegisterConsumer failed on node " << GetNode()->GetId()
                    << " (prefix=" << m_prefix << ")"
                    << " — NDNd stack must be initialized before consumer app starts");
}

void
NdndConsumer::OnStop()
{
    NS_LOG_FUNCTION(this);
    NdndSimStopConsumer(GetNode()->GetId());
}

} // namespace ndndsim
} // namespace ns3
