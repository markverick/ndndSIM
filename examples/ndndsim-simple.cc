/*
 * ndndSIM Simple Example
 *
 * Topology:
 *
 *   Consumer ---- Router ---- Producer
 *   (node 0)    (node 1)    (node 2)
 *
 * All links are point-to-point with 1Gbps and 10ms delay.
 * The consumer sends Interests for /ndn/test/<seqno>.
 * The producer registers /ndn/test and replies with Data.
 * The router forwards packets using NDNd's BestRoute strategy.
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

// ndndSIM headers
#include "ns3/ndndsim-stack-helper.h"
#include "ns3/ndndsim-app-helper.h"

using namespace ns3;

int
main(int argc, char* argv[])
{
    // Enable logging
    LogComponentEnable("NdndStack", LOG_LEVEL_INFO);
    LogComponentEnable("NdndConsumer", LOG_LEVEL_INFO);
    LogComponentEnable("NdndProducer", LOG_LEVEL_INFO);
    LogComponentEnable("NdndSimGoBridge", LOG_LEVEL_INFO);

    // ─── Topology ──────────────────────────────────────────────────

    NodeContainer nodes;
    nodes.Create(3);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("10ms"));

    // Consumer (0) <-> Router (1)
    NetDeviceContainer dev01 = p2p.Install(nodes.Get(0), nodes.Get(1));
    // Router (1) <-> Producer (2)
    NetDeviceContainer dev12 = p2p.Install(nodes.Get(1), nodes.Get(2));

    // ─── NDNd Stack ────────────────────────────────────────────────

    // Initialize the Go bridge (must be called once)
    ndndsim::NdndStackHelper::InitializeBridge();

    // Install NDNd on all nodes
    ndndsim::NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    // ─── Routing ───────────────────────────────────────────────────

    // Consumer: route /ndn/test → face connected to Router (device 0)
    ndndsim::NdndStackHelper::AddRoute(nodes.Get(0), "/ndn/test", uint32_t(0), uint64_t(1));

    // Router: route /ndn/test → face connected to Producer (device 1)
    // Note: device 0 = link to Consumer, device 1 = link to Producer
    ndndsim::NdndStackHelper::AddRoute(nodes.Get(1), "/ndn/test", uint32_t(1), uint64_t(1));

    // Producer: prefix served locally (route added by app to face 1)

    // ─── Applications ──────────────────────────────────────────────

    // Consumer on node 0
    ndndsim::NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue("/ndn/test"));
    consumerHelper.SetAttribute("Frequency", DoubleValue(10.0)); // 10 Interests/sec
    auto consumerApps = consumerHelper.Install(nodes.Get(0));
    consumerApps.Start(Seconds(1.0));
    consumerApps.Stop(Seconds(10.0));

    // Producer on node 2
    ndndsim::NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue("/ndn/test"));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(1024));
    auto producerApps = producerHelper.Install(nodes.Get(2));
    producerApps.Start(Seconds(0.5));
    producerApps.Stop(Seconds(10.0));

    // ─── Simulation ────────────────────────────────────────────────

    Simulator::Stop(Seconds(11.0));
    Simulator::Run();

    // Cleanup
    ndndsim::NdndStackHelper::DestroyBridge();
    Simulator::Destroy();

    return 0;
}
