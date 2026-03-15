/*
 * ndndSIM Sprint Topology — Random Link Flaps & Prefix Churn
 *
 * Reads the Sprint PoP-level backbone topology (11 nodes, 16 links),
 * enables DV routing, and then randomly toggles links up/down and
 * announces/withdraws DV prefixes throughout the simulation.
 *
 * Traffic is measured at the link layer (aggregate bytes on all
 * point-to-point links) rather than per-application hooks.
 * Prefix events are pure DV route announcements/withdrawals —
 * no extra producer/consumer apps are installed.
 *
 * Topology: contrib/ndndSIM/examples/topologies/topo-sprint.txt
 *
 * Outputs (in results/ndndsim-sprint-churn/):
 *   rate-trace.csv       — per-node traffic rates (all combined)
 *   link-traffic.csv     — aggregate link-level packets & bytes
 *   events.csv           — timestamped event log
 *   traffic.png          — auto-generated plot (requires matplotlib)
 *
 * Usage:
 *   ./ns3 run ndndsim-sprint-churn
 *   ./ns3 run "ndndsim-sprint-churn --simTime=300 --linkEvents=10"
 */

#include "ns3/core-module.h"
#include "ns3/error-model.h"
#include "ns3/names.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

// ndndSIM headers
#include "ns3/ndndsim-app-helper.h"
#include "ns3/ndndsim-link-tracer.h"
#include "ns3/ndndsim-rate-tracer.h"
#include "ns3/ndndsim-stack-helper.h"
#include "ns3/ndndsim-stack.h"
#include "ns3/ndndsim-topology-reader.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sys/stat.h>
#include <vector>

using namespace ns3;
using namespace ns3::ndndsim;

// Global event log stream (opened in main)
static std::ofstream* g_eventLog = nullptr;

static void
LogEvent(double time, const std::string& type, const std::string& detail)
{
    if (g_eventLog)
    {
        *g_eventLog << time << "," << type << "," << detail << "\n";
    }
}

// ─── Link-flap helpers ─────────────────────────────────────────────

static void
LinkDown(Ptr<RateErrorModel> emA,
         Ptr<RateErrorModel> emB,
         const std::string& from,
         const std::string& to)
{
    double t = Simulator::Now().GetSeconds();
    std::cout << t << "s  LINK DOWN  " << from << " <-> " << to << std::endl;
    LogEvent(t, "LinkDown", from + " <-> " + to);
    emA->SetRate(1.0);
    emB->SetRate(1.0);
}

static void
LinkUp(Ptr<RateErrorModel> emA,
       Ptr<RateErrorModel> emB,
       const std::string& from,
       const std::string& to)
{
    double t = Simulator::Now().GetSeconds();
    std::cout << t << "s  LINK UP    " << from << " <-> " << to << std::endl;
    LogEvent(t, "LinkUp", from + " <-> " + to);
    emA->Disable();
    emB->Disable();
}

// ─── Prefix-event helpers (DV announce / withdraw) ─────────────────

static void
AnnouncePrefix(Ptr<NdndStack> stack, const std::string& prefix, const std::string& nodeName)
{
    double t = Simulator::Now().GetSeconds();
    std::cout << t << "s  PREFIX ADD " << prefix << " @ " << nodeName << std::endl;
    LogEvent(t, "PrefixAdd", prefix + " @ " + nodeName);
    stack->AnnouncePrefixToDv(prefix);
}

static void
WithdrawPrefix(Ptr<NdndStack> stack, const std::string& prefix, const std::string& nodeName)
{
    double t = Simulator::Now().GetSeconds();
    std::cout << t << "s  PREFIX DEL " << prefix << " @ " << nodeName << std::endl;
    LogEvent(t, "PrefixDel", prefix + " @ " + nodeName);
    stack->WithdrawPrefixFromDv(prefix);
}

int
main(int argc, char* argv[])
{
    double simTime = 300.0;
    uint32_t linkEvents = 10;
    uint32_t prefixEvents = 8;
    double minEventGap = 10.0;
    uint32_t seed = 42;

    CommandLine cmd;
    cmd.AddValue("simTime", "Simulation duration (seconds)", simTime);
    cmd.AddValue("linkEvents", "Number of random link-flap events", linkEvents);
    cmd.AddValue("prefixEvents", "Number of random prefix add/remove events", prefixEvents);
    cmd.AddValue("minEventGap", "Minimum seconds between events", minEventGap);
    cmd.AddValue("seed", "RNG seed", seed);
    cmd.Parse(argc, argv);

    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(1);

    // ─── Read Sprint topology ──────────────────────────────────────

    NdndTopologyReader reader;
    reader.SetFileName("contrib/ndndSIM/examples/topologies/topo-sprint.txt");
    NodeContainer nodes = reader.Read();

    std::cout << "Sprint topology: " << nodes.GetN() << " nodes, " << reader.GetLinks().size()
              << " links" << std::endl;

    // ─── NDNd Stack + DV routing ───────────────────────────────────

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    NdndStackHelper::EnableDvRouting("/ndn", nodes);

    // ─── Attach error models to every link (for later flaps) ───────

    struct LinkState
    {
        Ptr<RateErrorModel> errorModelA; // from → to direction
        Ptr<RateErrorModel> errorModelB; // to → from direction
        std::string fromName;
        std::string toName;
    };

    const auto& topoLinks = reader.GetLinks();
    std::vector<LinkState> links;
    links.reserve(topoLinks.size());

    for (const auto& l : topoLinks)
    {
        Ptr<RateErrorModel> emA = CreateObject<RateErrorModel>();
        Ptr<RateErrorModel> emB = CreateObject<RateErrorModel>();
        emA->SetRate(0.0);
        emB->SetRate(0.0);
        // Bidirectional: attach error models to both sides
        l.devices.Get(0)->SetAttribute("ReceiveErrorModel", PointerValue(emA));
        l.devices.Get(1)->SetAttribute("ReceiveErrorModel", PointerValue(emB));
        links.push_back({emA, emB, l.fromName, l.toName});
    }

    // ─── Schedule random link flaps ────────────────────────────────

    // Deterministic PRNG so results are reproducible with the same seed
    std::mt19937 rng(seed);

    // Link events occur in the [10, simTime - 10] window to let DV
    // converge at start and drain at end.
    double eventWindowStart = 10.0;
    double eventWindowEnd = simTime - 10.0;
    std::uniform_int_distribution<uint32_t> linkDist(0, links.size() - 1);

    // Generate link-down events, each followed by a link-up 3-5 s later
    double flapMin = 3.0;
    double flapMax = 5.0;
    std::uniform_real_distribution<double> flapDur(flapMin, flapMax);
    // Draw start time so that down + max duration fits in the window
    std::uniform_real_distribution<double> linkTimeDist(eventWindowStart,
                                                        eventWindowEnd - flapMax);

    // Generate candidate times, sort, then enforce minimum gap
    std::vector<double> linkTimes;
    for (uint32_t i = 0; i < linkEvents; ++i)
        linkTimes.push_back(linkTimeDist(rng));
    std::sort(linkTimes.begin(), linkTimes.end());
    for (size_t i = 1; i < linkTimes.size(); ++i)
    {
        if (linkTimes[i] - linkTimes[i - 1] < minEventGap)
            linkTimes[i] = linkTimes[i - 1] + minEventGap;
    }

    for (uint32_t i = 0; i < linkEvents; ++i)
    {
        uint32_t idx = linkDist(rng);
        double tDown = linkTimes[i];
        double tUp = tDown + flapDur(rng);

        Simulator::Schedule(Seconds(tDown),
                            &LinkDown,
                            links[idx].errorModelA,
                            links[idx].errorModelB,
                            links[idx].fromName,
                            links[idx].toName);
        Simulator::Schedule(Seconds(tUp),
                            &LinkUp,
                            links[idx].errorModelA,
                            links[idx].errorModelB,
                            links[idx].fromName,
                            links[idx].toName);
    }

    // ─── Output directory ──────────────────────────────────────────

    const std::string outDir = "results/ndndsim-sprint-churn";
    ::mkdir("results", 0755);
    ::mkdir(outDir.c_str(), 0755);

    const std::string rateFile = outDir + "/rate-trace.csv";
    const std::string eventsFile = outDir + "/events.csv";
    const std::string linkFile = outDir + "/link-traffic.csv";

    // ─── Link-level traffic tracer ─────────────────────────────────

    auto linkTracer = NdndLinkTracer::Create(linkFile, Seconds(1.0));
    for (const auto& l : topoLinks)
    {
        linkTracer->ConnectLink(l.devices);
    }

    // ─── Background traffic: consumers at edges, producers at core ──

    std::string basePrefix = "/ndn/sprint";

    const char* producerNames[] = {"LosAngeles", "Chicago", "NewYork"};
    for (const auto& name : producerNames)
    {
        Ptr<Node> pNode = Names::Find<Node>(name);
        NS_ABORT_MSG_IF(!pNode, "Producer node not found: " << name);

        std::string prefix = basePrefix + "/" + name;

        NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
        producerHelper.SetAttribute("Prefix", StringValue(prefix));
        producerHelper.SetAttribute("PayloadSize", UintegerValue(1024));
        producerHelper.SetAttribute("Freshness", TimeValue(Seconds(2.0)));
        auto apps = producerHelper.Install(pNode);
        apps.Start(Seconds(0.5));
        apps.Stop(Seconds(simTime - 1.0));
    }

    const char* consumerNames[] = {"Seattle", "Dallas", "Atlanta", "Washington"};
    for (const auto& cName : consumerNames)
    {
        Ptr<Node> cNode = Names::Find<Node>(cName);
        NS_ABORT_MSG_IF(!cNode, "Consumer node not found: " << cName);

        for (const auto& pName : producerNames)
        {
            std::string prefix = basePrefix + "/" + pName;

            NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
            consumerHelper.SetAttribute("Prefix", StringValue(prefix));
            consumerHelper.SetAttribute("Frequency", DoubleValue(20.0));
            auto apps = consumerHelper.Install(cNode);
            apps.Start(Seconds(5.0));
            apps.Stop(Seconds(simTime - 2.0));
        }
    }

    // ─── Prefix events (DV announce / withdraw, no apps) ───────────

    std::vector<std::string> nodeNames = {"Seattle",     "SanFrancisco", "LosAngeles",
                                          "Denver",      "KansasCity",   "Dallas",
                                          "Chicago",     "Indianapolis", "Atlanta",
                                          "Washington",  "NewYork"};
    std::uniform_int_distribution<uint32_t> nodeDist(0, nodeNames.size() - 1);
    double prefixMin = 4.0;
    double prefixMax = 8.0;
    std::uniform_real_distribution<double> prefixDur(prefixMin, prefixMax);
    std::uniform_real_distribution<double> prefixTimeDist(eventWindowStart,
                                                          eventWindowEnd - prefixMax);

    // Generate candidate times, sort, then enforce minimum gap
    std::vector<double> prefixTimes;
    for (uint32_t i = 0; i < prefixEvents; ++i)
        prefixTimes.push_back(prefixTimeDist(rng));
    std::sort(prefixTimes.begin(), prefixTimes.end());
    for (size_t i = 1; i < prefixTimes.size(); ++i)
    {
        if (prefixTimes[i] - prefixTimes[i - 1] < minEventGap)
            prefixTimes[i] = prefixTimes[i - 1] + minEventGap;
    }

    for (uint32_t i = 0; i < prefixEvents; ++i)
    {
        uint32_t nodeIdx = nodeDist(rng);
        std::string nodeName = nodeNames[nodeIdx];
        Ptr<Node> node = Names::Find<Node>(nodeName);
        NS_ABORT_MSG_IF(!node, "Node not found: " << nodeName);

        Ptr<NdndStack> stack = node->GetObject<NdndStack>();
        NS_ABORT_MSG_IF(!stack, "NdndStack not found on node: " << nodeName);

        std::string prefix = "/ndn/ephemeral/svc-" + std::to_string(i);
        double tAdd = prefixTimes[i];
        double tRemove = tAdd + prefixDur(rng);

        Simulator::Schedule(Seconds(tAdd), &AnnouncePrefix, stack, prefix, nodeName);
        Simulator::Schedule(Seconds(tRemove), &WithdrawPrefix, stack, prefix, nodeName);
    }

    // ─── Event Log CSV ─────────────────────────────────────────────

    std::ofstream eventLog(eventsFile);
    eventLog << "Time,Type,Detail\n";
    g_eventLog = &eventLog;

    // ─── Rate Tracer ───────────────────────────────────────────────

    NdndRateTracer::InstallAll(rateFile, Seconds(1.0));

    // ─── Run ───────────────────────────────────────────────────────

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    g_eventLog = nullptr;
    eventLog.close();
    linkTracer->Stop();

    std::cout << "\nRate trace written to:       " << rateFile << std::endl;
    std::cout << "Event log written to:        " << eventsFile << std::endl;
    std::cout << "Link traffic written to:     " << linkFile << std::endl;

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();

    // ─── Auto-generate plot ───────────────────────────────────────

    std::string plotCmd =
        "python3 contrib/ndndSIM/examples/plot-ndndsim-traffic.py"
        " " + linkFile +
        " -e " + eventsFile +
        " -o " + outDir + "/traffic.png";

    std::cout << "\nRunning plot script...\n" << plotCmd << std::endl;
    int rc = std::system(plotCmd.c_str());
    if (rc != 0)
    {
        std::cerr << "WARNING: Plot script failed (exit code " << rc << ").\n"
                  << "         You can run it manually:\n"
                  << "         " << plotCmd << std::endl;
    }

    // Control-plane-only plot (exclude user traffic and other)
    std::string ctrlPlotCmd =
        "python3 contrib/ndndSIM/examples/plot-ndndsim-traffic.py"
        " " + linkFile +
        " -e " + eventsFile +
        " -o " + outDir + "/traffic-control.png"
        " --title \"Control Plane Traffic\""
        " --exclude UserInterest UserData Other";

    std::cout << ctrlPlotCmd << std::endl;
    rc = std::system(ctrlPlotCmd.c_str());
    if (rc != 0)
    {
        std::cerr << "WARNING: Control-plane plot failed (exit code " << rc << ").\n";
    }

    return 0;
}
