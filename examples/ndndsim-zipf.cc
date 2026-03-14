/*
 * ndndSIM Zipf-Mandelbrot Consumer Example
 *
 * Equivalent to old ndnSIM: ndn-zipf-mandelbrot.cpp
 *
 * Topology:
 *
 *   Consumer ---- Router ---- Producer
 *   (node 0)    (node 1)    (node 2)
 *
 * The consumer selects content IDs from a Zipf-Mandelbrot distribution
 * with 100 content items. This models realistic content popularity
 * where a few items are requested much more frequently than others.
 *
 * Usage:
 *   ./ns3 run ndndsim-zipf
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

// ndndSIM headers
#include "ns3/ndndsim-app-helper.h"
#include "ns3/ndndsim-stack-helper.h"

using namespace ns3;

int
main(int argc, char* argv[])
{
    CommandLine cmd;
    cmd.Parse(argc, argv);

    LogComponentEnable("NdndConsumerZipf", LOG_LEVEL_INFO);
    LogComponentEnable("NdndProducer", LOG_LEVEL_INFO);

    // ─── Topology ──────────────────────────────────────────────────

    Config::SetDefault("ns3::PointToPointNetDevice::DataRate", StringValue("1Mbps"));
    Config::SetDefault("ns3::PointToPointChannel::Delay", StringValue("10ms"));

    NodeContainer nodes;
    nodes.Create(3);

    PointToPointHelper p2p;
    p2p.Install(nodes.Get(0), nodes.Get(1));
    p2p.Install(nodes.Get(1), nodes.Get(2));

    // ─── NDNd Stack ────────────────────────────────────────────────

    ndndsim::NdndStackHelper::InitializeBridge();

    ndndsim::NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    // ─── Routing ───────────────────────────────────────────────────

    std::string prefix = "/ndn/zipf";
    ndndsim::NdndStackHelper::AddRoute(nodes.Get(0), prefix, uint32_t(0), uint64_t(1));
    ndndsim::NdndStackHelper::AddRoute(nodes.Get(1), prefix, uint32_t(1), uint64_t(1));

    // ─── Zipf Consumer on node 0 ──────────────────────────────────

    ndndsim::NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumerZipf");
    consumerHelper.SetAttribute("Prefix", StringValue(prefix));
    consumerHelper.SetAttribute("Frequency", DoubleValue(10.0));
    consumerHelper.SetAttribute("NumberOfContents", UintegerValue(100));
    consumerHelper.SetAttribute("q", DoubleValue(0.0));
    consumerHelper.SetAttribute("s", DoubleValue(0.7));
    auto consumerApps = consumerHelper.Install(nodes.Get(0));
    consumerApps.Start(Seconds(1.0));
    consumerApps.Stop(Seconds(10.0));

    // ─── Producer on node 2 ────────────────────────────────────────

    ndndsim::NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue(prefix));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(1024));
    auto producerApps = producerHelper.Install(nodes.Get(2));
    producerApps.Start(Seconds(0.5));
    producerApps.Stop(Seconds(10.0));

    // ─── Simulation ────────────────────────────────────────────────

    Simulator::Stop(Seconds(11.0));
    Simulator::Run();

    ndndsim::NdndStackHelper::DestroyBridge();
    Simulator::Destroy();

    return 0;
}
