/*
 * ndndsim-atlas-prefix-scale-scenario.cc
 *
 * Routing-only scenario on a caller-provided topology. After router
 * reachability converges, synthetic prefixes are announced only on the
 * caller-specified edge routers. The scenario records per-node table metrics
 * emitted by ndndSIM.
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
    std::string coreDvConfig;
    std::string edgeDvConfig;
    std::string network = "/minindn";
    std::string edgeNodesCsv;
    std::string exportSnap;
    std::string importSnap;
    double simTime = 40.0;
    double traceInterval = 0.05;
    double stableWindow = 2.0;
    int numPrefixes = 0;

    CommandLine cmd;
    cmd.AddValue("topo", "Topology file path (required)", topoFile);
    cmd.AddValue("linkTrace", "Output link traffic CSV path", linkTrace);
    cmd.AddValue("convTrace", "Output convergence time file path", convTrace);
    cmd.AddValue("tableTrace", "Output per-node table metrics CSV path", tableTrace);
    cmd.AddValue("edgeNodes", "Comma-separated edge node names", edgeNodesCsv);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.AddValue("traceInterval", "Link trace sampling interval in seconds", traceInterval);
    cmd.AddValue("stableWindow",
                 "Stability window in seconds for prefix convergence detection. "
                 "The sim stops once NdndSimGetConvergenceMetric is unchanged for "
                 "this long. Should be >= 2 * DV adv-interval (default: 2.0s).",
                 stableWindow);
    cmd.AddValue("dvConfig", "DV config JSON overlay (overrides defaults)", dvConfig);
    cmd.AddValue("coreDvConfig", "DV config JSON overlay applied to core nodes",
                 coreDvConfig);
    cmd.AddValue("edgeDvConfig", "DV config JSON overlay applied to edge nodes",
                 edgeDvConfig);
    cmd.AddValue("network", "DV network prefix (default: /minindn)", network);
    cmd.AddValue("numPrefixes", "Total prefixes to announce across edge routers", numPrefixes);
    cmd.AddValue("exportSnap", "Export DV snapshot to this JSON file after routing converges",
                 exportSnap);
    cmd.AddValue("importSnap", "Import DV snapshot from this JSON file before simulation starts",
                 importSnap);
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
    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        Ptr<Node> node = nodes.Get(i);
        std::string nodeName = Names::FindName(node);
        std::string roleDvConfig =
          edgeNodeSet.find(nodeName) != edgeNodeSet.end() ? edgeDvConfig : coreDvConfig;
        const std::string& effectiveDvConfig =
          roleDvConfig.empty() ? dvConfig : roleDvConfig;
        NdndStackHelper::EnableDvRouting(network, node, effectiveDvConfig);
    }

    NdndSimSetTotalNodes(static_cast<int>(nodes.GetN()));

    if (!importSnap.empty())
    {
        // Routing state is pre-loaded from snapshot; convergence is instantaneous.
        // Import the snapshot, then schedule prefix announcements at t=0 so they
        // propagate during the simulation window.
        int rc = NdndSimImportSnapshot(importSnap.c_str());
        NS_ABORT_MSG_IF(rc != 0, "NdndSimImportSnapshot failed for: " << importSnap);

        if (numPrefixes > 0)
        {
            Simulator::Schedule(Seconds(0.0), [edgeNodes, numPrefixes]() {
                for (int i = 0; i < numPrefixes; ++i)
                {
                    Ptr<Node> node = edgeNodes.at(static_cast<size_t>(i) % edgeNodes.size());
                    std::string nodeName = Names::FindName(node);
                    auto stack = node->GetObject<NdndStack>();
                    std::string prefix = "/data/" + nodeName + "/pfx" + std::to_string(i);
                    stack->RegisterProducer(prefix);
                }
            });

            // Install an event-driven stop condition based on stableWindow:
            //
            //   stableWindow > 0  →  stability window (twophase/ndnd@dv2):
            //     NdndSimGetConvergenceMetric() sums forwarder_pet entries.
            //     PET is updated synchronously with DV prefix events so the
            //     count stabilises exactly when all prefix→router mappings are
            //     installed.  Stop once the count has risen above the baseline
            //     AND has been unchanged for stableRoundsNeeded polls.
            //
            //   stableWindow == 0  →  target-count (onephase/ndnd@main):
            //     Every node acquires exactly numPrefixes new FIB entries when
            //     fully converged, so the global delta is
            //       numPrefixes × numNodes.
            //     Baseline is read on the FIRST poll tick (not before
            //     Simulator::Run()) so all t=0 DES events have already fired
            //     and the FIB is in a consistent initial state.  Stops as soon
            //     as the metric reaches baseCount + targetDelta, independent of
            //     topology size, prefix count, or SVS periodic-timeout cycles.
            //
            //   stableWindow < 0  →  no event-driven stop; simulation runs to
            //     the hard --simTime ceiling.
            if (stableWindow > 0)
            {
            const int stableRoundsNeeded =
                std::max(1, static_cast<int>(stableWindow / traceInterval));
            auto baseCount = std::make_shared<int64_t>(-1); // -1 = not yet captured
            auto lastCount = std::make_shared<int64_t>(-1);
            auto stableRounds = std::make_shared<int>(0);
            auto checkerPtr = std::make_shared<std::function<void()>>();
            *checkerPtr = [checkerPtr, baseCount, lastCount, stableRounds, traceInterval, stableRoundsNeeded]() {
                int64_t raw = NdndSimGetConvergenceMetric();
                if (*baseCount < 0)
                {
                    *baseCount = raw;
                    *lastCount = raw;
                    Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
                    return;
                }
                if (raw > *baseCount && raw == *lastCount)
                {
                    if (++(*stableRounds) >= stableRoundsNeeded)
                    {
                        Simulator::Stop();
                        return;
                    }
                }
                else
                {
                    *stableRounds = 0;
                    *lastCount = raw;
                }
                Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
            };
            Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
            } // if (stableWindow > 0)
            else if (stableWindow == 0.0)
            {
            // Target-count checker: read baseline on first tick, then stop
            // once metric >= baseCount + numPrefixes * numNodes.
            const int64_t targetDelta = static_cast<int64_t>(numPrefixes) *
                                        static_cast<int64_t>(nodes.GetN());
            auto baseCount = std::make_shared<int64_t>(-1); // -1 = not yet read
            auto checkerPtr = std::make_shared<std::function<void()>>();
            *checkerPtr = [checkerPtr, baseCount, targetDelta, traceInterval]() {
                int64_t raw = NdndSimGetConvergenceMetric();
                if (*baseCount < 0)
                {
                    // First tick: t=0 DES events have all fired; safe to capture
                    // the baseline FIB count.
                    *baseCount = raw;
                    Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
                    return;
                }
                if (raw >= *baseCount + targetDelta)
                {
                    Simulator::Stop();
                    return;
                }
                Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
            };
            Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
            } // else if (stableWindow == 0.0)
        }
    }
    else
    {
        // Normal path: wait for DV routing to converge, then announce prefixes
        // and optionally export a snapshot.
        if (numPrefixes > 0)
        {
            RegisterRoutingConvergedCallback([edgeNodes, numPrefixes, exportSnap]() {
                if (!exportSnap.empty())
                {
                    int rc = NdndSimExportSnapshot(exportSnap.c_str());
                    NS_ABORT_MSG_IF(rc != 0,
                                    "NdndSimExportSnapshot failed for: " << exportSnap);
                }
                for (int i = 0; i < numPrefixes; ++i)
                {
                    Ptr<Node> node = edgeNodes.at(static_cast<size_t>(i) % edgeNodes.size());
                    std::string nodeName = Names::FindName(node);
                    auto stack = node->GetObject<NdndStack>();
                    std::string prefix = "/data/" + nodeName + "/pfx" + std::to_string(i);
                    stack->RegisterProducer(prefix);
                }
            });
        }
        else if (!exportSnap.empty())
        {
            RegisterRoutingConvergedCallback([exportSnap]() {
                int rc = NdndSimExportSnapshot(exportSnap.c_str());
                NS_ABORT_MSG_IF(rc != 0, "NdndSimExportSnapshot failed for: " << exportSnap);
                // Stop immediately after snapshot export: no prefixes to propagate,
                // so there is no reason to continue simulating past DV convergence.
                Simulator::Stop();
            });
        }
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

    // When importing a snapshot, export the post-run snapshot here (after prefix
    // SVS sync has propagated) rather than inside the convergence callback.
    if (!importSnap.empty() && !exportSnap.empty())
    {
        int rc = NdndSimExportSnapshot(exportSnap.c_str());
        NS_ABORT_MSG_IF(rc != 0, "NdndSimExportSnapshot failed for: " << exportSnap);
    }

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