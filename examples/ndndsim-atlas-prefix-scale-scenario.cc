/*
 * ndndsim-atlas-prefix-scale-scenario.cc
 *
 * Routing-only scenario on a fixed core/edge topology. After router
 * reachability converges, synthetic prefixes are announced only on the edge
 * routers. The scenario records per-node table metrics emitted by ndndSIM.
 */

#include "ns3/core-module.h"
#include "ns3/names.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include "ns3/ndndsim-go-bridge.h"
#include "ns3/ndndsim-link-tracer.h"
#include "ns3/ndndsim-stack-helper.h"
#include "ns3/ndndsim-stack.h"
#include "ns3/ndndsim-topology-reader.h"

#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace ns3;
using namespace ns3::ndndsim;

namespace {

std::vector<std::string>
SplitCsv(const std::string& text)
{
    std::vector<std::string> items;
    std::stringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ','))
    {
        if (!item.empty())
        {
            items.push_back(item);
        }
    }
    return items;
}

} // namespace

int
main(int argc, char* argv[])
{
    std::string topoFile;
    std::string linkTrace;
    std::string convTrace;
    std::string tableTrace;
    std::string dvConfig;
    std::string network = "/minindn";
    std::string edgeNodesCsv;
    double simTime = 40.0;
    double traceInterval = 0.05;
    int numPrefixes = 0;

    CommandLine cmd;
    cmd.AddValue("topo", "Topology file path (required)", topoFile);
    cmd.AddValue("linkTrace", "Output link traffic CSV path", linkTrace);
    cmd.AddValue("convTrace", "Output convergence time file path", convTrace);
    cmd.AddValue("tableTrace", "Output per-node table metrics CSV path", tableTrace);
    cmd.AddValue("edgeNodes", "Comma-separated edge node names", edgeNodesCsv);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.AddValue("traceInterval", "Link trace sampling interval in seconds", traceInterval);
    cmd.AddValue("dvConfig", "DV config JSON overlay (overrides defaults)", dvConfig);
    cmd.AddValue("network", "DV network prefix (default: /minindn)", network);
    cmd.AddValue("numPrefixes", "Total prefixes to announce across edge routers", numPrefixes);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_IF(topoFile.empty(), "--topo is required");

    std::vector<std::string> edgeNodeNames = SplitCsv(edgeNodesCsv);
    NS_ABORT_MSG_IF(edgeNodeNames.empty(), "--edgeNodes is required");
    std::unordered_set<std::string> edgeNodeSet(edgeNodeNames.begin(), edgeNodeNames.end());

    NdndTopologyReader reader;
    reader.SetFileName(topoFile);
    NodeContainer nodes = reader.Read();
    NS_ABORT_MSG_IF(nodes.GetN() == 0, "No nodes read from topology file");

    std::vector<Ptr<Node>> edgeNodes;
    edgeNodes.reserve(edgeNodeNames.size());
    for (const auto& name : edgeNodeNames)
    {
        Ptr<Node> node = Names::Find<Node>(name);
        NS_ABORT_MSG_IF(node == nullptr, "Edge node not found in topology: " << name);
        edgeNodes.push_back(node);
    }

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);
    NdndStackHelper::EnableDvRouting(network, nodes, dvConfig);

    NdndSimSetTotalNodes(static_cast<int>(nodes.GetN()));
    if (numPrefixes > 0)
    {
        RegisterRoutingConvergedCallback([edgeNodes, numPrefixes]() {
            for (int i = 0; i < numPrefixes; ++i)
            {
                Ptr<Node> node = edgeNodes.at(static_cast<size_t>(i) % edgeNodes.size());
                std::string nodeName = Names::FindName(node);
                auto stack = node->GetObject<NdndStack>();
                std::string prefix = "/data/" + nodeName + "/pfx" + std::to_string(i);
                stack->AnnouncePrefixToDv(prefix);
            }
        });
    }

    std::shared_ptr<NdndLinkTracer> linkTracer;
    if (!linkTrace.empty())
    {
        linkTracer = NdndLinkTracer::Create(linkTrace, Seconds(traceInterval));
        for (const auto& link : reader.GetLinks())
        {
            linkTracer->ConnectLink(link.devices);
        }
    }

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    if (!convTrace.empty())
    {
        std::ofstream ofs(convTrace);
        NS_ABORT_MSG_IF(!ofs, "Failed to open convTrace output: " << convTrace);

        int64_t convNs = NdndSimGetRoutingConvergenceNs(static_cast<int>(nodes.GetN()));
        if (convNs >= 0)
        {
            ofs << (static_cast<double>(convNs) / 1e9) << std::endl;
        }
        else
        {
            ofs << -1 << std::endl;
        }
    }

    if (!tableTrace.empty())
    {
        std::ofstream ofs(tableTrace);
        NS_ABORT_MSG_IF(!ofs, "Failed to open tableTrace output: " << tableTrace);

        ofs << "node,role,table_category,table_name,entry_count\n";
        for (uint32_t i = 0; i < nodes.GetN(); ++i)
        {
            Ptr<Node> node = nodes.Get(i);
            std::string nodeName = Names::FindName(node);
            std::string role = edgeNodeSet.find(nodeName) != edgeNodeSet.end() ? "edge" : "core";
            auto stack = node->GetObject<NdndStack>();
            std::string report = stack->GetTableMetricsReport();

            std::stringstream reportStream(report);
            std::string line;
            while (std::getline(reportStream, line))
            {
                if (line.empty())
                {
                    continue;
                }

                std::vector<std::string> fields = SplitCsv(line);
                NS_ABORT_MSG_IF(fields.size() < 3,
                                "Malformed table metric line for node " << nodeName << ": "
                                                                       << line);
                ofs << nodeName << ',' << role << ',' << fields[0] << ',' << fields[1] << ','
                    << fields[2] << '\n';
            }
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