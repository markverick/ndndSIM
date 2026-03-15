/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndTrafficTracer implementation.
 */

#include "ndndsim-traffic-tracer.h"

#include "ns3/log.h"
#include "ns3/simulator.h"

#include <stdexcept>

NS_LOG_COMPONENT_DEFINE("ndndsim.TrafficTracer");

namespace ns3
{
namespace ndndsim
{

// ─── Counters ──────────────────────────────────────────────────────

void
NdndTrafficTracer::Counters::Snapshot(uint64_t& dI, uint64_t& dDRx,
                                      uint64_t& dDTx, uint64_t& dB)
{
    dI = interestsSent - prevInterestsSent;
    dDRx = dataReceived - prevDataReceived;
    dDTx = dataSent - prevDataSent;
    dB = dataBytes - prevDataBytes;
    prevInterestsSent = interestsSent;
    prevDataReceived = dataReceived;
    prevDataSent = dataSent;
    prevDataBytes = dataBytes;
}

// ─── Construction / lifetime ───────────────────────────────────────

NdndTrafficTracer::NdndTrafficTracer(Time period)
    : m_period(period)
{
}

NdndTrafficTracer::~NdndTrafficTracer()
{
    if (!m_stopped)
    {
        Stop();
    }
}

std::shared_ptr<NdndTrafficTracer>
NdndTrafficTracer::Create(Time period)
{
    // Can't use make_shared because constructor is private
    return std::shared_ptr<NdndTrafficTracer>(new NdndTrafficTracer(period));
}

// ─── Category management ──────────────────────────────────────────

void
NdndTrafficTracer::AddCategory(const std::string& name,
                                const std::string& csvPath)
{
    if (m_categories.count(name))
    {
        NS_FATAL_ERROR("NdndTrafficTracer: duplicate category '" << name << "'");
    }

    auto cat = std::make_unique<Category>();
    cat->name = name;
    cat->out.open(csvPath);
    if (!cat->out.is_open())
    {
        NS_FATAL_ERROR("NdndTrafficTracer: cannot open '" << csvPath << "'");
    }
    cat->out << "Time,InterestsSent,DataReceived,DataSent,Bytes\n";
    m_categories[name] = std::move(cat);

    // Start sampling on first category
    if (m_categories.size() == 1)
    {
        ScheduleNext();
    }
}

NdndTrafficTracer::Category&
NdndTrafficTracer::GetCategory(const std::string& name)
{
    auto it = m_categories.find(name);
    if (it == m_categories.end())
    {
        NS_FATAL_ERROR("NdndTrafficTracer: unknown category '" << name << "'");
    }
    return *it->second;
}

// ─── Trace source connections ──────────────────────────────────────

void
NdndTrafficTracer::ConnectConsumer(const std::string& category,
                                    ApplicationContainer& apps)
{
    Category& cat = GetCategory(category);
    Counters* c = &cat.counters;

    for (uint32_t i = 0; i < apps.GetN(); ++i)
    {
        apps.Get(i)->TraceConnectWithoutContext(
            "InterestSent",
            MakeBoundCallback(
                +[](Counters* ctr, uint32_t) { ctr->interestsSent++; }, c));
        apps.Get(i)->TraceConnectWithoutContext(
            "DataReceived",
            MakeBoundCallback(
                +[](Counters* ctr, uint32_t sz) {
                    ctr->dataReceived++;
                    ctr->dataBytes += sz;
                },
                c));
    }
}

void
NdndTrafficTracer::ConnectProducer(const std::string& category,
                                    ApplicationContainer& apps)
{
    Category& cat = GetCategory(category);
    Counters* c = &cat.counters;

    for (uint32_t i = 0; i < apps.GetN(); ++i)
    {
        apps.Get(i)->TraceConnectWithoutContext(
            "DataSent",
            MakeBoundCallback(
                +[](Counters* ctr, uint32_t sz) {
                    ctr->dataSent++;
                    ctr->dataBytes += sz;
                },
                c));
    }
}

// ─── Periodic sampling ─────────────────────────────────────────────

void
NdndTrafficTracer::Sample()
{
    double t = Simulator::Now().GetSeconds();

    for (auto& [name, cat] : m_categories)
    {
        uint64_t dI, dDRx, dDTx, dB;
        cat->counters.Snapshot(dI, dDRx, dDTx, dB);

        cat->out << t << "," << dI << "," << dDRx << ","
                 << dDTx << "," << dB << "\n";
    }

    ScheduleNext();
}

void
NdndTrafficTracer::ScheduleNext()
{
    m_sampleEvent = Simulator::Schedule(m_period, &NdndTrafficTracer::Sample, this);
}

void
NdndTrafficTracer::Stop()
{
    m_stopped = true;
    Simulator::Cancel(m_sampleEvent);
    for (auto& [name, cat] : m_categories)
    {
        cat->out.flush();
        cat->out.close();
    }
}

} // namespace ndndsim
} // namespace ns3
