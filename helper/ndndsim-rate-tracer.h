/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndRateTracer: Connects to Interest/Data trace sources and writes
 * periodic rate statistics to a CSV file.
 */

#ifndef NDNDSIM_RATE_TRACER_H
#define NDNDSIM_RATE_TRACER_H

#include "ns3/event-id.h"
#include "ns3/node-container.h"
#include "ns3/nstime.h"
#include "ns3/ptr.h"

#include <fstream>
#include <map>
#include <memory>
#include <string>

namespace ns3
{
namespace ndndsim
{

/**
 * \brief Periodically logs per-node Interest/Data rates to a CSV file.
 *
 * Attaches to the InterestSent and DataSent trace sources on consumer
 * and producer applications. Every `period` seconds, writes a row per
 * node with the count of packets since the last period.
 *
 * CSV columns: Time, Node, Type, Packets, Kilobytes (estimated)
 *
 * Usage:
 * \code
 *   NdndRateTracer::InstallAll("rate-trace.csv", Seconds(0.5));
 * \endcode
 */
class NdndRateTracer
{
  public:
    /**
     * Install tracers on all nodes and write to the given file.
     *
     * \param file output CSV file path
     * \param period reporting interval
     */
    static void InstallAll(const std::string& file, Time period);

    /**
     * Install tracers on specific nodes.
     */
    static void Install(NodeContainer nodes, const std::string& file, Time period);

    ~NdndRateTracer();

  private:
    NdndRateTracer(const std::string& file, Time period);

    void ConnectNode(Ptr<Node> node);
    void InterestSentCallback(uint32_t nodeId, uint32_t seqNo);
    void DataSentCallback(uint32_t nodeId, uint32_t payloadSize);
    void DataReceivedCallback(uint32_t nodeId, uint32_t dataSize);
    void PeriodicPrint();

    friend void InterestSentTraceCallback(NdndRateTracer*, uint32_t, uint32_t);
    friend void DataSentTraceCallback(NdndRateTracer*, uint32_t, uint32_t);
    friend void DataReceivedTraceCallback(NdndRateTracer*, uint32_t, uint32_t);

    struct NodeCounters
    {
        uint32_t interests = 0;
        uint32_t data = 0;
        uint64_t dataBytes = 0;
        uint32_t dataReceived = 0;
        uint64_t dataReceivedBytes = 0;
    };

    std::ofstream m_out;
    Time m_period;
    EventId m_printEvent;
    std::map<uint32_t, NodeCounters> m_counters;

    static std::unique_ptr<NdndRateTracer> s_instance;
};

} // namespace ndndsim
} // namespace ns3

#endif /* NDNDSIM_RATE_TRACER_H */
