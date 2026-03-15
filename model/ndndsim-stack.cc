/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndStack implementation.
 */

#include "ndndsim-stack.h"

#include "ndndsim-go-bridge.h"

#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/packet.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("NdndStack");

namespace ndndsim
{

NS_OBJECT_ENSURE_REGISTERED(NdndStack);

TypeId
NdndStack::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::ndndsim::NdndStack")
            .SetParent<Object>()
            .SetGroupName("NdndSIM")
            .AddConstructor<NdndStack>();
    return tid;
}

NdndStack::NdndStack()
    : m_node(nullptr),
      m_installed(false)
{
}

NdndStack::~NdndStack()
{
}

void
NdndStack::DoDispose()
{
    if (m_installed && m_node)
    {
        NS_LOG_INFO("Destroying NDNd stack on node " << m_node->GetId());
        NdndSimDestroyNode(m_node->GetId());
        m_installed = false;
    }
    m_node = nullptr;
    m_faceIds.clear();
    Object::DoDispose();
}

void
NdndStack::NotifyNewAggregate()
{
    if (!m_node)
    {
        m_node = GetObject<Node>();
    }
    Object::NotifyNewAggregate();
}

void
NdndStack::Install()
{
    NS_ASSERT_MSG(m_node, "NdndStack must be aggregated to a Node before Install()");
    NS_ASSERT_MSG(!m_installed, "NdndStack already installed on this node");

    uint32_t nodeId = m_node->GetId();
    NS_LOG_INFO("Installing NDNd stack on node " << nodeId);

    // Create the Go-side node
    int rc = NdndSimCreateNode(nodeId);
    NS_ASSERT_MSG(rc == 0, "Failed to create NDNd simulation node " << nodeId);
    m_installed = true;

    // Create a face for each NetDevice
    for (uint32_t i = 0; i < m_node->GetNDevices(); ++i)
    {
        Ptr<NetDevice> dev = m_node->GetDevice(i);
        if (!dev)
        {
            continue;
        }

        uint64_t faceId = NdndSimAddFace(nodeId, i);
        m_faceIds[i] = faceId;

        NS_LOG_INFO("  Created face " << faceId << " for device " << i
                                        << " (" << dev->GetInstanceTypeId().GetName() << ")");

        // Register receive callback for NDN packets (EtherType 0x8624)
        m_node->RegisterProtocolHandler(
            MakeCallback(&NdndStack::ReceiveFromDevice, this),
            0x8624, // NDN EtherType
            dev,
            false);
    }
}

void
NdndStack::ReceiveFromDevice(Ptr<NetDevice> device,
                              Ptr<const Packet> packet,
                              uint16_t protocol,
                              const Address& sender,
                              const Address& receiver,
                              NetDevice::PacketType packetType)
{
    NS_LOG_FUNCTION(this << device << packet->GetSize());

    if (!m_installed || !m_node)
    {
        return;
    }

    uint32_t ifIndex = device->GetIfIndex();

    // Copy packet bytes
    uint32_t size = packet->GetSize();
    auto buffer = std::make_unique<uint8_t[]>(size);
    packet->CopyData(buffer.get(), size);

    // Deliver to NDNd
    NdndSimReceivePacket(m_node->GetId(), ifIndex, buffer.get(), size);
}

void
NdndStack::AddRoute(const std::string& prefix, uint64_t faceId, uint64_t cost)
{
    NS_ASSERT_MSG(m_installed, "NdndStack not installed");

    std::string p = prefix;
    NdndSimAddRoute(m_node->GetId(),
                     const_cast<char*>(p.c_str()),
                     static_cast<int>(p.size()),
                     faceId,
                     cost);
}

void
NdndStack::RemoveRoute(const std::string& prefix, uint64_t faceId)
{
    NS_ASSERT_MSG(m_installed, "NdndStack not installed");

    std::string p = prefix;
    NdndSimRemoveRoute(m_node->GetId(),
                        const_cast<char*>(p.c_str()),
                        static_cast<int>(p.size()),
                        faceId);
}

void
NdndStack::AnnouncePrefixToDv(const std::string& prefix)
{
    NS_ASSERT_MSG(m_installed, "NdndStack not installed");

    std::string p = prefix;
    NdndSimAnnouncePrefixToDv(m_node->GetId(),
                               const_cast<char*>(p.c_str()),
                               static_cast<int>(p.size()));
}

void
NdndStack::WithdrawPrefixFromDv(const std::string& prefix)
{
    NS_ASSERT_MSG(m_installed, "NdndStack not installed");

    std::string p = prefix;
    NdndSimWithdrawPrefixFromDv(m_node->GetId(),
                                 const_cast<char*>(p.c_str()),
                                 static_cast<int>(p.size()));
}

uint64_t
NdndStack::GetFaceId(uint32_t ifIndex) const
{
    auto it = m_faceIds.find(ifIndex);
    if (it != m_faceIds.end())
    {
        return it->second;
    }
    return 0;
}

} // namespace ndndsim
} // namespace ns3
