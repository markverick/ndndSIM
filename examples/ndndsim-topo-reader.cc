/*
 * ndndSIM Topology Reader Example
 *
 * Equivalent to old ndnSIM: ndn-grid-topo-plugin.cpp / ndn-tree-tracers.cpp
 *
 * Reads a standard ndnSIM topology file and runs NDN over it.
 * Uses the same file format as the old AnnotatedTopologyReader.
 *
 * Default: reads topo-grid-3x3.txt
 *   Consumer at "Node0", Producer at "Node8"
 *
 * Usage:
 *   ./ns3 run ndndsim-topo-reader
 *   ./ns3 run "ndndsim-topo-reader --topo=contrib/ndndSIM/examples/topologies/topo-tree.txt --consumer=leaf-1 --producer=root"
 */

#include "ns3/core-module.h"
#include "ns3/names.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

// ndndSIM headers
#include "ns3/ndndsim-app-helper.h"
#include "ns3/ndndsim-stack-helper.h"
#include "ns3/ndndsim-topology-reader.h"

using namespace ns3;

int
main(int argc, char* argv[])
{
    std::string topoFile = "contrib/ndndSIM/examples/topologies/topo-grid-3x3.txt";
    std::string consumerName = "Node0";
    std::string producerName = "Node8";
    std::string prefix = "/ndn/topo";

    CommandLine cmd;
    cmd.AddValue("topo", "Topology file path", topoFile);
    cmd.AddValue("consumer", "Consumer node name", consumerName);
    cmd.AddValue("producer", "Producer node name", producerName);
    cmd.AddValue("prefix", "NDN name prefix", prefix);
    cmd.Parse(argc, argv);

    LogComponentEnable("NdndConsumer", LOG_LEVEL_INFO);
    LogComponentEnable("NdndProducer", LOG_LEVEL_INFO);

    // ─── Read topology from file ───────────────────────────────────

    ndndsim::NdndTopologyReader reader;
    reader.SetFileName(topoFile);
    NodeContainer nodes = reader.Read();

    std::cout << "Topology: " << nodes.GetN() << " nodes, "
              << reader.GetLinks().size() << " links" << std::endl;

    Ptr<Node> consumer = Names::Find<Node>(consumerName);
    Ptr<Node> producer = Names::Find<Node>(producerName);
    NS_ABORT_MSG_IF(!consumer, "Consumer node not found: " << consumerName);
    NS_ABORT_MSG_IF(!producer, "Producer node not found: " << producerName);

    // ─── NDNd Stack ────────────────────────────────────────────────

    ndndsim::NdndStackHelper::InitializeBridge();

    ndndsim::NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    // ─── Routing ───────────────────────────────────────────────────
    // Flood routes to all nodes (like SetDefaultRoutes)
    ndndsim::NdndStackHelper::AddRoutesToAll(prefix, nodes);

    // ─── Applications ──────────────────────────────────────────────

    ndndsim::NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue(prefix));
    consumerHelper.SetAttribute("Frequency", DoubleValue(100.0));
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
