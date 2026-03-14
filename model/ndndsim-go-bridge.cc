/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * Bridge between ns-3 C++ and NDNd Go simulation library.
 * Implements the callback functions that Go calls into.
 */

#include "ndndsim-go-bridge.h"

#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/node-list.h"
#include "ns3/simulator.h"

#include <cstring>
#include <unordered_map>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("NdndSimGoBridge");

namespace ndndsim
{

/*
 * Map from Go event IDs to ns-3 EventIds for cancellation support.
 */
static std::unordered_map<uint64_t, EventId> g_eventMap;

/**
 * Callback: NDNd wants to send a packet on an ns-3 NetDevice.
 * This is called from Go code during packet processing.
 */
static void
OnSendPacket(uint32_t nodeId, uint32_t ifIndex, const void* data, uint32_t dataLen)
{
    NS_LOG_FUNCTION(nodeId << ifIndex << dataLen);

    Ptr<Node> node = NodeList::GetNode(nodeId);
    if (!node)
    {
        NS_LOG_WARN("OnSendPacket: node " << nodeId << " not found");
        return;
    }

    if (ifIndex >= node->GetNDevices())
    {
        NS_LOG_WARN("OnSendPacket: ifIndex " << ifIndex << " out of range for node " << nodeId);
        return;
    }

    Ptr<NetDevice> dev = node->GetDevice(ifIndex);
    if (!dev)
    {
        NS_LOG_WARN("OnSendPacket: device " << ifIndex << " is null on node " << nodeId);
        return;
    }

    // Create an ns-3 Packet from the raw bytes
    Ptr<Packet> pkt = Create<Packet>(static_cast<const uint8_t*>(data), dataLen);

    // Send as broadcast (NDN typically uses broadcast on shared media).
    // For point-to-point links, the destination address doesn't matter.
    dev->Send(pkt, dev->GetBroadcast(), 0x8624); // NDN EtherType
}

/**
 * Callback: NDNd wants to fire a previously scheduled event.
 * This trampoline is scheduled by OnScheduleEvent and calls back into Go.
 */
static void
FireEventTrampoline(uint32_t nodeId, uint64_t eventId)
{
    NS_LOG_FUNCTION(nodeId << eventId);
    g_eventMap.erase(eventId);
    NdndSimFireEvent(nodeId, eventId);
}

/**
 * Callback: NDNd wants to schedule a future event via ns-3.
 */
static void
OnScheduleEvent(uint32_t nodeId, int64_t delayNs, uint64_t eventId)
{
    NS_LOG_FUNCTION(nodeId << delayNs << eventId);

    Time delay = NanoSeconds(delayNs);
    EventId eid = Simulator::Schedule(delay, &FireEventTrampoline, nodeId, eventId);
    g_eventMap[eventId] = eid;
}

/**
 * Callback: NDNd wants to cancel a previously scheduled event.
 */
static void
OnCancelEvent(uint64_t eventId)
{
    NS_LOG_FUNCTION(eventId);
    auto it = g_eventMap.find(eventId);
    if (it != g_eventMap.end())
    {
        Simulator::Cancel(it->second);
        g_eventMap.erase(it);
    }
}

/**
 * Callback: NDNd wants to know the current simulation time.
 */
static int64_t
OnGetTimeNs()
{
    return Simulator::Now().GetNanoSeconds();
}

/*
 * Initialize the Go bridge with ns-3 callbacks.
 */
void
InitGoBridge()
{
    NS_LOG_FUNCTION_NOARGS();
    NdndSimInit(&OnSendPacket, &OnScheduleEvent, &OnCancelEvent, &OnGetTimeNs);
}

/*
 * Cleanup the Go bridge.
 */
void
DestroyGoBridge()
{
    NS_LOG_FUNCTION_NOARGS();
    NdndSimDestroy();
    g_eventMap.clear();
}

} // namespace ndndsim
} // namespace ns3
