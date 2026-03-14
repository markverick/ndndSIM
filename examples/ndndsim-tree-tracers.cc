/*
 * ndndSIM Tree Topology with Rate Tracers Example
 *
 * Equivalent to old ndnSIM: ndn-tree-tracers.cpp
 *
 * Reads the tree topology from file and installs rate tracers that
 * write periodic CSV statistics to "rate-trace.csv".
 *
 * Topology (from topo-tree.txt):
 *
 *    leaf-1   leaf-2          leaf-3   leaf-4
 *       \      /                 \      /
 *        \    /                   \    /     10Mbps / 1ms
 *         \  /                     \  /
 *        rtr-1                    rtr-2
 *           \                      /
 *            +------ root --------+
 *
 * Usage:
 *   ./ns3 run ndndsim-tree-tracers
 *   # Then inspect rate-trace.csv
 */

#include "ns3/core-module.h"
#include "ns3/names.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

// ndndSIM headers
#include "ns3/ndndsim-app-helper.h"
#include "ns3/ndndsim-rate-tracer.h"
#include "ns3/ndndsim-stack-helper.h"
#include "ns3/ndndsim-topology-reader.h"

using namespace ns3;

int
main(int argc, char* argv[])
{
    CommandLine cmd;
    cmd.Parse(argc, argv);

    // ─── Read tree topology ────────────────────────────────────────

    ndndsim::NdndTopologyReader reader;
    reader.SetFileName("contrib/ndndSIM/examples/topologies/topo-tree.txt");
    NodeContainer nodes = reader.Read();

    std::cout << "Tree topology: " << nodes.GetN() << " nodes, "
              << reader.GetLinks().size() << " links" << std::endl;

    // ─── NDNd Stack ────────────────────────────────────────────────

    ndndsim::NdndStackHelper::InitializeBridge();

    ndndsim::NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    // ─── Routing (all toward root) ─────────────────────────────────

    std::string prefix = "/ndn/tree";
    ndndsim::NdndStackHelper::AddRoutesToAll(prefix, nodes);

    // ─── 4 Consumers at leaves ─────────────────────────────────────

    ndndsim::NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Frequency", DoubleValue(100.0));

    const char* leafNames[] = {"leaf-1", "leaf-2", "leaf-3", "leaf-4"};
    for (const auto& name : leafNames)
    {
        Ptr<Node> leaf = Names::Find<Node>(name);
        NS_ABORT_MSG_IF(!leaf, "Leaf node not found: " << name);

        consumerHelper.SetAttribute("Prefix",
                                    StringValue(std::string(prefix) + "/" + name));
        auto apps = consumerHelper.Install(leaf);
        apps.Start(Seconds(1.0));
        apps.Stop(Seconds(10.0));
    }

    // ─── Producer at root ──────────────────────────────────────────

    Ptr<Node> root = Names::Find<Node>("root");
    NS_ABORT_MSG_IF(!root, "Root node not found");

    ndndsim::NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue(prefix));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(1024));
    auto producerApps = producerHelper.Install(root);
    producerApps.Start(Seconds(0.5));
    producerApps.Stop(Seconds(10.0));

    // ─── Rate Tracers ──────────────────────────────────────────────
    // Install after apps are configured. Writes CSV every 0.5 seconds.
    ndndsim::NdndRateTracer::InstallAll("rate-trace.csv", Seconds(0.5));

    // ─── Simulation ────────────────────────────────────────────────

    Simulator::Stop(Seconds(11.0));
    Simulator::Run();

    std::cout << "Rate trace written to: rate-trace.csv" << std::endl;

    ndndsim::NdndStackHelper::DestroyBridge();
    Simulator::Destroy();

    return 0;
}
