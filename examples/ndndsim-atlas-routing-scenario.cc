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

    CommandLine cmd;
    cmd.AddValue("topo", "Topology file path (required)", topoFile);
    cmd.AddValue("linkTrace", "Output link traffic CSV path", linkTrace);
    cmd.AddValue("packetTrace", "Output per-packet event CSV path", packetTrace);
    cmd.AddValue("convTrace", "Output convergence time file path", convTrace);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.AddValue("traceInterval", "Rate trace sampling interval in seconds", traceInterval);
    cmd.AddValue("dvConfig", "DV config JSON overlay (overrides defaults)", dvConfig);
    cmd.AddValue("network", "DV network prefix (default: /minindn)", network);
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

    // No consumer or producer — pure routing traffic measurement.
    // Convergence is tracked event-driven via RouterReachableEvent
    // in the Go DV code (no probe prefix needed).

    // ─── Link Traffic Tracer ───────────────────────────────────────

    std::shared_ptr<NdndLinkTracer> linkTracer;
    if (!linkTrace.empty())
    {
        linkTracer = NdndLinkTracer::Create(linkTrace, Seconds(traceInterval));
        for (const auto& link : reader.GetLinks())
        {
            linkTracer->ConnectLink(link.devices);
        }
    }

    // ─── Per-Packet Event Tracer ───────────────────────────────────

    std::shared_ptr<NdndLinkTracer> pktTracer;
    if (!packetTrace.empty())
    {
        pktTracer = NdndLinkTracer::CreatePerPacket(packetTrace);
        for (const auto& link : reader.GetLinks())
        {
            pktTracer->ConnectLink(link.devices);
        }
    }

    // ─── Run ───────────────────────────────────────────────────────

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // Write convergence time: event-driven measurement from Go.
    // Returns the span from the first RouterReachable event to the
    // event that makes all N nodes have routes to all N-1 others.
    if (!convTrace.empty())
    {
        std::ofstream ofs(convTrace);
        int64_t convNs = NdndSimGetRoutingConvergenceNs(
            static_cast<int>(nodes.GetN()));
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
