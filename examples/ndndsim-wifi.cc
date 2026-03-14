/*
 * ndndSIM WiFi Ad-Hoc Example
 *
 * Equivalent to old ndnSIM: ndn-simple-wifi.cpp
 *
 * Topology:
 *
 *   Consumer (node 0) ~~~ WiFi Ad-Hoc ~~~ Producer (node 1)
 *
 * Two nodes communicate over an 802.11a WiFi Ad-Hoc channel.
 * The consumer sends 10 Interests/sec for /ndn/wifi/<seqno>.
 * The producer replies with 1200-byte Data.
 *
 * Usage:
 *   ./ns3 run ndndsim-wifi
 */

#include "ns3/core-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/wifi-module.h"

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

    // Disable fragmentation for simplicity
    Config::SetDefault("ns3::WifiRemoteStationManager::FragmentationThreshold",
                       StringValue("2200"));
    Config::SetDefault("ns3::WifiRemoteStationManager::RtsCtsThreshold",
                       StringValue("2200"));
    Config::SetDefault("ns3::WifiRemoteStationManager::NonUnicastMode",
                       StringValue("OfdmRate24Mbps"));

    // ─── Nodes ─────────────────────────────────────────────────────

    NodeContainer nodes;
    nodes.Create(2);

    // ─── WiFi Setup ────────────────────────────────────────────────

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211a);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode", StringValue("OfdmRate24Mbps"));

    YansWifiChannelHelper wifiChannel;
    wifiChannel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    wifiChannel.AddPropagationLoss("ns3::ThreeLogDistancePropagationLossModel");
    wifiChannel.AddPropagationLoss("ns3::NakagamiPropagationLossModel");

    YansWifiPhyHelper wifiPhy;
    wifiPhy.SetChannel(wifiChannel.Create());
    wifiPhy.Set("TxPowerStart", DoubleValue(5.0));
    wifiPhy.Set("TxPowerEnd", DoubleValue(5.0));

    WifiMacHelper wifiMac;
    wifiMac.SetType("ns3::AdhocWifiMac");

    NetDeviceContainer wifiDevices = wifi.Install(wifiPhy, wifiMac, nodes);

    // ─── Mobility (fixed positions, close enough for WiFi) ─────────

    MobilityHelper mobility;
    Ptr<ListPositionAllocator> posAlloc = CreateObject<ListPositionAllocator>();
    posAlloc->Add(Vector(0.0, 0.0, 0.0));   // Consumer
    posAlloc->Add(Vector(10.0, 0.0, 0.0));   // Producer (10m away)
    mobility.SetPositionAllocator(posAlloc);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(nodes);

    // ─── NDNd Stack ────────────────────────────────────────────────

    ndndsim::NdndStackHelper::InitializeBridge();

    ndndsim::NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    // ─── Routing ───────────────────────────────────────────────────

    std::string prefix = "/ndn/wifi";
    ndndsim::NdndStackHelper::AddRoutesToAll(prefix, nodes);

    // ─── Applications ──────────────────────────────────────────────

    ndndsim::NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue(prefix));
    consumerHelper.SetAttribute("Frequency", DoubleValue(10.0));
    auto consumerApps = consumerHelper.Install(nodes.Get(0));
    consumerApps.Start(Seconds(2.0));
    consumerApps.Stop(Seconds(15.0));

    ndndsim::NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue(prefix));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(1200));
    auto producerApps = producerHelper.Install(nodes.Get(1));
    producerApps.Start(Seconds(1.0));
    producerApps.Stop(Seconds(15.0));

    // ─── Simulation ────────────────────────────────────────────────

    Simulator::Stop(Seconds(16.0));
    Simulator::Run();

    ndndsim::NdndStackHelper::DestroyBridge();
    Simulator::Destroy();

    return 0;
}
