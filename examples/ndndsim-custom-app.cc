/*
 * ndndSIM Custom Application Example
 *
 * Equivalent to old ndnSIM: ndn-custom-apps.cpp
 *
 * Demonstrates how to subclass NdndApp to create a custom application.
 * This example creates a "Ping" app that sends a single Interest and
 * logs when it starts/stops, showing the NdndApp extension pattern.
 *
 * Topology:
 *
 *   CustomPing ---- Router ---- Producer
 *   (node 0)      (node 1)    (node 2)
 *
 * Usage:
 *   ./ns3 run ndndsim-custom-app
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include "ns3/ndndsim-app.h"
#include "ns3/ndndsim-app-helper.h"
#include "ns3/ndndsim-go-bridge.h"
#include "ns3/ndndsim-stack-helper.h"
#include "ns3/ndndsim-stack.h"

#include <cstdint>
#include <sstream>
#include <vector>

using namespace ns3;

// ─── Custom App: NdndPing ──────────────────────────────────────────
//
// A minimal custom NDN application that sends one Interest at startup
// and prints timing information.

namespace ns3
{
namespace ndndsim
{

class NdndPing : public NdndApp
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::ndndsim::NdndPing")
                                .SetParent<NdndApp>()
                                .SetGroupName("NdndSIM")
                                .AddConstructor<NdndPing>()
                                .AddAttribute("Prefix",
                                               "NDN prefix to ping",
                                               StringValue("/ndn/ping"),
                                               MakeStringAccessor(&NdndPing::m_prefix),
                                               MakeStringChecker())
                                .AddAttribute("Count",
                                               "Number of Interests to send",
                                               UintegerValue(5),
                                               MakeUintegerAccessor(&NdndPing::m_count),
                                               MakeUintegerChecker<uint32_t>(1));
        return tid;
    }

    NdndPing()
        : m_sent(0)
    {
    }

  protected:
    void OnStart() override
    {
        std::cout << Simulator::Now().GetSeconds() << "s [Node "
                  << GetNode()->GetId() << "] NdndPing started for "
                  << m_prefix << " (count=" << m_count << ")" << std::endl;
        SendPing();
    }

    void OnStop() override
    {
        Simulator::Cancel(m_sendEvent);
        std::cout << Simulator::Now().GetSeconds() << "s [Node "
                  << GetNode()->GetId() << "] NdndPing stopped — sent "
                  << m_sent << " Interests" << std::endl;
    }

  private:
    void SendPing()
    {
        Ptr<NdndStack> stack = GetStack();
        if (!stack)
        {
            return;
        }

        // Build a minimal Interest TLV for <prefix>/<seq>
        auto wire = EncodeMinimalInterest(m_prefix, m_sent);
        NdndSimReceivePacket(GetNode()->GetId(), UINT32_MAX, wire.data(), wire.size());

        std::cout << Simulator::Now().GetSeconds() << "s [Node "
                  << GetNode()->GetId() << "] PING " << m_prefix << "/"
                  << m_sent << std::endl;

        m_sent++;
        if (m_sent < m_count)
        {
            m_sendEvent = Simulator::Schedule(Seconds(1.0), &NdndPing::SendPing, this);
        }
    }

    // Minimal TLV Interest encoder
    static std::vector<uint8_t> EncodeTlvVarNum(uint64_t val)
    {
        std::vector<uint8_t> out;
        if (val < 253)
        {
            out.push_back(static_cast<uint8_t>(val));
        }
        else if (val <= 0xFFFF)
        {
            out.push_back(253);
            out.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>(val & 0xFF));
        }
        return out;
    }

    static std::vector<uint8_t> EncodeMinimalInterest(const std::string& prefix, uint32_t seq)
    {
        // Parse name components
        std::vector<std::string> comps;
        std::istringstream ss(prefix);
        std::string tok;
        while (std::getline(ss, tok, '/'))
        {
            if (!tok.empty())
            {
                comps.push_back(tok);
            }
        }
        comps.push_back(std::to_string(seq));

        // Encode Name value
        std::vector<uint8_t> nameVal;
        for (const auto& c : comps)
        {
            auto t = EncodeTlvVarNum(8);
            auto l = EncodeTlvVarNum(c.size());
            nameVal.insert(nameVal.end(), t.begin(), t.end());
            nameVal.insert(nameVal.end(), l.begin(), l.end());
            nameVal.insert(nameVal.end(), c.begin(), c.end());
        }

        // Name TLV
        std::vector<uint8_t> name;
        auto nt = EncodeTlvVarNum(7);
        auto nl = EncodeTlvVarNum(nameVal.size());
        name.insert(name.end(), nt.begin(), nt.end());
        name.insert(name.end(), nl.begin(), nl.end());
        name.insert(name.end(), nameVal.begin(), nameVal.end());

        // Nonce
        name.push_back(10); // type
        name.push_back(4);  // len
        name.push_back(0xDE);
        name.push_back(0xAD);
        name.push_back(static_cast<uint8_t>((seq >> 8) & 0xFF));
        name.push_back(static_cast<uint8_t>(seq & 0xFF));

        // Wrap as Interest
        std::vector<uint8_t> pkt;
        auto it = EncodeTlvVarNum(5);
        auto il = EncodeTlvVarNum(name.size());
        pkt.insert(pkt.end(), it.begin(), it.end());
        pkt.insert(pkt.end(), il.begin(), il.end());
        pkt.insert(pkt.end(), name.begin(), name.end());
        return pkt;
    }

    std::string m_prefix;
    uint32_t m_count;
    uint32_t m_sent;
    EventId m_sendEvent;
};

NS_OBJECT_ENSURE_REGISTERED(NdndPing);

} // namespace ndndsim
} // namespace ns3

// ─── Main ──────────────────────────────────────────────────────────

int
main(int argc, char* argv[])
{
    CommandLine cmd;
    cmd.Parse(argc, argv);

    // ─── Topology ──────────────────────────────────────────────────

    NodeContainer nodes;
    nodes.Create(3);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("10ms"));
    p2p.Install(nodes.Get(0), nodes.Get(1));
    p2p.Install(nodes.Get(1), nodes.Get(2));

    // ─── NDNd Stack ────────────────────────────────────────────────

    ndndsim::NdndStackHelper::InitializeBridge();

    ndndsim::NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    // ─── Routing ───────────────────────────────────────────────────
    ndndsim::NdndStackHelper::AddRoute(nodes.Get(0), "/ndn/ping", uint32_t(0), uint64_t(1));
    ndndsim::NdndStackHelper::AddRoute(nodes.Get(1), "/ndn/ping", uint32_t(1), uint64_t(1));

    // ─── Custom App (NdndPing) on node 0 ───────────────────────────
    ndndsim::NdndAppHelper pingHelper("ns3::ndndsim::NdndPing");
    pingHelper.SetAttribute("Prefix", StringValue("/ndn/ping"));
    pingHelper.SetAttribute("Count", UintegerValue(5));
    auto pingApps = pingHelper.Install(nodes.Get(0));
    pingApps.Start(Seconds(1.0));
    pingApps.Stop(Seconds(10.0));

    // ─── Producer on node 2 ────────────────────────────────────────
    ndndsim::NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue("/ndn/ping"));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(64));
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
