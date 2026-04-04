/*
 * ndndsim-atlas-churn-scenario.cc — Two-phase routing churn scenario.
 *
 * Phase 1 (convergence): Boot → DV convergence → announce prefixes → stabilize.
 * Phase 2 (churn):       Scheduled dynamic events — link fail/recover, prefix
 *                         withdraw/re-announce.
 *
 * Measurement: per-packet event tracer + interval link tracer run continuously.
 * Python driver splits traffic by phase using the known phase boundary time.
 *
 * Churn events are passed via --churnEvents as a JSON array:
 *   [{"time":20.0,"type":"link_down","src":"n0_0","dst":"n0_1"},
 *    {"time":25.0,"type":"link_up","src":"n0_0","dst":"n0_1"},
 *    {"time":20.0,"type":"neighbor_down","src":"n0_0","dst":"n0_1"},
 *    {"time":25.0,"type":"neighbor_up","src":"n0_0","dst":"n0_1"},
 *    {"time":22.0,"type":"prefix_withdraw","node":"n0_0","prefix":"/data/n0_0/pfx0"},
 *    {"time":27.0,"type":"prefix_announce","node":"n0_0","prefix":"/data/n0_0/pfx0"}]
 *
 * Usage:
 *   ./ns3 run "ndndsim-atlas-churn-scenario --topo=... --churnEvents='[...]'"
 */

#include "ns3/core-module.h"
#include "ns3/error-model.h"
#include "ns3/names.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include "ns3/ndndsim-link-tracer.h"
#include "ns3/ndndsim-stack-helper.h"
#include "ns3/ndndsim-stack.h"
#include "ns3/ndndsim-topology-reader.h"
#include "ns3/ndndsim-go-bridge.h"

#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

using namespace ns3;
using namespace ns3::ndndsim;

// ─── Minimal JSON helpers (no external deps) ──────────────────────

// Tiny JSON array-of-objects parser for churn events.
// Only supports flat objects with string values (all we need here).
struct ChurnEvent
{
    double time;
    std::string type;
    std::map<std::string, std::string> fields;
};

static std::string
Trim(const std::string& s)
{
    auto a = s.find_first_not_of(" \t\n\r\"");
    auto b = s.find_last_not_of(" \t\n\r\"");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

static std::vector<ChurnEvent>
ParseChurnEvents(const std::string& json)
{
    std::vector<ChurnEvent> events;
    if (json.empty())
        return events;

    // Walk character-by-character for robustness
    size_t pos = json.find('[');
    if (pos == std::string::npos)
        return events;
    pos++; // skip '['

    while (pos < json.size())
    {
        // Find next '{'
        pos = json.find('{', pos);
        if (pos == std::string::npos)
            break;
        size_t objEnd = json.find('}', pos);
        if (objEnd == std::string::npos)
            break;

        std::string obj = json.substr(pos + 1, objEnd - pos - 1);
        pos = objEnd + 1;

        // Parse key:value pairs
        ChurnEvent ev;
        ev.time = 0;
        std::istringstream ss(obj);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            auto colon = token.find(':');
            if (colon == std::string::npos)
                continue;
            std::string key = Trim(token.substr(0, colon));
            std::string val = Trim(token.substr(colon + 1));
            if (key == "time")
                ev.time = std::stod(val);
            else if (key == "type")
                ev.type = val;
            else
                ev.fields[key] = val;
        }
        events.push_back(ev);
    }
    return events;
}

// ─── Link error model registry ────────────────────────────────────

struct LinkErrorModels
{
    Ptr<RateErrorModel> fwd; // from → to
    Ptr<RateErrorModel> rev; // to → from
};

struct LinkInterfaces
{
    uint32_t fromIf;
    uint32_t toIf;
};

// Key: "srcName:dstName" (alphabetically ordered)
static std::map<std::string, LinkErrorModels> g_linkErrors;
static std::map<std::string, LinkInterfaces> g_linkInterfaces;

static std::string
LinkKey(const std::string& a, const std::string& b)
{
    return (a < b) ? (a + ":" + b) : (b + ":" + a);
}

static void
DoLinkDown(const std::string& src, const std::string& dst)
{
    auto key = LinkKey(src, dst);
    auto it = g_linkErrors.find(key);
    NS_ABORT_MSG_IF(it == g_linkErrors.end(),
                    "Link error models not found for " << src << "--" << dst);
    std::cout << Simulator::Now().GetSeconds() << "s: LINK DOWN " << src << "--" << dst << std::endl;
    it->second.fwd->SetRate(1.0);
    it->second.rev->SetRate(1.0);
}

static void
DoLinkUp(const std::string& src, const std::string& dst)
{
    auto key = LinkKey(src, dst);
    auto it = g_linkErrors.find(key);
    NS_ABORT_MSG_IF(it == g_linkErrors.end(),
                    "Link error models not found for " << src << "--" << dst);
    std::cout << Simulator::Now().GetSeconds() << "s: LINK UP " << src << "--" << dst << std::endl;
    it->second.fwd->Disable();
    it->second.rev->Disable();
}

static void
DoNeighborDown(const std::string& src, const std::string& dst)
{
    auto key = LinkKey(src, dst);
    auto it = g_linkInterfaces.find(key);
    NS_ABORT_MSG_IF(it == g_linkInterfaces.end(),
                    "Link interface state not found for " << src << "--" << dst);

    auto srcNode = Names::Find<Node>(src);
    auto dstNode = Names::Find<Node>(dst);
    NS_ABORT_MSG_IF(!srcNode || !dstNode, "Neighbor-down nodes not found for " << src << "--" << dst);

    auto srcStack = srcNode->GetObject<NdndStack>();
    auto dstStack = dstNode->GetObject<NdndStack>();
    NS_ABORT_MSG_IF(!srcStack || !dstStack, "NDNd stack missing for " << src << "--" << dst);

    std::cout << Simulator::Now().GetSeconds() << "s: NEIGHBOR DOWN " << src << "--" << dst << std::endl;
    srcStack->DeactivateInterface(it->second.fromIf);
    dstStack->DeactivateInterface(it->second.toIf);
}

static void
DoNeighborUp(const std::string& src, const std::string& dst)
{
    auto key = LinkKey(src, dst);
    auto it = g_linkInterfaces.find(key);
    NS_ABORT_MSG_IF(it == g_linkInterfaces.end(),
                    "Link interface state not found for " << src << "--" << dst);

    auto srcNode = Names::Find<Node>(src);
    auto dstNode = Names::Find<Node>(dst);
    NS_ABORT_MSG_IF(!srcNode || !dstNode, "Neighbor-up nodes not found for " << src << "--" << dst);

    auto srcStack = srcNode->GetObject<NdndStack>();
    auto dstStack = dstNode->GetObject<NdndStack>();
    NS_ABORT_MSG_IF(!srcStack || !dstStack, "NDNd stack missing for " << src << "--" << dst);

    std::cout << Simulator::Now().GetSeconds() << "s: NEIGHBOR UP " << src << "--" << dst << std::endl;
    srcStack->ReactivateInterface(it->second.fromIf);
    dstStack->ReactivateInterface(it->second.toIf);
}

static void
DoPrefixAnnounce(Ptr<Node> node, const std::string& prefix)
{
    auto stack = node->GetObject<NdndStack>();
    std::cout << Simulator::Now().GetSeconds() << "s: PREFIX ANNOUNCE "
              << Names::FindName(node) << " " << prefix << std::endl;
    stack->AnnouncePrefixToDv(prefix);
}

static void
DoPrefixWithdraw(Ptr<Node> node, const std::string& prefix)
{
    auto stack = node->GetObject<NdndStack>();
    std::cout << Simulator::Now().GetSeconds() << "s: PREFIX WITHDRAW "
              << Names::FindName(node) << " " << prefix << std::endl;
    stack->WithdrawPrefixFromDv(prefix);
}

// ─── Event log ─────────────────────────────────────────────────────

struct EventLogEntry
{
    double time;
    std::string type;
    std::string details;
};
static std::vector<EventLogEntry> g_eventLog;

struct SuppressionNodeStats
{
    std::string node;
    bool available;
    uint64_t enter;
    uint64_t ok;
    uint64_t fail;
};

struct SuppressionSnapshot
{
    std::vector<SuppressionNodeStats> nodes;
    uint64_t totalEnter = 0;
    uint64_t totalOk = 0;
    uint64_t totalFail = 0;
};

static std::unique_ptr<SuppressionSnapshot> g_suppressionStart;
static double g_suppressionStartTime = -1.0;

static void
LogEvent(double t, const std::string& type, const std::string& details)
{
    g_eventLog.push_back({t, type, details});
}

static SuppressionSnapshot
CollectSuppressionSnapshot(const NodeContainer& nodes)
{
    SuppressionSnapshot snapshot;
    for (uint32_t i = 0; i < nodes.GetN(); ++i)
    {
        auto node = nodes.Get(i);
        auto stack = node->GetObject<NdndStack>();
        uint64_t enter = 0;
        uint64_t ok = 0;
        uint64_t fail = 0;
        bool available = stack && stack->GetDvSuppressionStats(enter, ok, fail);
        snapshot.nodes.push_back({
            Names::FindName(node),
            available,
            enter,
            ok,
            fail,
        });
        snapshot.totalEnter += enter;
        snapshot.totalOk += ok;
        snapshot.totalFail += fail;
    }
    return snapshot;
}

static void
WriteSuppressionNodeArray(std::ofstream& ofs,
                         const std::vector<SuppressionNodeStats>& nodes,
                         const SuppressionSnapshot* startSnapshot)
{
    ofs << "[\n";
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        const auto& endNode = nodes[i];
        uint64_t startEnter = 0;
        uint64_t startOk = 0;
        uint64_t startFail = 0;
        bool available = endNode.available;
        if (startSnapshot != nullptr && i < startSnapshot->nodes.size())
        {
            const auto& startNode = startSnapshot->nodes[i];
            startEnter = startNode.enter;
            startOk = startNode.ok;
            startFail = startNode.fail;
            available = available || startNode.available;
        }

        uint64_t enter = endNode.enter - startEnter;
        uint64_t ok = endNode.ok - startOk;
        uint64_t fail = endNode.fail - startFail;

        ofs << "      {\"node\":\"" << endNode.node << "\",";
        ofs << "\"available\":" << (available ? "true" : "false") << ",";
        ofs << "\"enter\":" << enter << ",";
        ofs << "\"ok\":" << ok << ",";
        ofs << "\"fail\":" << fail << ",";
        ofs << "\"unresolved\":" << (enter - ok - fail) << "}";
        if (i + 1 != nodes.size())
        {
            ofs << ",";
        }
        ofs << "\n";
    }
    ofs << "    ]";
}

static void
WriteSuppressionSnapshot(std::ofstream& ofs, const SuppressionSnapshot& snapshot)
{
    ofs << "{\n";
    ofs << "      \"nodes\": [\n";
    for (size_t i = 0; i < snapshot.nodes.size(); ++i)
    {
        const auto& node = snapshot.nodes[i];
        ofs << "        {\"node\":\"" << node.node << "\",";
        ofs << "\"available\":" << (node.available ? "true" : "false") << ",";
        ofs << "\"enter\":" << node.enter << ",";
        ofs << "\"ok\":" << node.ok << ",";
        ofs << "\"fail\":" << node.fail << ",";
        ofs << "\"unresolved\":" << (node.enter - node.ok - node.fail) << "}";
        if (i + 1 != snapshot.nodes.size())
        {
            ofs << ",";
        }
        ofs << "\n";
    }
    ofs << "      ],\n";
    ofs << "      \"aggregate\": {";
    ofs << "\"enter\":" << snapshot.totalEnter << ",";
    ofs << "\"ok\":" << snapshot.totalOk << ",";
    ofs << "\"fail\":" << snapshot.totalFail << ",";
    ofs << "\"unresolved\":"
        << (snapshot.totalEnter - snapshot.totalOk - snapshot.totalFail) << "}\n";
    ofs << "    }";
}

static void
WriteSuppressionTrace(const SuppressionSnapshot* startSnapshot,
                     const SuppressionSnapshot& endSnapshot,
                     double startTime,
                     double endTime,
                     const std::string& path)
{
    std::ofstream ofs(path);
    const SuppressionSnapshot emptyStart;
    const auto& start = startSnapshot != nullptr ? *startSnapshot : emptyStart;
    uint64_t deltaEnter = endSnapshot.totalEnter - start.totalEnter;
    uint64_t deltaOk = endSnapshot.totalOk - start.totalOk;
    uint64_t deltaFail = endSnapshot.totalFail - start.totalFail;

    ofs << "{\n";
    ofs << "  \"phase\": \"churn\",\n";
    ofs << "  \"window\": {";
    ofs << "\"start_s\":" << startTime << ",";
    ofs << "\"end_s\":" << endTime << ",";
    ofs << "\"duration_s\":" << (endTime - startTime) << "},\n";
    ofs << "  \"nodes\": ";
    WriteSuppressionNodeArray(ofs, endSnapshot.nodes, startSnapshot);
    ofs << ",\n";
    ofs << "  \"aggregate\": {";
    ofs << "\"enter\":" << deltaEnter << ",";
    ofs << "\"ok\":" << deltaOk << ",";
    ofs << "\"fail\":" << deltaFail << ",";
    ofs << "\"unresolved\":" << (deltaEnter - deltaOk - deltaFail) << "},\n";
    ofs << "  \"snapshots\": {\n";
    ofs << "    \"start\": ";
    WriteSuppressionSnapshot(ofs, start);
    ofs << ",\n";
    ofs << "    \"end\": ";
    WriteSuppressionSnapshot(ofs, endSnapshot);
    ofs << "\n  }\n";
    ofs << "}\n";
}

// Actual churn start time (set when churn events are first scheduled)
static double g_churnStartTime = -1.0;

/**
 * Schedule all parsed churn events.
 *
 * In legacy mode (baseTime == 0): event times are absolute sim-times,
 *   scheduled with Simulator::Schedule(Seconds(ev.time), ...) from time 0.
 * In deferred mode: event times are relative offsets, and this function is
 *   called from within the sim at the desired start moment, so we use
 *   Simulator::Schedule(Seconds(ev.time), ...) which adds the offset to Now().
 *
 * The logged absolute time is always baseTime + ev.time.
 */
static void
ScheduleChurnEvents(const std::vector<ChurnEvent>& events, double baseTime)
{
    g_churnStartTime = baseTime;
    for (const auto& ev : events)
    {
        double logTime = baseTime + ev.time;
        if (ev.type == "link_down")
        {
            auto src = ev.fields.at("src");
            auto dst = ev.fields.at("dst");
            Simulator::Schedule(Seconds(ev.time), &DoLinkDown, src, dst);
            LogEvent(logTime, "link_down", src + "--" + dst);
        }
        else if (ev.type == "link_up")
        {
            auto src = ev.fields.at("src");
            auto dst = ev.fields.at("dst");
            Simulator::Schedule(Seconds(ev.time), &DoLinkUp, src, dst);
            LogEvent(logTime, "link_up", src + "--" + dst);
        }
        else if (ev.type == "neighbor_down")
        {
            auto src = ev.fields.at("src");
            auto dst = ev.fields.at("dst");
            Simulator::Schedule(Seconds(ev.time), &DoNeighborDown, src, dst);
            LogEvent(logTime, "neighbor_down", src + "--" + dst);
        }
        else if (ev.type == "neighbor_up")
        {
            auto src = ev.fields.at("src");
            auto dst = ev.fields.at("dst");
            Simulator::Schedule(Seconds(ev.time), &DoNeighborUp, src, dst);
            LogEvent(logTime, "neighbor_up", src + "--" + dst);
        }
        else if (ev.type == "prefix_withdraw")
        {
            auto nodeName = ev.fields.at("node");
            auto pfx = ev.fields.at("prefix");
            Ptr<Node> node = Names::Find<Node>(nodeName);
            NS_ABORT_MSG_IF(!node, "Churn event: node not found: " << nodeName);
            Simulator::Schedule(Seconds(ev.time), &DoPrefixWithdraw, node, pfx);
            LogEvent(logTime, "prefix_withdraw", nodeName + " " + pfx);
        }
        else if (ev.type == "prefix_announce")
        {
            auto nodeName = ev.fields.at("node");
            auto pfx = ev.fields.at("prefix");
            Ptr<Node> node = Names::Find<Node>(nodeName);
            NS_ABORT_MSG_IF(!node, "Churn event: node not found: " << nodeName);
            Simulator::Schedule(Seconds(ev.time), &DoPrefixAnnounce, node, pfx);
            LogEvent(logTime, "prefix_announce", nodeName + " " + pfx);
        }
        else
        {
            NS_ABORT_MSG("Unknown churn event type: " << ev.type);
        }
    }
    std::cout << Simulator::Now().GetSeconds() << "s: CHURN PHASE STARTS ("
              << events.size() << " events scheduled)" << std::endl;
    LogEvent(baseTime, "churn_start", std::to_string(events.size()) + "_events");
}

// ─── Main ──────────────────────────────────────────────────────────

int
main(int argc, char* argv[])
{
    std::string topoFile;
    std::string linkTrace;
    std::string packetTrace;
    std::string convTrace;
    std::string eventLogFile;
    std::string churnStartTrace;
    std::string suppressTrace;
    double suppressPhaseStart = -1.0;
    std::string dvConfig;
    std::string churnEventsJson;
    std::string network = "/minindn";
    double simTime = 60.0;
    double traceInterval = 0.05;
    int numPrefixes = 0;
    bool churnAfterConvergence = false;
    double churnMargin = 10.0;
    double churnDuration = 0.0;  // 0 = use simTime as fixed end

    CommandLine cmd;
    cmd.AddValue("topo", "Topology file path (required)", topoFile);
    cmd.AddValue("linkTrace", "Output link traffic CSV path", linkTrace);
    cmd.AddValue("packetTrace", "Output per-packet event CSV path", packetTrace);
    cmd.AddValue("convTrace", "Output convergence time file path", convTrace);
    cmd.AddValue("eventLog", "Output event log CSV path", eventLogFile);
    cmd.AddValue("churnStartTrace", "Output churn start time file path", churnStartTrace);
    cmd.AddValue("suppressTrace", "Output SVS suppression JSON path", suppressTrace);
    cmd.AddValue("suppressPhaseStart", "Churn phase start time for SVS suppression delta", suppressPhaseStart);
    cmd.AddValue("simTime", "Simulation time in seconds", simTime);
    cmd.AddValue("traceInterval", "Link trace sampling interval in seconds", traceInterval);
    cmd.AddValue("dvConfig", "DV config JSON overlay", dvConfig);
    cmd.AddValue("network", "DV network prefix", network);
    cmd.AddValue("numPrefixes", "Prefixes per node to announce after convergence", numPrefixes);
    cmd.AddValue("churnEvents", "JSON array of churn events", churnEventsJson);
    cmd.AddValue("churnAfterConvergence",
                 "Defer churn events until after DV convergence + margin", churnAfterConvergence);
    cmd.AddValue("churnMargin",
                 "Seconds to wait after convergence before starting churn", churnMargin);
    cmd.AddValue("churnDuration",
                 "Churn phase duration in seconds (0 = use simTime as fixed end)", churnDuration);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_IF(topoFile.empty(), "--topo is required");

    // Parse churn events
    auto churnEvents = ParseChurnEvents(churnEventsJson);
    g_suppressionStart.reset();
    g_suppressionStartTime = -1.0;

    // ─── Topology ──────────────────────────────────────────────────

    NdndTopologyReader reader;
    reader.SetFileName(topoFile);
    NodeContainer nodes = reader.Read();
    NS_ABORT_MSG_IF(nodes.GetN() == 0, "No nodes read from topology file");

    // ─── Install error models on ALL links (for potential churn) ───

    for (const auto& link : reader.GetLinks())
    {
        std::string from = Names::FindName(link.fromNode);
        std::string to = Names::FindName(link.toNode);
        auto key = LinkKey(from, to);

        if (g_linkErrors.find(key) == g_linkErrors.end())
        {
            LinkErrorModels lem;
            lem.fwd = CreateObject<RateErrorModel>();
            lem.fwd->SetRate(0.0);
            lem.rev = CreateObject<RateErrorModel>();
            lem.rev->SetRate(0.0);

            link.devices.Get(0)->SetAttribute("ReceiveErrorModel",
                                               PointerValue(lem.rev));
            link.devices.Get(1)->SetAttribute("ReceiveErrorModel",
                                               PointerValue(lem.fwd));
            g_linkErrors[key] = lem;
            g_linkInterfaces[key] = {
                link.devices.Get(0)->GetIfIndex(),
                link.devices.Get(1)->GetIfIndex(),
            };
        }
    }

    // ─── NDNd Stack + DV Routing ───────────────────────────────────

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);
    NdndStackHelper::EnableDvRouting(network, nodes, dvConfig);

    // Announce prefixes after DV convergence, and optionally schedule churn
    if (numPrefixes > 0 || churnAfterConvergence)
    {
        NdndSimSetTotalNodes(static_cast<int>(nodes.GetN()));
        bool collectSuppression = !suppressTrace.empty();
        RegisterRoutingConvergedCallback(
            [nodes, numPrefixes, churnAfterConvergence, churnMargin,
             churnDuration, churnEvents, collectSuppression]() {
            double now = Simulator::Now().GetSeconds();
            std::cout << now
                      << "s: DV CONVERGED — announcing " << numPrefixes
                      << " prefixes per node" << std::endl;
            LogEvent(now, "dv_converged", "");
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
            LogEvent(now, "prefixes_announced",
                     std::to_string(numPrefixes) + "_per_node");

            if (churnAfterConvergence && !churnEvents.empty())
            {
                double churnBase = now + churnMargin;
                Simulator::Schedule(Seconds(churnMargin),
                    [nodes, churnEvents, churnBase, collectSuppression]() {
                        if (collectSuppression)
                        {
                            g_suppressionStart = std::make_unique<SuppressionSnapshot>(
                                CollectSuppressionSnapshot(nodes));
                            g_suppressionStartTime = Simulator::Now().GetSeconds();
                        }
                        ScheduleChurnEvents(churnEvents, churnBase);
                    });
            }

            // Dynamic stop: always schedule when churn_after_convergence
            // so even baseline (empty events) terminates promptly.
            if (churnAfterConvergence && churnDuration > 0)
            {
                double stopDelay = churnMargin + churnDuration;
                std::cout << now << "s: scheduling sim stop at t="
                          << (now + stopDelay) << "s (conv + "
                          << churnMargin << "s margin + "
                          << churnDuration << "s churn)" << std::endl;
                Simulator::Stop(Seconds(stopDelay));
            }
        });
    }

    // ─── Schedule churn events (immediate mode — absolute times) ───

    if (!churnAfterConvergence)
    {
        if (!suppressTrace.empty() && suppressPhaseStart >= 0.0)
        {
            Simulator::Schedule(Seconds(suppressPhaseStart), [nodes]() {
                g_suppressionStart = std::make_unique<SuppressionSnapshot>(
                    CollectSuppressionSnapshot(nodes));
                g_suppressionStartTime = Simulator::Now().GetSeconds();
            });
        }
        ScheduleChurnEvents(churnEvents, 0.0);
    }

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

    // simTime acts as a hard backstop.  When churnAfterConvergence with
    // churnDuration > 0, the convergence callback schedules an earlier
    // Simulator::Stop that fires first, making the end time dynamic.
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // Write convergence time
    if (!convTrace.empty())
    {
        std::ofstream ofs(convTrace);
        int64_t convNs = NdndSimGetRoutingConvergenceNs(
            static_cast<int>(nodes.GetN()));
        if (convNs >= 0)
            ofs << (static_cast<double>(convNs) / 1e9) << std::endl;
        else
            ofs << -1 << std::endl;
    }

    // Write churn start time (for phase splitting in Python)
    if (!churnStartTrace.empty())
    {
        std::ofstream ofs(churnStartTrace);
        ofs << g_churnStartTime << std::endl;
    }

    // Write event log
    if (!eventLogFile.empty())
    {
        std::ofstream ofs(eventLogFile);
        ofs << "Time,Event,Details" << std::endl;
        for (const auto& e : g_eventLog)
        {
            ofs << e.time << "," << e.type << "," << e.details << std::endl;
        }
    }

    if (!suppressTrace.empty())
    {
        auto endSnapshot = CollectSuppressionSnapshot(nodes);
        double endTime = Simulator::Now().GetSeconds();
        double startTime = g_suppressionStart ? g_suppressionStartTime : 0.0;
        WriteSuppressionTrace(g_suppressionStart.get(), endSnapshot, startTime, endTime,
                              suppressTrace);
    }

    if (linkTracer)
        linkTracer->Stop();
    if (pktTracer)
        pktTracer->Stop();

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();

    return 0;
}
