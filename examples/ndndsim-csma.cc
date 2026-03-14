/*
 * ndndSIM CSMA Bus Example
 *
 * Equivalent to old ndnSIM: ndn-csma.cpp
 *
 * Topology:
 *
 *                     CSMA bus (1Mbps, 10ms)
 *   +--------------------------+--------------------------+
 *   |                          |                          |
 *   Consumer                 Router                   Producer
 *   (node 0)                (node 1)                 (node 2)
 *
 * Three nodes share a CSMA (Ethernet) bus.
 * The consumer sends 10 Interests/sec for /ndn/csma/<seqno>.
 * The producer replies with 1024-byte Data.
 * Demonstrates NDN over shared broadcast media.
 *
 * Usage:
 *   ./ns3 run ndndsim-csma
 */

#include "ns3/core-module.h"
#include "ns3/csma-module.h"
#include "ns3/network-module.h"

// ndndSIM headers
#include "ns3/ndndsim-app-helper.h"
#include "ns3/ndndsim-stack-helper.h"

using namespace ns3;

int
main(int argc, char* argv[])
{
    CommandLine cmd;
    cmd.Parse(argc, argv);

    LogComponentEnable("NdndConsumer", LOG_LEVEL_INFO);
    LogComponentEnable("NdndProducer", LOG_LEVEL_INFO);

    // ─── CSMA Topology ─────────────────────────────────────────────

    Config::SetDefault("ns3::CsmaChannel::DataRate", StringValue("1Mbps"));
    Config::SetDefault("ns3::CsmaChannel::Delay", StringValue("10ms"));
    Config::SetDefault("ns3::DropTailQueue<Packet>::MaxSize", StringValue("20p"));

    NodeContainer nodes;
    nodes.Create(3);

    CsmaHelper csma;
    NetDeviceContainer devices = csma.Install(nodes);

    // ─── NDNd Stack ────────────────────────────────────────────────

    ndndsim::NdndStackHelper::InitializeBridge();

    ndndsim::NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    // ─── Routing ───────────────────────────────────────────────────

    // On CSMA bus all nodes share one interface; flood routes to all faces.
    std::string prefix = "/ndn/csma";
    ndndsim::NdndStackHelper::AddRoutesToAll(prefix, nodes);

    // ─── Applications ──────────────────────────────────────────────

    ndndsim::NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue(prefix));
    consumerHelper.SetAttribute("Frequency", DoubleValue(10.0));
    auto consumerApps = consumerHelper.Install(nodes.Get(0));
    consumerApps.Start(Seconds(1.0));
    consumerApps.Stop(Seconds(10.0));

    ndndsim::NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue(prefix));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(1024));
    auto producerApps = producerHelper.Install(nodes.Get(2));
    producerApps.Start(Seconds(0.5));
    producerApps.Stop(Seconds(10.0));

    // ─── Simulation ────────────────────────────────────────────────

    Simulator::Stop(Seconds(11.0));
    Simulator::Run();

    ndndsim::NdndStackHelper::DestroyBridge();
    Simulator::Destroy();

    return 0;
}
