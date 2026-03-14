/*
 * ndndSIM Grid Topology Example
 *
 * Equivalent to old ndnSIM: ndn-grid.cpp
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
 * Consumer at (0,0) sends 100 Interests/sec to /ndn/grid/<seqno>.
 * Producer at (2,2) replies with 1024-byte Data.
 * Routes are configured along the shortest path through the grid.
 *
 * Usage:
 *   ./ns3 run ndndsim-grid
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

    // Install on all grid nodes
    for (uint32_t row = 0; row < 3; ++row)
    {
        for (uint32_t col = 0; col < 3; ++col)
        {
            stackHelper.Install(grid.GetNode(row, col));
        }
    }

    // ─── Routing ───────────────────────────────────────────────────
    // Shortest path from Consumer(0,0) to Producer(2,2):
    //   (0,0) → (1,0) → (2,0) → (2,1) → (2,2)
    //
    // Grid node (r,c) device indices:
    //   In a 3x3 grid, each internal node has 4 links.
    //   We use AddRoutesToAll which floods routes on all faces.

    std::string prefix = "/ndn/grid";

    // Flood routes to all nodes — each device/face gets a route for the prefix.
    // This mimics the "SetDefaultRoutes" or global routing of old ndnSIM.
    NodeContainer allNodes;
    for (uint32_t row = 0; row < 3; ++row)
    {
        for (uint32_t col = 0; col < 3; ++col)
        {
            allNodes.Add(grid.GetNode(row, col));
        }
    }
    ndndsim::NdndStackHelper::AddRoutesToAll(prefix, allNodes);

    // ─── Applications ──────────────────────────────────────────────

    Ptr<Node> consumer = grid.GetNode(0, 0);
    Ptr<Node> producer = grid.GetNode(2, 2);

    ndndsim::NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue(prefix));
    consumerHelper.SetAttribute("Frequency", DoubleValue(100.0)); // 100 Interests/sec
    auto consumerApps = consumerHelper.Install(consumer);
    consumerApps.Start(Seconds(1.0));
    consumerApps.Stop(Seconds(10.0));

    ndndsim::NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue(prefix));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(1024));
    auto producerApps = producerHelper.Install(producer);
    producerApps.Start(Seconds(0.5));
    producerApps.Stop(Seconds(10.0));

    // ─── Simulation ────────────────────────────────────────────────

    Simulator::Stop(Seconds(11.0));
    Simulator::Run();

    ndndsim::NdndStackHelper::DestroyBridge();
    Simulator::Destroy();

    return 0;
}
