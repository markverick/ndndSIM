/*
 * ndndsim-atlas-multihop-scenario.cc — Multi-hop DV routing changes test
 *
 * Topology:  a -- b -- c -- d  (linear, 4-node, read from topo file)
 *
 * Tests 5 phases:
 *   Phase 0: initial_convergence  — announce prefix, verify RIB + traffic
 *   Phase 1: link_failure         — b-c 100% loss, verify prefix gone + no traffic
 *   Phase 2: link_recovery        — b-c restored, verify prefix back + traffic
 *   Phase 3: prefix_withdrawal    — withdraw prefix on d, verify gone + no traffic
 *   Phase 4: prefix_reannounce    — re-announce prefix on d, verify back + traffic
 *
 * Outputs a JSON results file with per-phase pass/fail for each assertion.
 *
 * Usage:
 *   ./ns3 run "ndndsim-atlas-multihop-scenario --topo=... --resultsFile=..."
 */

#include "ns3/core-module.h"
#include "ns3/error-model.h"
#include "ns3/names.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

// ndndSIM headers
#include "ns3/ndndsim-app-helper.h"
#include "ns3/ndndsim-rate-tracer.h"
#include "ns3/ndndsim-stack-helper.h"
#include "ns3/ndndsim-stack.h"
#include "ns3/ndndsim-topology-reader.h"
#include "ns3/ndndsim-go-bridge.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

using namespace ns3;
using namespace ns3::ndndsim;

// ─── Globals ───────────────────────────────────────────────────────

struct PhaseResult
{
    std::string phase;
    std::string assertion;
    bool passed;
};

static std::vector<PhaseResult> g_results;
static Ptr<RateErrorModel> g_errorModelBC_fwd;  // on b→c device
static Ptr<RateErrorModel> g_errorModelBC_rev;  // on c→b device
static Ptr<Node> g_consumer;
static Ptr<Node> g_producer;
static std::string g_prefix;
static std::string g_network;

// ─── Helpers ───────────────────────────────────────────────────────

static void
AddResult(const std::string& phase, const std::string& assertion, bool passed)
{
    g_results.push_back({phase, assertion, passed});
    std::cout << Simulator::Now().GetSeconds() << "s: ["
              << (passed ? "PASS" : "FAIL") << "] "
              << phase << ": " << assertion << std::endl;
}

static bool
CheckRib(Ptr<Node> node, const std::string& prefix, bool wantPresent)
{
    auto stack = node->GetObject<NdndStack>();
    int count = stack->GetRibEntryCount(prefix);
    bool present = (count > 0);
    return present == wantPresent;
}

// Consumer traffic check: start a Go-side consumer on node a, wait for
// Data callbacks.  A persistent NdndProducer (installed via NdndAppHelper
// at start-up) serves Data on the producer node.  NdndAppHelper does NOT
// auto-announce to DV, so prefix reachability is controlled entirely
// through AnnouncePrefixToDv / WithdrawPrefixFromDv.

static int g_dataReceived = 0;

static void
OnDataReceived(uint32_t dataSize, const std::string& dataName)
{
    g_dataReceived++;
}

static void
StartTrafficCheck()
{
    g_dataReceived = 0;
    RegisterDataReceivedCallback(g_consumer->GetId(), OnDataReceived);
    // Start a consumer that sends Interests for g_prefix/<seqno>.
    // Interest lifetime set high so they don't time out during the check window.
    NdndSimRegisterConsumer(g_consumer->GetId(),
                            const_cast<char*>(g_prefix.c_str()),
                            static_cast<int>(g_prefix.size()),
                            5.0,   // 5 Hz
                            4000); // 4s lifetime
}

static void
StopTrafficCheck()
{
    NdndSimStopConsumer(g_consumer->GetId());
}

static bool
DidTrafficFlow()
{
    return g_dataReceived > 0;
}

// ─── Phase callbacks ───────────────────────────────────────────────

// Phase 0: initial_convergence
//   action: announce prefix on producer
//   after wait: check RIB on consumer, check traffic
static void
Phase0_Announce()
{
    std::cout << Simulator::Now().GetSeconds() << "s: Phase 0 — announce prefix " << g_prefix << " on producer" << std::endl;
    auto stack = g_producer->GetObject<NdndStack>();
    stack->AnnouncePrefixToDv(g_prefix);
}

static void
Phase0_StartTraffic()
{
    StartTrafficCheck();
}

static void
Phase0_Assert()
{
    StopTrafficCheck();
    bool ribOk = CheckRib(g_consumer, g_prefix, true);
    bool trafficOk = DidTrafficFlow();
    AddResult("initial_convergence", "prefix_reachable", ribOk);
    AddResult("initial_convergence", "traffic_flows", trafficOk);
}

// Phase 1: link_failure
//   action: enable 100% loss on b-c link
//   after wait: check RIB gone on consumer, check traffic blocked
static void
Phase1_LinkDown()
{
    std::cout << Simulator::Now().GetSeconds() << "s: Phase 1 — link DOWN b--c" << std::endl;
    g_errorModelBC_fwd->SetRate(1.0);
    g_errorModelBC_rev->SetRate(1.0);
}

static void
Phase1_StartTraffic()
{
    StartTrafficCheck();
}

static void
Phase1_Assert()
{
    StopTrafficCheck();
    // After link failure + router_dead_interval, routes through b-c should be gone.
    // Check that the DV router prefix for the producer is unreachable.
    // DV router names use the format: network + "/node" + nodeId.
    std::string producerRouter = g_network + "/node" + std::to_string(g_producer->GetId());
    bool ribOk = CheckRib(g_consumer, producerRouter, false);
    bool trafficOk = !DidTrafficFlow();
    AddResult("link_failure", "prefix_unreachable", ribOk);
    AddResult("link_failure", "traffic_blocked", trafficOk);
}

// Phase 2: link_recovery
//   action: disable error model on b-c
//   after wait: check RIB restored, traffic flows
static void
Phase2_LinkUp()
{
    std::cout << Simulator::Now().GetSeconds() << "s: Phase 2 — link UP b--c" << std::endl;
    g_errorModelBC_fwd->Disable();
    g_errorModelBC_rev->Disable();
}

static void
Phase2_StartTraffic()
{
    StartTrafficCheck();
}

static void
Phase2_Assert()
{
    StopTrafficCheck();
    bool ribOk = CheckRib(g_consumer, g_prefix, true);
    bool trafficOk = DidTrafficFlow();
    AddResult("link_recovery", "prefix_reachable", ribOk);
    AddResult("link_recovery", "traffic_flows", trafficOk);
}

// Phase 3: prefix_withdrawal
//   action: withdraw prefix on producer
//   after wait: check RIB gone, traffic blocked
static void
Phase3_Withdraw()
{
    std::cout << Simulator::Now().GetSeconds() << "s: Phase 3 — withdraw prefix " << g_prefix << " on producer" << std::endl;
    auto stack = g_producer->GetObject<NdndStack>();
    stack->WithdrawPrefixFromDv(g_prefix);
}

static void
Phase3_StartTraffic()
{
    StartTrafficCheck();
}

static void
Phase3_Assert()
{
    StopTrafficCheck();
    bool ribOk = CheckRib(g_consumer, g_prefix, false);
    bool trafficOk = !DidTrafficFlow();
    AddResult("prefix_withdrawal", "prefix_unreachable", ribOk);
    AddResult("prefix_withdrawal", "traffic_blocked", trafficOk);
}

// Phase 4: prefix_reannounce
//   action: re-announce prefix on producer
//   after wait: check RIB restored, traffic flows
static void
Phase4_Reannounce()
{
    std::cout << Simulator::Now().GetSeconds() << "s: Phase 4 — re-announce prefix " << g_prefix << " on producer" << std::endl;
    auto stack = g_producer->GetObject<NdndStack>();
    stack->AnnouncePrefixToDv(g_prefix);
}

static void
Phase4_StartTraffic()
{
    StartTrafficCheck();
}

static void
Phase4_Assert()
{
    StopTrafficCheck();
    bool ribOk = CheckRib(g_consumer, g_prefix, true);
    bool trafficOk = DidTrafficFlow();
    AddResult("prefix_reannounce", "prefix_reachable", ribOk);
    AddResult("prefix_reannounce", "traffic_flows", trafficOk);
}

// ─── Main ──────────────────────────────────────────────────────────

int
main(int argc, char* argv[])
{
    std::string topoFile;
    std::string resultsFile = "multihop-results.json";
    std::string rateTrace = "multihop-rate-trace.csv";
    std::string dvConfig;
    std::string consumerName = "a";
    std::string producerName = "d";
    std::string linkDownSrc = "b";
    std::string linkDownDst = "c";
    std::string prefix = "/app/data";
    std::string network = "/minindn";
    double simTime = 90.0;
    // DV timing: advertise_interval=2s, router_dead_interval=10s
    // Phase durations must account for convergence time.
    double phase0_announce = 2.0;    // when to announce prefix
    double phase0_traffic  = 14.0;   // start traffic check (after DV convergence ~12s)
    double phase0_assert   = 18.0;   // assert after traffic window

    double phase1_action   = 20.0;   // link down
    double phase1_traffic  = 33.0;   // start traffic check (after router_dead_interval)
    double phase1_assert   = 37.0;   // assert

    double phase2_action   = 39.0;   // link up
    double phase2_traffic  = 49.0;   // start traffic check (after re-convergence)
    double phase2_assert   = 53.0;   // assert

    double phase3_action   = 55.0;   // withdraw prefix
    double phase3_traffic  = 65.0;   // start traffic check (after DV propagation)
    double phase3_assert   = 69.0;   // assert

    double phase4_action   = 71.0;   // re-announce prefix
    double phase4_traffic  = 81.0;   // start traffic check (after DV propagation)
    double phase4_assert   = 85.0;   // assert

    CommandLine cmd;
    cmd.AddValue("topo", "Topology file path (required)", topoFile);
    cmd.AddValue("resultsFile", "Output JSON results path", resultsFile);
    cmd.AddValue("rateTrace", "Output rate trace CSV path", rateTrace);
    cmd.AddValue("prefix", "App prefix to test", prefix);
    cmd.AddValue("network", "DV network prefix", network);
    cmd.AddValue("consumer", "Consumer node name", consumerName);
    cmd.AddValue("producer", "Producer node name", producerName);
    cmd.AddValue("linkDownSrc", "Source node of link to fail", linkDownSrc);
    cmd.AddValue("linkDownDst", "Destination node of link to fail", linkDownDst);
    cmd.AddValue("simTime", "Total simulation time", simTime);
    cmd.AddValue("dvConfig", "DV config JSON overlay", dvConfig);
    cmd.Parse(argc, argv);

    NS_ABORT_MSG_IF(topoFile.empty(), "--topo is required");

    g_prefix = prefix;
    g_network = network;

    // ─── Topology ──────────────────────────────────────────────────

    NdndTopologyReader reader;
    reader.SetFileName(topoFile);
    NodeContainer nodes = reader.Read();
    NS_ABORT_MSG_IF(nodes.GetN() == 0, "No nodes read from topology file");

    g_consumer = Names::Find<Node>(consumerName);
    g_producer = Names::Find<Node>(producerName);
    NS_ABORT_MSG_IF(!g_consumer, "Consumer node not found: " << consumerName);
    NS_ABORT_MSG_IF(!g_producer, "Producer node not found: " << producerName);

    Ptr<Node> linkSrc = Names::Find<Node>(linkDownSrc);
    Ptr<Node> linkDst = Names::Find<Node>(linkDownDst);
    NS_ABORT_MSG_IF(!linkSrc, "Link-down source node not found: " << linkDownSrc);
    NS_ABORT_MSG_IF(!linkDst, "Link-down dest node not found: " << linkDownDst);

    // ─── Find the b-c link devices ────────────────────────────────

    // Walk the topology links to find the b-c link and install error models.
    const auto& links = reader.GetLinks();
    bool foundBC = false;
    for (const auto& link : links)
    {
        bool match1 = (link.fromNode == linkSrc && link.toNode == linkDst);
        bool match2 = (link.fromNode == linkDst && link.toNode == linkSrc);
        if (match1 || match2)
        {
            // devices.Get(0) is on fromNode, devices.Get(1) is on toNode
            g_errorModelBC_fwd = CreateObject<RateErrorModel>();
            g_errorModelBC_fwd->SetRate(0.0);
            g_errorModelBC_rev = CreateObject<RateErrorModel>();
            g_errorModelBC_rev->SetRate(0.0);

            // Install error models on both directions
            link.devices.Get(0)->SetAttribute("ReceiveErrorModel",
                                               PointerValue(g_errorModelBC_rev));
            link.devices.Get(1)->SetAttribute("ReceiveErrorModel",
                                               PointerValue(g_errorModelBC_fwd));
            foundBC = true;
            break;
        }
    }
    NS_ABORT_MSG_IF(!foundBC, "Link " << linkDownSrc << "--" << linkDownDst << " not found in topology");

    // ─── NDNd Stack + DV Routing ───────────────────────────────────

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);
    NdndStackHelper::EnableDvRouting(network, nodes, dvConfig);

    // ─── Persistent Producer ───────────────────────────────────────
    // Install a NdndProducer on the producer node for the app prefix.
    // NdndAppHelper does NOT auto-announce to DV — the DV announcement
    // is controlled separately via AnnouncePrefixToDv/WithdrawPrefixFromDv.
    // The producer stays up for the entire simulation; only DV reachability
    // determines whether the consumer can fetch data.

    NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue(prefix));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(1024));
    producerHelper.SetAttribute("Freshness", TimeValue(Seconds(2.0)));
    auto pApps = producerHelper.Install(g_producer);
    pApps.Start(Seconds(0.5));

    // ─── Rate Tracer ───────────────────────────────────────────────

    NdndRateTracer::InstallAll(rateTrace, Seconds(0.5));

    // ─── Schedule Phases ───────────────────────────────────────────

    // Phase 0: initial_convergence
    Simulator::Schedule(Seconds(phase0_announce), &Phase0_Announce);
    Simulator::Schedule(Seconds(phase0_traffic), &Phase0_StartTraffic);
    Simulator::Schedule(Seconds(phase0_assert), &Phase0_Assert);

    // Phase 1: link_failure
    Simulator::Schedule(Seconds(phase1_action), &Phase1_LinkDown);
    Simulator::Schedule(Seconds(phase1_traffic), &Phase1_StartTraffic);
    Simulator::Schedule(Seconds(phase1_assert), &Phase1_Assert);

    // Phase 2: link_recovery
    Simulator::Schedule(Seconds(phase2_action), &Phase2_LinkUp);
    Simulator::Schedule(Seconds(phase2_traffic), &Phase2_StartTraffic);
    Simulator::Schedule(Seconds(phase2_assert), &Phase2_Assert);

    // Phase 3: prefix_withdrawal
    Simulator::Schedule(Seconds(phase3_action), &Phase3_Withdraw);
    Simulator::Schedule(Seconds(phase3_traffic), &Phase3_StartTraffic);
    Simulator::Schedule(Seconds(phase3_assert), &Phase3_Assert);

    // Phase 4: prefix_reannounce
    Simulator::Schedule(Seconds(phase4_action), &Phase4_Reannounce);
    Simulator::Schedule(Seconds(phase4_traffic), &Phase4_StartTraffic);
    Simulator::Schedule(Seconds(phase4_assert), &Phase4_Assert);

    // ─── Run ───────────────────────────────────────────────────────

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // ─── Write Results JSON ────────────────────────────────────────

    {
        std::ofstream ofs(resultsFile);
        ofs << "[\n";
        for (size_t i = 0; i < g_results.size(); ++i)
        {
            const auto& r = g_results[i];
            ofs << "  {\"phase\": \"" << r.phase
                << "\", \"assertion\": \"" << r.assertion
                << "\", \"passed\": " << (r.passed ? "true" : "false")
                << "}";
            if (i + 1 < g_results.size())
                ofs << ",";
            ofs << "\n";
        }
        ofs << "]\n";
    }

    // ─── Summary ───────────────────────────────────────────────────

    int passed = 0;
    for (const auto& r : g_results)
    {
        if (r.passed) passed++;
    }
    std::cout << "\n" << passed << "/" << g_results.size() << " assertions passed" << std::endl;

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();

    return (passed == static_cast<int>(g_results.size())) ? 0 : 1;
}
