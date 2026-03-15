/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndRateTracer implementation.
 */

#include "ndndsim-rate-tracer.h"

#include "ns3/config.h"
#include "ns3/log.h"
#include "ns3/node-list.h"
#include "ns3/node.h"
#include "ns3/simulator.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("NdndRateTracer");

namespace ndndsim
{

std::unique_ptr<NdndRateTracer> NdndRateTracer::s_instance = nullptr;

NdndRateTracer::NdndRateTracer(const std::string& file, Time period)
    : m_out(file),
      m_period(period)
{
    NS_ABORT_MSG_IF(!m_out.is_open(), "Cannot open tracer output file: " << file);
    m_out << "Time,Node,Type,Packets,KBytes" << std::endl;
}

NdndRateTracer::~NdndRateTracer()
{
    Simulator::Cancel(m_printEvent);
    if (m_out.is_open())
    {
        m_out.close();
    }
}

void
NdndRateTracer::InstallAll(const std::string& file, Time period)
{
    NodeContainer allNodes;
    for (uint32_t i = 0; i < NodeList::GetNNodes(); ++i)
    {
        allNodes.Add(NodeList::GetNode(i));
    }
    Install(allNodes, file, period);
}

void
NdndRateTracer::Install(NodeContainer nodes, const std::string& file, Time period)
{
    // Only one tracer instance at a time
    s_instance = std::unique_ptr<NdndRateTracer>(new NdndRateTracer(file, period));

    for (auto it = nodes.Begin(); it != nodes.End(); ++it)
    {
        s_instance->ConnectNode(*it);
    }

    s_instance->m_printEvent =
        Simulator::Schedule(period, &NdndRateTracer::PeriodicPrint, s_instance.get());
}

// Free-function callback wrappers (friends of NdndRateTracer)
void
InterestSentTraceCallback(NdndRateTracer* tracer, uint32_t nodeId, uint32_t seqNo)
{
    tracer->InterestSentCallback(nodeId, seqNo);
}

void
DataSentTraceCallback(NdndRateTracer* tracer, uint32_t nodeId, uint32_t payloadSize)
{
    tracer->DataSentCallback(nodeId, payloadSize);
}

void
DataReceivedTraceCallback(NdndRateTracer* tracer, uint32_t nodeId, uint32_t dataSize)
{
    tracer->DataReceivedCallback(nodeId, dataSize);
}

void
NdndRateTracer::ConnectNode(Ptr<Node> node)
{
    uint32_t nodeId = node->GetId();
    m_counters[nodeId] = NodeCounters{};

    std::string basePath = "/NodeList/" + std::to_string(nodeId) + "/ApplicationList/*/";

    // NdndConsumer
    std::string consumerPath = basePath + "$ns3::ndndsim::NdndConsumer";
    if (Config::LookupMatches(consumerPath).GetN() > 0)
    {
        Config::ConnectWithoutContext(
            consumerPath + "/InterestSent",
            MakeBoundCallback(&InterestSentTraceCallback, this, nodeId));
        Config::ConnectWithoutContext(
            consumerPath + "/DataReceived",
            MakeBoundCallback(&DataReceivedTraceCallback, this, nodeId));
    }

    // NdndConsumerZipf
    std::string zipfPath = basePath + "$ns3::ndndsim::NdndConsumerZipf";
    if (Config::LookupMatches(zipfPath).GetN() > 0)
    {
        Config::ConnectWithoutContext(
            zipfPath + "/InterestSent",
            MakeBoundCallback(&InterestSentTraceCallback, this, nodeId));
        Config::ConnectWithoutContext(
            zipfPath + "/DataReceived",
            MakeBoundCallback(&DataReceivedTraceCallback, this, nodeId));
    }

    // NdndProducer
    std::string producerPath = basePath + "$ns3::ndndsim::NdndProducer";
    if (Config::LookupMatches(producerPath).GetN() > 0)
    {
        Config::ConnectWithoutContext(
            producerPath + "/DataSent",
            MakeBoundCallback(&DataSentTraceCallback, this, nodeId));
    }
}

void
NdndRateTracer::InterestSentCallback(uint32_t nodeId, uint32_t seqNo)
{
    m_counters[nodeId].interests++;
}

void
NdndRateTracer::DataSentCallback(uint32_t nodeId, uint32_t payloadSize)
{
    m_counters[nodeId].data++;
    m_counters[nodeId].dataBytes += payloadSize;
}

void
NdndRateTracer::DataReceivedCallback(uint32_t nodeId, uint32_t dataSize)
{
    m_counters[nodeId].dataReceived++;
    m_counters[nodeId].dataReceivedBytes += dataSize;
}

void
NdndRateTracer::PeriodicPrint()
{
    double timeNow = Simulator::Now().GetSeconds();

    for (auto& [nodeId, counters] : m_counters)
    {
        if (counters.interests > 0)
        {
            m_out << timeNow << "," << nodeId << ",InterestSent," << counters.interests << ","
                  << 0 << std::endl;
        }
        if (counters.data > 0)
        {
            double kb = static_cast<double>(counters.dataBytes) / 1024.0;
            m_out << timeNow << "," << nodeId << ",DataSent," << counters.data << "," << kb
                  << std::endl;
        }
        if (counters.dataReceived > 0)
        {
            double kb = static_cast<double>(counters.dataReceivedBytes) / 1024.0;
            m_out << timeNow << "," << nodeId << ",DataReceived," << counters.dataReceived << ","
                  << kb << std::endl;
        }

        // Reset counters for next period
        counters.interests = 0;
        counters.data = 0;
        counters.dataBytes = 0;
        counters.dataReceived = 0;
        counters.dataReceivedBytes = 0;
    }

    m_printEvent = Simulator::Schedule(m_period, &NdndRateTracer::PeriodicPrint, this);
}

} // namespace ndndsim
} // namespace ns3
