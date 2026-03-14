/*
 * ndndSIM Link Failure Example
 *
 * Equivalent to old ndnSIM: ndn-simple-with-link-failure.cpp
 *
 * Topology:
 *
 *   Consumer ---- Router ---- Producer
 *   (node 0)    (node 1)    (node 2)
 *
 * All links are point-to-point with 1Mbps and 10ms delay.
 * The consumer sends 10 Interests/sec for /ndn/test/<seqno>.
 * At t=5s the link between Consumer and Router fails (100% loss).
 * At t=8s the link recovers.
 *
 * Usage:
 *   ./ns3 run ndndsim-link-failure
 */

#include "ns3/core-module.h"
#include "ns3/error-model.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

// ndndSIM headers
#include "ns3/ndndsim-app-helper.h"
#include "ns3/ndndsim-stack-helper.h"

using namespace ns3;

// Callback: schedule link failure by enabling 100% error rate
static void
LinkDown(Ptr<RateErrorModel> errorModel)
{
    std::cout << Simulator::Now().GetSeconds() << "s: *** Link DOWN ***" << std::endl;
    errorModel->SetRate(1.0); // 100% packet loss
}

// Callback: schedule link recovery by disabling error model
static void
LinkUp(Ptr<RateErrorModel> errorModel)
{
    std::cout << Simulator::Now().GetSeconds() << "s: *** Link UP   ***" << std::endl;
    errorModel->Disable();
}

int
main(int argc, char* argv[])
{
    CommandLine cmd;
    cmd.Parse(argc, argv);

    LogComponentEnable("NdndConsumer", LOG_LEVEL_INFO);
    LogComponentEnable("NdndProducer", LOG_LEVEL_INFO);

    // ─── Topology ──────────────────────────────────────────────────

    Config::SetDefault("ns3::PointToPointNetDevice::DataRate", StringValue("1Mbps"));
    Config::SetDefault("ns3::PointToPointChannel::Delay", StringValue("10ms"));
    Config::SetDefault("ns3::DropTailQueue<Packet>::MaxSize", StringValue("20p"));

    NodeContainer nodes;
    nodes.Create(3);

    PointToPointHelper p2p;
    NetDeviceContainer devConsumerRouter = p2p.Install(nodes.Get(0), nodes.Get(1));
    NetDeviceContainer devRouterProducer = p2p.Install(nodes.Get(1), nodes.Get(2));

    // ─── Error model for link failure ──────────────────────────────

    Ptr<RateErrorModel> errorModel = CreateObject<RateErrorModel>();
    errorModel->SetRate(0.0); // no loss initially
    devConsumerRouter.Get(1)->SetAttribute("ReceiveErrorModel", PointerValue(errorModel));

    Simulator::Schedule(Seconds(5.0), &LinkDown, errorModel);
    Simulator::Schedule(Seconds(8.0), &LinkUp, errorModel);

    // ─── NDNd Stack ────────────────────────────────────────────────

    ndndsim::NdndStackHelper::InitializeBridge();

    ndndsim::NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    // ─── Routing ───────────────────────────────────────────────────

    // Consumer → Router → Producer
    ndndsim::NdndStackHelper::AddRoute(nodes.Get(0), "/ndn/test", uint32_t(0), uint64_t(1));
    ndndsim::NdndStackHelper::AddRoute(nodes.Get(1), "/ndn/test", uint32_t(1), uint64_t(1));

    // ─── Applications ──────────────────────────────────────────────

    ndndsim::NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue("/ndn/test"));
    consumerHelper.SetAttribute("Frequency", DoubleValue(10.0));
    auto consumerApps = consumerHelper.Install(nodes.Get(0));
    consumerApps.Start(Seconds(1.0));
    consumerApps.Stop(Seconds(12.0));

    ndndsim::NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue("/ndn/test"));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(1024));
    auto producerApps = producerHelper.Install(nodes.Get(2));
    producerApps.Start(Seconds(0.5));
    producerApps.Stop(Seconds(12.0));

    // ─── Simulation ────────────────────────────────────────────────

    Simulator::Stop(Seconds(13.0));
    Simulator::Run();

    ndndsim::NdndStackHelper::DestroyBridge();
    Simulator::Destroy();

    return 0;
}
