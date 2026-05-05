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
    int targetNodes = 0; // 0 = use all nodes (onephase); >0 overrides multiplier (twophase: edge nodes)
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
    cmd.AddValue("targetNodes",
                 "Node count multiplier for target-count checker (stableWindow==0). "
                 "0 (default) = use total node count (onephase). "
                 "Set to number of edge nodes for twophase (PET is only on edge nodes).",
                 targetNodes);
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
            // For the target-count checker (stableWindow == 0), delay prefix
            // announcements so the baseline FIB count can be read cleanly.
            // At t=0 the snapshot-import's pending RIB→FIB events fire AND the
            // RegisterProducer calls add local FIB entries — both at the same
            // simulated time, so there is no safe point to read a DV-only
            // baseline after t=0 if announcements are also at t=0.
            // Delaying announcements to t=announceDelay (0.5 s) lets us read
            // the true DV-only FIB sum at t=announceDelay-traceInterval, well
            // before any prefix entries exist.  The stability-window checker
            // (stableWindow > 0) is unaffected because it detects convergence
            // by watching the metric rise and stabilise, not by knowing the
            // exact target count.
            const double announceDelay = (stableWindow == 0.0) ? 0.5 : 0.0;
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

            // Install an event-driven stop condition based on stableWindow:
            //
            //   stableWindow > 0  →  advertisement-silence checker (twophase):
            //     Stops once no DV heartbeat has fired for stableWindow seconds
            //     AND the convergence metric has risen above its baseline.
            //     Set stableWindow = adv_interval + epsilon (e.g. 1.1 s for a
            //     1 s adv interval): after the last advertisement, all nodes
            //     will have processed it within one adv_interval, so silence
            //     for slightly longer than adv_interval is sufficient proof of
            //     convergence regardless of topology diameter.
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
            // Prefix-activity-silence checker: stop once the convergence metric
            // has risen above baseline AND no prefix SVS publication has been
            // received by any node for silenceNs nanoseconds.
            //
            // We watch NdndSimGetLastPfxActivityNs() — the last sim time any
            // node successfully fetched and applied a remote prefix Data packet
            // — rather than the metric value itself.  The metric can stabilise
            // while nodes are still mid-fetch (e.g. rf206 waiting for an SVS
            // retry), but the activity timestamp advances on every Data delivery.
            // True silence means no prefix Data is in-flight: all pending SVS
            // fetches have completed or exhausted retries.
            //
            // stableWindow should be set to SVS_periodic_timeout + epsilon
            // (typically pfx_sync_interval + a few hundred ms) so that at least
            // one full SVS sync round can fire after the last Data arrival before
            // we declare convergence.
            const int64_t silenceNs = static_cast<int64_t>(stableWindow * 1e9);
            auto baseCount = std::make_shared<int64_t>(-1); // -1 = not yet captured
            auto checkerPtr = std::make_shared<std::function<void()>>();
            *checkerPtr = [checkerPtr, baseCount, silenceNs, traceInterval]() {
                int64_t raw = NdndSimGetConvergenceMetric();
                if (*baseCount < 0)
                {
                    *baseCount = raw;
                    Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
                    return;
                }
                int64_t nowNs = Simulator::Now().GetNanoSeconds();
                int64_t lastPfxNs = NdndSimGetLastPfxActivityNs();
                // Stop once metric has risen above baseline and prefix activity
                // has been silent for silenceNs.
                if (raw > *baseCount && lastPfxNs > 0 && (nowNs - lastPfxNs) >= silenceNs)
                {
                    Simulator::Stop();
                    return;
                }
                Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
            };
            Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
            } // if (stableWindow > 0)
            else if (stableWindow == 0.0)
            {
            // Target-count checker: read DV-only baseline just BEFORE the
            // delayed prefix announcements fire (at t = announceDelay -
            // traceInterval), then stop once the global FIB sum reaches
            // baseCount + numPrefixes * numNodes.
            //
            // Why delayed baseline?  RegisterProducer fires at t=announceDelay
            // and immediately installs local FIB entries on the announcing edge
            // nodes (via nfdc.simExec → synchronous ExecMgmtCmd).  Reading the
            // baseline at t=announceDelay-traceInterval guarantees:
            //   • All snapshot-import t=0 RIB→FIB events have fired (they are
            //     t=0 DES events; announceDelay-traceInterval >> 0).
            //   • No prefix FIB entries yet (announcements haven't started).
            // Target = baseCount + numPrefixes × numNodes is then exactly the
            // fully-converged FIB sum, independent of topology size or timing.
            const int64_t multiplier = (targetNodes > 0)
                ? static_cast<int64_t>(targetNodes)
                : static_cast<int64_t>(nodes.GetN());
            const int64_t targetDelta = static_cast<int64_t>(numPrefixes) * multiplier;
            auto baseCount = std::make_shared<int64_t>(-1);
            const double captureTime = announceDelay - traceInterval;
            Simulator::Schedule(Seconds(captureTime), [baseCount]() {
                *baseCount = NdndSimGetConvergenceMetric();
            });
            auto checkerPtr = std::make_shared<std::function<void()>>();
            *checkerPtr = [checkerPtr, baseCount, targetDelta, traceInterval]() {
                if (*baseCount < 0)
                {
                    // Baseline not yet captured; shouldn't happen but be safe.
                    Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
                    return;
                }
                int64_t raw = NdndSimGetConvergenceMetric();
                if (raw >= *baseCount + targetDelta)
                {
                    Simulator::Stop();
                    return;
                }
                Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
            };
            // Start polling after announcements have fired.
            Simulator::Schedule(Seconds(announceDelay + traceInterval), *checkerPtr);
            } // else if (stableWindow == 0.0)
        }
        else if (stableWindow > 0)
        {
            // p0 + snap-import: no prefix announcements, so the target-count and
            // stability-window checkers above are skipped.  Install a simpler
            // "stable-only" poller: stop once the FIB/PET metric has been
            // unchanged for stableWindow seconds (no requirement to rise, since
            // zero prefixes means PET may stay at 0 the whole time).
            const int stableRoundsNeeded =
                std::max(1, static_cast<int>(stableWindow / traceInterval));
            auto lastCount = std::make_shared<int64_t>(-1);
            auto stableRounds = std::make_shared<int>(0);
            auto checkerPtr = std::make_shared<std::function<void()>>();
            *checkerPtr = [checkerPtr, lastCount, stableRounds,
                           traceInterval, stableRoundsNeeded]() {
                int64_t raw = NdndSimGetConvergenceMetric();
                if (*lastCount < 0)
                {
                    *lastCount = raw;
                    Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
                    return;
                }
                if (raw == *lastCount)
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
            // After DV routing converges, keep running until the SVS-ALO
            // metric (PET sum for twophase, FIB sum for onephase) stabilises.
            // Stopping immediately at routing convergence would capture an
            // underconverged PET/PfxEntries state on larger topologies where
            // SVS propagation takes longer than DV convergence.  The same
            // stability-window poller used in stage-2 is reused here so the
            // snapshot timing is correct regardless of topology size or delay.
            RegisterRoutingConvergedCallback([exportSnap, stableWindow, traceInterval]() {
                auto lastMetric = std::make_shared<int64_t>(-1);
                auto stableFor  = std::make_shared<double>(0.0);
                auto checkerPtr = std::make_shared<std::function<void()>>();
                *checkerPtr = [checkerPtr, exportSnap, stableWindow, traceInterval,
                                lastMetric, stableFor]() {
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
                    Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
                };
                Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
            });
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