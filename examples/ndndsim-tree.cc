/*
 * ndndSIM Tree Topology Example
 *
 * Equivalent to old ndnSIM: ndn-tree-tracers.cpp
 *
 * Topology (binary tree built with point-to-point links):
 *
 *    leaf-0   leaf-1          leaf-2   leaf-3
 *       \      /                 \      /
 *        \    /                   \    /     10Mbps / 1ms
 *         \  /                     \  /
 *        rtr-0                    rtr-1
 *           \                      /
 *            \                    /      10Mbps / 1ms
 *             \                  /
 *              +---- root ------+
 *
 * 4 consumers at the leaves each send 100 Interests/sec.
 * 1 producer at the root serves all requests under /ndn/tree.
 * Routing is configured to direct all leaf traffic toward root.
 *
 * Usage:
 *   ./ns3 run ndndsim-tree
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

    LogComponentEnable("NdndConsumer", LOG_LEVEL_INFO);
    LogComponentEnable("NdndProducer", LOG_LEVEL_INFO);

    // ─── Create Nodes ──────────────────────────────────────────────

    // root(0), rtr-0(1), rtr-1(2), leaf-0(3), leaf-1(4), leaf-2(5), leaf-3(6)
    NodeContainer allNodes;
    allNodes.Create(7);

    Ptr<Node> root  = allNodes.Get(0);
    Ptr<Node> rtr0  = allNodes.Get(1);
    Ptr<Node> rtr1  = allNodes.Get(2);
    Ptr<Node> leaf0 = allNodes.Get(3);
    Ptr<Node> leaf1 = allNodes.Get(4);
    Ptr<Node> leaf2 = allNodes.Get(5);
    Ptr<Node> leaf3 = allNodes.Get(6);

    // ─── Point-to-Point Links ──────────────────────────────────────

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));

    // root ↔ rtr-0, root ↔ rtr-1
    p2p.Install(root, rtr0);   // root dev0 ↔ rtr0 dev0
    p2p.Install(root, rtr1);   // root dev1 ↔ rtr1 dev0

    // rtr-0 ↔ leaf-0, rtr-0 ↔ leaf-1
    p2p.Install(rtr0, leaf0);  // rtr0 dev1 ↔ leaf0 dev0
    p2p.Install(rtr0, leaf1);  // rtr0 dev2 ↔ leaf1 dev0

    // rtr-1 ↔ leaf-2, rtr-1 ↔ leaf-3
    p2p.Install(rtr1, leaf2);  // rtr1 dev1 ↔ leaf2 dev0
    p2p.Install(rtr1, leaf3);  // rtr1 dev2 ↔ leaf3 dev0

    // ─── NDNd Stack ────────────────────────────────────────────────

    ndndsim::NdndStackHelper::InitializeBridge();

    ndndsim::NdndStackHelper stackHelper;
    stackHelper.Install(allNodes);

    // ─── Routing (toward root) ─────────────────────────────────────

    std::string prefix = "/ndn/tree";

    // Leaves → their router (device 0 is the uplink)
    ndndsim::NdndStackHelper::AddRoute(leaf0, prefix, uint32_t(0), uint64_t(1));
    ndndsim::NdndStackHelper::AddRoute(leaf1, prefix, uint32_t(0), uint64_t(1));
    ndndsim::NdndStackHelper::AddRoute(leaf2, prefix, uint32_t(0), uint64_t(1));
    ndndsim::NdndStackHelper::AddRoute(leaf3, prefix, uint32_t(0), uint64_t(1));

    // Routers → root (device 0 is the uplink to root)
    ndndsim::NdndStackHelper::AddRoute(rtr0, prefix, uint32_t(0), uint64_t(1));
    ndndsim::NdndStackHelper::AddRoute(rtr1, prefix, uint32_t(0), uint64_t(1));

    // ─── Applications ──────────────────────────────────────────────

    // 4 consumers at leaf nodes
    ndndsim::NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Frequency", DoubleValue(100.0)); // 100 Interests/sec

    Ptr<Node> consumers[] = {leaf0, leaf1, leaf2, leaf3};
    for (int i = 0; i < 4; ++i)
    {
        // Each consumer uses unique prefix to avoid caching effects
        consumerHelper.SetAttribute("Prefix",
                                    StringValue(prefix + "/leaf-" + std::to_string(i)));
        auto apps = consumerHelper.Install(consumers[i]);
        apps.Start(Seconds(1.0));
        apps.Stop(Seconds(10.0));
    }

    // 1 producer at root — serves /ndn/tree
    ndndsim::NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue(prefix));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(1024));
    auto producerApps = producerHelper.Install(root);
    producerApps.Start(Seconds(0.5));
    producerApps.Stop(Seconds(10.0));

    // ─── Simulation ────────────────────────────────────────────────

    Simulator::Stop(Seconds(11.0));
    Simulator::Run();

    ndndsim::NdndStackHelper::DestroyBridge();
    Simulator::Destroy();

    return 0;
}
