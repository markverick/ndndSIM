/*
 * ndndSIM Grid DV Routing Example
 *
 * Same topology as ndndsim-grid but uses DV routing instead of
 * BFS-based CalculateRoutes. Routes are discovered dynamically
 * via distance-vector advertisement exchange.
 *
 * Topology: 3x3 PointToPoint Grid
 *
 *   (consumer) -- ( ) ----- ( )
 *       |          |         |
 *      ( ) ------ ( ) ----- ( )
 *       |          |         |
 *      ( ) ------ ( ) -- (producer)
 *
 * All links are 1Mbps with 10ms delay.
 * DV routing is enabled at simulation start. Consumer begins at
 * t=5 to allow DV convergence.
 *
 * Usage:
 *   ./ns3 run ndndsim-grid-dv
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-layout-module.h"
#include "ns3/point-to-point-module.h"

// ndndSIM headers
#include "ns3/ndndsim-app-helper.h"
#include "ns3/ndndsim-stack-helper.h"
#include "ns3/ndndsim-stack.h"

using namespace ns3;

int
main(int argc, char* argv[])
{
    CommandLine cmd;
    cmd.Parse(argc, argv);

    LogComponentEnable("NdndConsumer", LOG_LEVEL_INFO);
    LogComponentEnable("NdndProducer", LOG_LEVEL_INFO);

    // ─── Grid Topology ─────────────────────────────────────────────

    Config::SetDefault("ns3::PointToPointNetDevice::DataRate", StringValue("1Mbps"));
    Config::SetDefault("ns3::PointToPointChannel::Delay", StringValue("10ms"));
    Config::SetDefault("ns3::DropTailQueue<Packet>::MaxSize", StringValue("10p"));

    PointToPointHelper p2p;
    PointToPointGridHelper grid(3, 3, p2p);
    grid.BoundingBox(100.0, 100.0, 200.0, 200.0);

    // ─── NDNd Stack ────────────────────────────────────────────────

    ndndsim::NdndStackHelper::InitializeBridge();

    ndndsim::NdndStackHelper stackHelper;

    NodeContainer allNodes;
    for (uint32_t row = 0; row < 3; ++row)
    {
        for (uint32_t col = 0; col < 3; ++col)
        {
            stackHelper.Install(grid.GetNode(row, col));
            allNodes.Add(grid.GetNode(row, col));
        }
    }

    // ─── DV Routing ────────────────────────────────────────────────
    // Enable DV on all nodes. Routes will be discovered dynamically.

    ndndsim::NdndStackHelper::EnableDvRouting("/ndn", allNodes);

    // ─── Applications ──────────────────────────────────────────────

    std::string prefix = "/ndn/grid";

    Ptr<Node> consumer = grid.GetNode(0, 0);
    Ptr<Node> producer = grid.GetNode(2, 2);

    // Producer starts early so DV can propagate prefix advertisements
    ndndsim::NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue(prefix));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(1024));
    auto producerApps = producerHelper.Install(producer);
    producerApps.Start(Seconds(0.5));
    producerApps.Stop(Seconds(20.0));

    // Consumer starts after DV convergence
    ndndsim::NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue(prefix));
    consumerHelper.SetAttribute("Frequency", DoubleValue(100.0)); // 100 Interests/sec
    auto consumerApps = consumerHelper.Install(consumer);
    consumerApps.Start(Seconds(5.0));
    consumerApps.Stop(Seconds(15.0));

    // ─── Simulation ────────────────────────────────────────────────

    Simulator::Stop(Seconds(20.0));
    Simulator::Run();

    ndndsim::NdndStackHelper::DestroyBridge();
    Simulator::Destroy();

    return 0;
}
