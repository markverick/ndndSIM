/*
 * ndndsim-atlas-scenario.cc — General-purpose atlas scenario
 *
 * Reads a topology file, installs NDNd with DV routing, places a
 * consumer and producer, and writes a rate trace CSV.
 *
 * Convergence detection: periodically polls each node's RIB entry count.
 * When every node has >= numNodes entries, DV has converged.  The
 * convergence time is written to a one-line file (--convTrace).
 *
 * All parameters are configurable via command line — no need to
 * regenerate C++ source for different topologies or settings.
 *
 * Usage:
 *   ./ns3 run "ndndsim-atlas-scenario --topo=path/to/topo.txt"
 *   ./ns3 run "ndndsim-atlas-scenario --topo=... --consumer=a --producer=c --simTime=20"
 */

#include "ns3/core-module.h"
#include "ns3/names.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

// ndndSIM headers
#include "ns3/ndndsim-app-helper.h"
#include "ns3/ndndsim-link-tracer.h"
#include "ns3/ndndsim-rate-tracer.h"
#include "ns3/ndndsim-stack-helper.h"
#include "ns3/ndndsim-stack.h"
#include "ns3/ndndsim-topology-reader.h"

#include <fstream>

using namespace ns3;
using namespace ns3::ndndsim;

// ─── Convergence checker ───────────────────────────────────────────

static bool g_converged = false;
static double g_convTime = -1.0;

static void
CheckConvergence(NodeContainer nodes, uint32_t numNodes, double dvStartTime,
                 double interval, std::string network)
{
    if (g_converged)
    {
        return;
    }

    bool allDone = true;
    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        Ptr<NdndStack> stack = nodes.Get(i)->GetObject<NdndStack>();
        if (!stack || static_cast<uint32_t>(stack->GetRibEntryCount(network)) < numNodes)
        {
            allDone = false;
            break;
        }
    }

    if (allDone)
    {
        g_converged = true;
        g_convTime = Simulator::Now().GetSeconds() - dvStartTime;
    }
    else
    {
        Simulator::Schedule(Seconds(interval),
                            &CheckConvergence, nodes, numNodes, dvStartTime, interval, network);
    }
}

int
main(int argc, char* argv[])
{
    std::string topoFile;
    std::string consumerName;
    std::string producerName;
    std::string prefix = "/ndn/test";
    std::string rateTrace = "rate-trace.csv";
    std::string linkTrace;
    std::string convTrace;
    std::string dvConfig;
    std::string network = "/minindn";
    double simTime = 60.0;
    double frequency = 10.0;
    double traceInterval = 0.05; // 50 ms

    CommandLine cmd;
    cmd.AddValue("topo", "Topology file path (required)", topoFile);
    cmd.AddValue("consumer", "Consumer node name (default: first node)", consumerName);
    cmd.AddValue("producer", "Producer node name (default: last node)", producerName);
    cmd.AddValue("prefix", "NDN name prefix", prefix);
    cmd.AddValue("rateTrace", "Output rate trace CSV path", rateTrace);
    cmd.AddValue("linkTrace", "Output link traffic CSV path", linkTrace);
    cmd.AddValue("convTrace", "Output convergence time file path", convTrace);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.AddValue("frequency", "Consumer Interest frequency (Hz)", frequency);
    cmd.AddValue("traceInterval", "Rate trace sampling interval in seconds", traceInterval);
    cmd.AddValue("dvConfig", "DV config JSON overlay (overrides defaults)", dvConfig);
    cmd.AddValue("network", "DV network prefix (default: /minindn)", network);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_IF(topoFile.empty(), "--topo is required");

    // ─── Topology ──────────────────────────────────────────────────

    NdndTopologyReader reader;
    reader.SetFileName(topoFile);
    NodeContainer nodes = reader.Read();
    NS_ABORT_MSG_IF(nodes.GetN() == 0, "No nodes read from topology file");

    // Resolve consumer/producer — default to first/last node
    Ptr<Node> consumer;
    Ptr<Node> producer;

    if (consumerName.empty())
    {
        consumer = nodes.Get(0);
    }
    else
    {
        consumer = Names::Find<Node>(consumerName);
        NS_ABORT_MSG_IF(!consumer, "Consumer node not found: " << consumerName);
    }

    if (producerName.empty())
    {
        producer = nodes.Get(nodes.GetN() - 1);
    }
    else
    {
        producer = Names::Find<Node>(producerName);
        NS_ABORT_MSG_IF(!producer, "Producer node not found: " << producerName);
    }

    // ─── NDNd Stack + DV Routing ───────────────────────────────────

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);
    NdndStackHelper::EnableDvRouting(network, nodes, dvConfig);

    // ─── Convergence Detection ─────────────────────────────────────

    // DV starts immediately at t=0 when EnableDvRouting is called.
    // Schedule periodic checks starting at t=traceInterval.
    double dvStartTime = 0.0;
    std::string dvNetwork = network;
    Simulator::Schedule(Seconds(traceInterval),
                        &CheckConvergence, nodes, nodes.GetN(), dvStartTime, traceInterval,
                        dvNetwork);

    // ─── Applications ──────────────────────────────────────────────

    NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue(prefix));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(1024));
    producerHelper.SetAttribute("Freshness", TimeValue(Seconds(2.0)));
    auto pApps = producerHelper.Install(producer);
    pApps.Start(Seconds(0.5));

    NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue(prefix));
    consumerHelper.SetAttribute("Frequency", DoubleValue(frequency));
    auto cApps = consumerHelper.Install(consumer);
    cApps.Start(Seconds(0.5));
    cApps.Stop(Seconds(simTime - 2.0));

    // ─── Rate Tracer ───────────────────────────────────────────────

    NdndRateTracer::InstallAll(rateTrace, Seconds(traceInterval));

    // ─── Link Traffic Tracer ───────────────────────────────────────

    std::shared_ptr<NdndLinkTracer> linkTracer;
    if (!linkTrace.empty())
    {
        linkTracer = NdndLinkTracer::Create(linkTrace, Seconds(traceInterval));
        for (const auto& link : reader.GetLinks())
        {
            linkTracer->ConnectLink(link.devices);
        }
    }

    // ─── Run ───────────────────────────────────────────────────────

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // Write convergence time to file (if requested)
    if (!convTrace.empty())
    {
        std::ofstream ofs(convTrace);
        if (g_converged)
        {
            ofs << g_convTime << std::endl;
        }
        else
        {
            ofs << -1 << std::endl;
        }
    }

    if (linkTracer)
    {
        linkTracer->Stop();
    }

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();

    return 0;
}
