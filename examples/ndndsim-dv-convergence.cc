/*
 * ndndsim-dv-convergence.cc
 *
 * Demonstrates DV routing convergence on a larger topology.
 * A 4×4 grid uses DV for automatic route discovery. The consumer
 * starts early (before DV converges) and continues after. The rate
 * tracer output shows how Data delivery ramps up as DV propagates
 * routes across the grid.
 *
 * Consumer at (0,0), Producer at (3,3) — maximum hop distance.
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-layout-module.h"
#include "ns3/point-to-point-module.h"

// ndndSIM headers
#include "ns3/ndndsim-app-helper.h"
#include "ns3/ndndsim-rate-tracer.h"
#include "ns3/ndndsim-stack-helper.h"
#include "ns3/ndndsim-stack.h"

using namespace ns3;
using namespace ns3::ndndsim;

int
main(int argc, char* argv[])
{
    uint32_t gridSize = 4;
    double simTime = 30.0;
    std::string delay = "50ms"; // high per-hop delay slows DV convergence

    CommandLine cmd;
    cmd.AddValue("grid", "Grid side length (default 4)", gridSize);
    cmd.AddValue("delay", "Per-link propagation delay (default 50ms)", delay);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.Parse(argc, argv);

    LogComponentEnable("NdndConsumer", LOG_LEVEL_INFO);
    LogComponentEnable("NdndProducer", LOG_LEVEL_INFO);

    // ─── Grid Topology ─────────────────────────────────────────────

    Config::SetDefault("ns3::PointToPointNetDevice::DataRate", StringValue("10Mbps"));
    Config::SetDefault("ns3::PointToPointChannel::Delay", StringValue(delay));
    Config::SetDefault("ns3::DropTailQueue<Packet>::MaxSize", StringValue("20p"));

    PointToPointHelper p2p;
    PointToPointGridHelper grid(gridSize, gridSize, p2p);
    grid.BoundingBox(100.0, 100.0, 300.0, 300.0);

    // ─── NDNd Stack on all nodes ───────────────────────────────────

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    NodeContainer allNodes;
    for (uint32_t row = 0; row < gridSize; ++row)
    {
        for (uint32_t col = 0; col < gridSize; ++col)
        {
            stackHelper.Install(grid.GetNode(row, col));
            allNodes.Add(grid.GetNode(row, col));
        }
    }

    NdndStackHelper::EnableDvRouting("/ndn", allNodes);

    // ─── Applications ──────────────────────────────────────────────

    std::string prefix = "/ndn/convergence";

    // Producer at opposite corner (gridSize-1, gridSize-1)
    NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue(prefix));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(1024));
    producerHelper.SetAttribute("Freshness", TimeValue(Seconds(2.0)));
    auto producerApps = producerHelper.Install(grid.GetNode(gridSize - 1, gridSize - 1));
    producerApps.Start(Seconds(0.5));
    producerApps.Stop(Seconds(simTime));

    // Consumer at (0,0) — starts at the same time as the producer.
    // With high link delays, DV takes several seconds to converge across
    // the grid. Early Interests are lost; Data ramps up after convergence.
    NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue(prefix));
    consumerHelper.SetAttribute("Frequency", DoubleValue(10.0));
    auto consumerApps = consumerHelper.Install(grid.GetNode(0, 0));
    consumerApps.Start(Seconds(0.5));
    consumerApps.Stop(Seconds(simTime - 2.0));

    // ─── Rate Tracer ───────────────────────────────────────────────

    NdndRateTracer::InstallAll("ndndsim-dv-convergence-rate-trace.csv", Seconds(1.0));

    // ─── Run ───────────────────────────────────────────────────────

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();

    return 0;
}
