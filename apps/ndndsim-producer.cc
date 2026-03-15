/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndProducer implementation.
 *
 * The producer registers a route in the NDNd forwarder FIB for its prefix.
 * When an Interest arrives at this node, NDNd's forwarder will find
 * the route pointing to the local application face and deliver it.
 *
 * Note: In the current architecture, Interest handling and Data generation
 * happen on the Go side. The C++ producer sets up the FIB route so
 * that Interests reach the correct node's forwarder.
 */

#include "ndndsim-producer.h"

#include "../model/ndndsim-go-bridge.h"
#include "../model/ndndsim-stack.h"

#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("NdndProducer");

namespace ndndsim
{

NS_OBJECT_ENSURE_REGISTERED(NdndProducer);

TypeId
NdndProducer::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::ndndsim::NdndProducer")
            .SetParent<NdndApp>()
            .SetGroupName("NdndSIM")
            .AddConstructor<NdndProducer>()
            .AddAttribute("Prefix",
                           "NDN name prefix to serve",
                           StringValue("/ndn/test"),
                           MakeStringAccessor(&NdndProducer::m_prefix),
                           MakeStringChecker())
            .AddAttribute("PayloadSize",
                           "Size of Data payload in bytes",
                           UintegerValue(1024),
                           MakeUintegerAccessor(&NdndProducer::m_payloadSize),
                           MakeUintegerChecker<uint32_t>())
            .AddAttribute("Freshness",
                           "FreshnessPeriod of produced Data",
                           TimeValue(MilliSeconds(0)),
                           MakeTimeAccessor(&NdndProducer::m_freshness),
                           MakeTimeChecker())
            .AddTraceSource("DataSent",
                             "Trace fired when Data is sent",
                             MakeTraceSourceAccessor(&NdndProducer::m_dataSentTrace),
                             "ns3::ndndsim::NdndProducer::DataSentCallback");
    return tid;
}

NdndProducer::NdndProducer()
    : m_payloadSize(1024),
      m_freshness(MilliSeconds(0))
{
}

NdndProducer::~NdndProducer()
{
}

void
NdndProducer::OnStart()
{
    NS_LOG_FUNCTION(this);

    Ptr<NdndStack> stack = GetStack();
    NS_ASSERT_MSG(stack, "No NDNd stack installed on this node");

    NS_LOG_INFO("Producer starting on node " << GetNode()->GetId()
                                              << " prefix=" << m_prefix
                                              << " payload=" << m_payloadSize);

    // Register the prefix route to the internal application face.
    uint64_t appFace = NdndSimGetAppFaceId(GetNode()->GetId());
    stack->AddRoute(m_prefix, appFace, 0);

    // Register a Go-side Interest handler that generates Data replies.
    uint32_t freshnessMs = static_cast<uint32_t>(m_freshness.GetMilliSeconds());
    int rc = NdndSimRegisterProducer(GetNode()->GetId(),
                                      const_cast<char*>(m_prefix.c_str()),
                                      static_cast<int>(m_prefix.size()),
                                      m_payloadSize,
                                      freshnessMs);
    NS_ASSERT_MSG(rc == 0, "Failed to register producer on Go side");

    // Register for Data produced notifications from the Go bridge
    RegisterDataProducedCallback(GetNode()->GetId(),
        [this](uint32_t dataSize) {
            NS_LOG_INFO("t=" << Simulator::Now().GetSeconds() << "s node="
                        << GetNode()->GetId() << " Produced Data (" << dataSize << " bytes)");
            m_dataSentTrace(dataSize);
        });

    NS_LOG_INFO("Registered route " << m_prefix << " → face " << appFace
                                     << " on node " << GetNode()->GetId());
}

void
NdndProducer::OnStop()
{
    NS_LOG_FUNCTION(this);

    Ptr<NdndStack> stack = GetStack();
    if (stack)
    {
        uint64_t appFace = NdndSimGetAppFaceId(GetNode()->GetId());
        stack->RemoveRoute(m_prefix, appFace);
    }
}

} // namespace ndndsim
} // namespace ns3
