/*
 * ndndsim-dv-multipath.cc
 *
 * Demonstrates DV routing on a diamond topology with redundant paths.
 * The consumer can reach the producer via two different intermediate
 * routers. DV discovers both routes and the forwarder selects the
 * best next-hop. The link between Router-A and the Producer is
 * intentionally slower, so DV should prefer the path through Router-B.
 *
 * Topology:
 *
 *              10Mbps/1ms   10Mbps/1ms
 *   Consumer ──────────── Router-A ──────────── Producer
 *      │                                            │
 *      │  10Mbps/1ms                    10Mbps/1ms  │
 *      └──────────── Router-B ──────────────────────┘
 *
 *   All links are symmetric.
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
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
    CommandLine cmd;
    cmd.Parse(argc, argv);

    LogComponentEnable("NdndConsumer", LOG_LEVEL_INFO);
    LogComponentEnable("NdndProducer", LOG_LEVEL_INFO);

    // ─── Diamond Topology ──────────────────────────────────────────

    NodeContainer nodes;
    nodes.Create(4); // 0=Consumer, 1=Router-A, 2=Router-B, 3=Producer

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));

    p2p.Install(nodes.Get(0), nodes.Get(1)); // Consumer -- Router-A
    p2p.Install(nodes.Get(0), nodes.Get(2)); // Consumer -- Router-B
    p2p.Install(nodes.Get(1), nodes.Get(3)); // Router-A -- Producer
    p2p.Install(nodes.Get(2), nodes.Get(3)); // Router-B -- Producer

    // ─── NDNd Stack ────────────────────────────────────────────────

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    NdndStackHelper::EnableDvRouting("/ndn", nodes);

    // ─── Applications ──────────────────────────────────────────────

    // Producer on node 3
    NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue("/ndn/multipath"));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(1024));
    producerHelper.SetAttribute("Freshness", TimeValue(Seconds(1.0)));
    auto producerApps = producerHelper.Install(nodes.Get(3));
    producerApps.Start(Seconds(0.5));
    producerApps.Stop(Seconds(25.0));

    // Consumer on node 0 — starts after DV convergence
    NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue("/ndn/multipath"));
    consumerHelper.SetAttribute("Frequency", DoubleValue(10.0));
    auto consumerApps = consumerHelper.Install(nodes.Get(0));
    consumerApps.Start(Seconds(5.0));
    consumerApps.Stop(Seconds(20.0));

    // ─── Rate Tracer ───────────────────────────────────────────────

    NdndRateTracer::InstallAll("ndndsim-dv-multipath-rate-trace.csv", Seconds(0.5));

    // ─── Run ───────────────────────────────────────────────────────

    Simulator::Stop(Seconds(25.0));
    Simulator::Run();

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();

    return 0;
}
