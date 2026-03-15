/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndTrafficTracer: Track exact per-category packet counts from NDN
 * consumer/producer app trace sources.  Each "category" is a named
 * traffic class (e.g. "background", "event-driven") and gets its own
 * CSV output with per-interval counters.
 */

#ifndef NDNDSIM_TRAFFIC_TRACER_H
#define NDNDSIM_TRAFFIC_TRACER_H

#include "ns3/application-container.h"
#include "ns3/event-id.h"
#include "ns3/nstime.h"

#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace ns3
{
namespace ndndsim
{

/**
 * \brief Track exact per-category traffic from NDN app trace sources.
 *
 * Create named categories, connect consumer/producer apps to them,
 * and get periodic CSV output with exact packet and byte counts.
 *
 * CSV columns: Time,InterestsSent,DataReceived,DataSent,Bytes
 *
 * Usage:
 * \code
 *   auto tracer = NdndTrafficTracer::Create(Seconds(1.0));
 *
 *   // Register categories with output files
 *   tracer->AddCategory("background", "results/background.csv");
 *   tracer->AddCategory("event",      "results/event-traffic.csv");
 *
 *   // Connect apps
 *   tracer->ConnectConsumer("background", consumerApps);
 *   tracer->ConnectProducer("background", producerApps);
 *   tracer->ConnectConsumer("event", eventConsumerApps);
 *   tracer->ConnectProducer("event", eventProducerApps);
 *
 *   // ... run simulation ...
 *   tracer->Stop();  // flush and close files
 * \endcode
 */
class NdndTrafficTracer
{
  public:
    /**
     * Create a traffic tracer with the given sampling period.
     *
     * \param period  reporting interval (e.g. Seconds(1.0))
     * \return shared pointer to the tracer instance
     */
    static std::shared_ptr<NdndTrafficTracer> Create(Time period);

    ~NdndTrafficTracer();

    /**
     * Register a named traffic category with its output CSV file.
     *
     * \param name     category name (must be unique)
     * \param csvPath  output file path (directories must exist)
     */
    void AddCategory(const std::string& name, const std::string& csvPath);

    /**
     * Connect NdndConsumer apps to a category.
     * Hooks InterestSent and DataReceived trace sources.
     */
    void ConnectConsumer(const std::string& category,
                         ApplicationContainer& apps);

    /**
     * Connect NdndProducer apps to a category.
     * Hooks DataSent trace source.
     */
    void ConnectProducer(const std::string& category,
                         ApplicationContainer& apps);

    /**
     * Flush remaining counters and close all output files.
     * Call after Simulator::Run() returns.
     */
    void Stop();

  private:
    explicit NdndTrafficTracer(Time period);

    struct Counters
    {
        uint64_t interestsSent = 0;
        uint64_t dataReceived = 0;
        uint64_t dataSent = 0;
        uint64_t dataBytes = 0;

        uint64_t prevInterestsSent = 0;
        uint64_t prevDataReceived = 0;
        uint64_t prevDataSent = 0;
        uint64_t prevDataBytes = 0;

        void Snapshot(uint64_t& dI, uint64_t& dDRx,
                      uint64_t& dDTx, uint64_t& dB);
    };

    struct Category
    {
        std::string name;
        Counters counters;
        std::ofstream out;
    };

    void Sample();
    void ScheduleNext();

    Category& GetCategory(const std::string& name);

    Time m_period;
    EventId m_sampleEvent;
    std::map<std::string, std::unique_ptr<Category>> m_categories;
    bool m_stopped = false;
};

} // namespace ndndsim
} // namespace ns3

#endif /* NDNDSIM_TRAFFIC_TRACER_H */
