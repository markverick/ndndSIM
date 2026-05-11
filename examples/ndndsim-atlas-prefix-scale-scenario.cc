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

void
CountMacTxDrop(uint64_t* cnt, Ptr<const Packet>)
{
    ++(*cnt);
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
    std::string dropTrace;
    double simTime = 40.0;
    double traceInterval = 0.05;
    double stableWindow = 2.0;
    int numPrefixes = 0;
    double announceGapMs = 0.0;

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
    cmd.AddValue("announceGap",
                 "Gap in milliseconds between successive edge-node prefix announcements. "
                 "0 (default) means all nodes announce simultaneously.",
                 announceGapMs);
    cmd.AddValue("dropTrace", "Output file path for total MacTxDrop count (optional)",
                 dropTrace);
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
            const double announceDelay = 0.0;
            const double gapS = announceGapMs / 1000.0;
            // Schedule each prefix individually, staggered by edge-node index.
            // Prefix i is assigned round-robin to edgeNodes[i % N], so it fires
            // at announceDelay + (i % N) * gapS.  When gapS == 0 all fire at the
            // same time, reproducing the original simultaneous-burst behaviour.
            for (int i = 0; i < numPrefixes; ++i)
            {
                size_t nodeIdx = static_cast<size_t>(i) % edgeNodes.size();
                Ptr<Node> node = edgeNodes.at(nodeIdx);
                std::string nodeName = Names::FindName(node);
                std::string prefix = "/data/" + nodeName + "/pfx" + std::to_string(i);
                double t = announceDelay + static_cast<double>(nodeIdx) * gapS;
                Simulator::Schedule(Seconds(t), [node, prefix]() {
                    node->GetObject<NdndStack>()->RegisterProducer(prefix);
                });
            }

            // SVS-delivery-silence checker: stop once no PES SVS publication has
            // been delivered to any node for stableWindow seconds.
            //
            // NdndSimGetLastPfxSvsDeliveryNs() is set inside
            // PrefixModule.simSubscribePublisher each time any node's SVS
            // subscription callback fires, and initialized to -1 (no delivery).
            //
            // When lastSvsNs < 0 (no delivery yet — e.g. p=0 baseline runs where
            // no prefixes are announced) we fall back to startNs so silence is
            // counted from the time the checker was installed instead of waiting
            // forever for a delivery that will never come.
            //
            // stableWindow < 0  →  no event-driven stop; sim runs to --simTime.
            if (stableWindow > 0)
            {
            const int64_t silenceNs = static_cast<int64_t>(stableWindow * 1e9);
            const int64_t startNs = Simulator::Now().GetNanoSeconds();
            auto checkerPtr = std::make_shared<std::function<void()>>();
            *checkerPtr = [checkerPtr, silenceNs, startNs, traceInterval]() {
                int64_t nowNs = Simulator::Now().GetNanoSeconds();
                int64_t lastSvsNs = NdndSimGetLastPfxSvsDeliveryNs();
                // NdndSimGetLastPfxSvsDeliveryNs returns -1 before the first prefix
                // SVS delivery.  The p=0 case (no prefixes at all) falls back to
                // startNs so the checker doesn't wait forever for deliveries that
                // will never come.
                int64_t refNs = startNs;
                if (lastSvsNs >= 0) {
                    refNs = lastSvsNs;
                }
                if ((nowNs - refNs) >= silenceNs)
                {
                    Simulator::Stop();
                    return;
                }
                Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
            };
            Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
            } // if (stableWindow > 0)
        }
        else if (stableWindow > 0)
        {
            // p=0 + snap-import: same SVS-silence checker as p>0.
            // No prefixes announced → lastSvsNs stays -1, refNs falls back to startNs,
            // so the checker fires exactly
            // stableWindow seconds after installation (clean fixed wait for DV
            // re-convergence after snap import).
            const int64_t silenceNs = static_cast<int64_t>(stableWindow * 1e9);
            const int64_t startNs = Simulator::Now().GetNanoSeconds();
            auto checkerPtr = std::make_shared<std::function<void()>>();
            *checkerPtr = [checkerPtr, silenceNs, startNs, traceInterval]() {
                int64_t nowNs = Simulator::Now().GetNanoSeconds();
                int64_t lastSvsNs = NdndSimGetLastPfxSvsDeliveryNs();
                int64_t refNs = startNs;
                if (lastSvsNs >= 0) {
                    refNs = lastSvsNs;
                }
                if ((nowNs - refNs) >= silenceNs)
                {
                    Simulator::Stop();
                    return;
                }
                Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
            };
            Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
        }
    }
    else
    {
        // Normal path: wait for DV routing to converge using in-flight advertisement
        // detection, then announce prefixes and optionally export a snapshot.
        if (stableWindow > 0)
        {
            const int64_t silenceNs = static_cast<int64_t>(stableWindow * 1e9);
            const int64_t startNs = Simulator::Now().GetNanoSeconds();
            auto dvConverged = std::make_shared<bool>(false);
            auto checkerPtr = std::make_shared<std::function<void()>>();
            *checkerPtr = [checkerPtr, silenceNs, startNs, traceInterval, dvConverged,
                           stableWindow, edgeNodes, numPrefixes, exportSnap]() {
                int64_t nowNs = Simulator::Now().GetNanoSeconds();
                int64_t lastAdvNs = NdndSimGetLastDvAdvReceiptNs();
                // p=0 safety net: if no advertisements have been received yet,
                // fall back to startNs so the checker fires after stableWindow.
                int64_t refNs = startNs;
                if (lastAdvNs >= 0) {
                    refNs = lastAdvNs;
                }
                if ((nowNs - refNs) >= silenceNs)
                {
                    *dvConverged = true;
                    // DV convergence detected: export snapshot and/or announce prefixes.
                    if (!exportSnap.empty())
                    {
                        int rc = NdndSimExportSnapshot(exportSnap.c_str());
                        NS_ABORT_MSG_IF(rc != 0,
                                        "NdndSimExportSnapshot failed for: " << exportSnap);
                    }
                    if (numPrefixes > 0)
                    {
                        for (int i = 0; i < numPrefixes; ++i)
                        {
                            Ptr<Node> node = edgeNodes.at(static_cast<size_t>(i) % edgeNodes.size());
                            std::string nodeName = Names::FindName(node);
                            auto stack = node->GetObject<NdndStack>();
                            std::string prefix = "/data/" + nodeName + "/pfx" + std::to_string(i);
                            stack->RegisterProducer(prefix);
                        }
                        // After DV convergence and prefix announcement, start stage 2
                        // prefix-SVS silence checker so the sim stops when prefixes have
                        // propagated (or after stableWindow of silence if no prefixes).
                        if (stableWindow > 0)
                        {
                            const int64_t prefixSilenceNs = static_cast<int64_t>(stableWindow * 1e9);
                            const int64_t prefixStartNs = Simulator::Now().GetNanoSeconds();
                            auto pfxCheckerPtr = std::make_shared<std::function<void()>>();
                            *pfxCheckerPtr = [pfxCheckerPtr, prefixSilenceNs, prefixStartNs,
                                              traceInterval]() {
                                int64_t nowNs = Simulator::Now().GetNanoSeconds();
                                int64_t lastSvsNs = NdndSimGetLastPfxSvsDeliveryNs();
                                int64_t refNs = prefixStartNs;
                                if (lastSvsNs >= 0) {
                                    refNs = lastSvsNs;
                                }
                                if ((nowNs - refNs) >= prefixSilenceNs)
                                {
                                    Simulator::Stop();
                                    return;
                                }
                                Simulator::Schedule(Seconds(traceInterval), *pfxCheckerPtr);
                            };
                            Simulator::Schedule(Seconds(traceInterval), *pfxCheckerPtr);
                        }
                        return;
                    }
                    else
                    {
                        // No prefixes: keep running until convergence metric stabilises
                        // for snapshot export.
                        if (!exportSnap.empty())
                        {
                            auto lastMetric = std::make_shared<int64_t>(-1);
                            auto stableFor  = std::make_shared<double>(0.0);
                            auto snapCheckerPtr = std::make_shared<std::function<void()>>();
                            *snapCheckerPtr = [snapCheckerPtr, exportSnap, stableWindow,
                                              traceInterval, lastMetric, stableFor]() {
                                int64_t cur = NdndSimGetConvergenceMetric();
                                if (cur == *lastMetric)
                                {
                                    *stableFor += traceInterval;
                                }
                                else
                                {
                                    *lastMetric = cur;
                                    *stableFor  = 0.0;
                                }
                                if (*stableFor >= stableWindow)
                                {
                                    int rc = NdndSimExportSnapshot(exportSnap.c_str());
                                    NS_ABORT_MSG_IF(rc != 0,
                                                    "NdndSimExportSnapshot failed for: " << exportSnap);
                                    Simulator::Stop();
                                    return;
                                }
                                Simulator::Schedule(Seconds(traceInterval), *snapCheckerPtr);
                            };
                            Simulator::Schedule(Seconds(traceInterval), *snapCheckerPtr);
                        }
                    }
                    return;
                }
                Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
            };
            Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
        }
        else if (numPrefixes > 0)
        {
            // stableWindow <= 0: no DV convergence check; announce prefixes immediately.
            for (int i = 0; i < numPrefixes; ++i)
            {
                Ptr<Node> node = edgeNodes.at(static_cast<size_t>(i) % edgeNodes.size());
                std::string nodeName = Names::FindName(node);
                auto stack = node->GetObject<NdndStack>();
                std::string prefix = "/data/" + nodeName + "/pfx" + std::to_string(i);
                stack->RegisterProducer(prefix);
            }
        }
    }

    // Drop counter: count MacTxDrop (queue-full drops) across all devices.
    uint64_t totalDrops = 0;
    if (!dropTrace.empty())
    {
        for (uint32_t i = 0; i < nodes.GetN(); ++i)
        {
            Ptr<Node> node = nodes.Get(i);
            for (uint32_t j = 0; j < node->GetNDevices(); ++j)
            {
                node->GetDevice(j)->TraceConnectWithoutContext(
                    "MacTxDrop",
                    MakeBoundCallback(&CountMacTxDrop, &totalDrops));
            }
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

        // DV routing convergence time: in-flight advertisement silence timestamp.
        // Returns -1 if no advertisements were ever received.
        int64_t convNs = NdndSimGetLastDvAdvReceiptNs();
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

    if (!dropTrace.empty())
    {
        std::ofstream ofs(dropTrace);
        NS_ABORT_MSG_IF(!ofs, "Failed to open dropTrace output: " << dropTrace);
        ofs << totalDrops << "\n";
    }

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
    return 0;
}