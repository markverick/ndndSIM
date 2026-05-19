/*
 * ndndsim-atlas-routing-scenario.cc — Routing-only scenario (no app traffic)
 *
 * Reads a topology file, installs NDNd with DV routing, and measures
 * only routing protocol traffic (packet counts, bytes, convergence time).
 * No consumer or producer applications are installed.
 *
 * Convergence is measured event-driven: the Go DV code fires a
 * RouterReachableEvent each time a node first learns a route to another
 * router.  Convergence time = (last such event that completes all FIBs)
 *                            − (first such event across all nodes).
 *
 * Usage:
 *   ./ns3 run "ndndsim-atlas-routing-scenario --topo=path/to/topo.txt"
 */

#include "ns3/core-module.h"
#include "ns3/names.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

// ndndSIM headers
#include "ns3/ndndsim-link-tracer.h"
#include "ns3/ndndsim-stack-helper.h"
#include "ns3/ndndsim-stack.h"
#include "ns3/ndndsim-topology-reader.h"
#include "ns3/ndndsim-go-bridge.h"

#include <functional>
#include <fstream>

using namespace ns3;
using namespace ns3::ndndsim;

int
main(int argc, char* argv[])
{
    std::string topoFile;
    std::string linkTrace;
    std::string packetTrace;
    std::string convTrace;
    std::string dvConfig;
    std::string network = "/minindn";
    double simTime = 30.0;
    double traceInterval = 0.05; // 50 ms
    int numPrefixes = 0;         // prefixes per node to announce (0 = none)
    int totalPrefixes = -1;      // total prefixes to introduce from node0
    double prefixIntroduceTime = 1.0;

    CommandLine cmd;
    cmd.AddValue("topo", "Topology file path (required)", topoFile);
    cmd.AddValue("linkTrace", "Output link traffic CSV path", linkTrace);
    cmd.AddValue("packetTrace", "Output per-packet event CSV path", packetTrace);
    cmd.AddValue("convTrace", "Output convergence time file path", convTrace);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.AddValue("traceInterval", "Rate trace sampling interval in seconds", traceInterval);
    cmd.AddValue("dvConfig", "DV config JSON overlay (overrides defaults)", dvConfig);
    cmd.AddValue("network", "DV network prefix (default: /minindn)", network);
    cmd.AddValue("numPrefixes",
                 "Number of prefixes each node announces (0 = none)", numPrefixes);
    cmd.AddValue("totalPrefixes",
                 "Total prefixes to introduce from node0 at prefixIntroduceTime", totalPrefixes);
    cmd.AddValue("prefixIntroduceTime",
                 "Time in seconds to introduce totalPrefixes from node0", prefixIntroduceTime);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_IF(topoFile.empty(), "--topo is required");

    // ─── Topology ──────────────────────────────────────────────────

    NdndTopologyReader reader;
    reader.SetFileName(topoFile);
    NodeContainer nodes = reader.Read();
    NS_ABORT_MSG_IF(nodes.GetN() == 0, "No nodes read from topology file");

    // ─── NDNd Stack + DV Routing ───────────────────────────────────

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);
    NdndStackHelper::EnableDvRouting(network, nodes, dvConfig);

    if (totalPrefixes > 0)
    {
        Simulator::Schedule(Seconds(prefixIntroduceTime), [nodes, totalPrefixes]() {
            auto stack = nodes.Get(0)->GetObject<NdndStack>();
            for (int j = 0; j < totalPrefixes; ++j)
            {
                std::string pfx = "/data/node0/pfx" + std::to_string(j);
                stack->AnnouncePrefixToDv(pfx);
            }
        });
    }

    // Legacy mode: announce generated prefixes on each node after DV convergence.
    // Uses in-flight DV advertisement silence detection via polling.
    if (numPrefixes > 0)
    {
        NdndSimSetTotalNodes(static_cast<int>(nodes.GetN()));
        const int64_t silenceNs = static_cast<int64_t>(2.0 * 1e9); // 2s silence
        const int64_t startNs = Simulator::Now().GetNanoSeconds();
        auto checkerPtr = std::make_shared<std::function<void()>>();
        *checkerPtr = [checkerPtr, silenceNs, startNs, traceInterval, nodes, numPrefixes]() {
            int64_t nowNs = Simulator::Now().GetNanoSeconds();
            int64_t lastAdvNs = NdndSimGetLastDvAdvReceiptNs();
            int64_t refNs = (lastAdvNs >= 0) ? lastAdvNs : startNs;
            if ((nowNs - refNs) >= silenceNs)
            {
                // DV converged: announce prefixes
                for (uint32_t i = 0; i < nodes.GetN(); ++i)
                {
                    auto stack = nodes.Get(i)->GetObject<NdndStack>();
                    for (int j = 0; j < numPrefixes; ++j)
                    {
                        std::string pfx =
                            "/data/node" + std::to_string(i) + "/pfx" + std::to_string(j);
                        stack->AnnouncePrefixToDv(pfx);
                    }
                }
                return;
            }
            Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
        };
        Simulator::Schedule(Seconds(traceInterval), *checkerPtr);
    }

    // No consumer or producer — pure routing traffic measurement.

    // ─── Link Traffic Tracer ───────────────────────────────────────

    std::shared_ptr<NdndLinkTracer> linkTracer;
    if (!linkTrace.empty())
    {
        linkTracer = NdndLinkTracer::Create(linkTrace, Seconds(traceInterval), "", "", false);
        for (const auto& link : reader.GetLinks())
        {
            std::string a = Names::FindName(link.fromNode);
            std::string b = Names::FindName(link.toNode);
            linkTracer->ConnectLink(link.devices, a, b);
        }
    }

    // ─── Per-Packet Event Tracer ───────────────────────────────────

    std::shared_ptr<NdndLinkTracer> pktTracer;
    if (!packetTrace.empty())
    {
        pktTracer = NdndLinkTracer::CreatePerPacket(packetTrace, "", "", false);
        for (const auto& link : reader.GetLinks())
        {
            std::string a = Names::FindName(link.fromNode);
            std::string b = Names::FindName(link.toNode);
            pktTracer->ConnectLink(link.devices, a, b);
        }
    }

    // ─── Run ───────────────────────────────────────────────────────

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // Write convergence time: in-flight DV advertisement silence detection.
    // Uses NdndSimGetLastDvAdvReceiptNs which tracks when DV advertisements
    // pass through any node (in-flight), not at-rest.
    if (!convTrace.empty())
    {
        std::ofstream ofs(convTrace);
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

    if (linkTracer)
    {
        linkTracer->Stop();
    }

    if (pktTracer)
    {
        pktTracer->Stop();
    }

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();

    return 0;
}
