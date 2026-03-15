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
#include <functional>
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

/*
 * Per-node callbacks for Data produced (producer) and Data received (consumer).
 * Apps register via RegisterDataProducedCallback / RegisterDataReceivedCallback.
 */
static std::unordered_map<uint32_t, std::function<void(uint32_t)>> g_dataProducedCallbacks;
static std::unordered_map<uint32_t, std::function<void(uint32_t, const std::string&)>> g_dataReceivedCallbacks;

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

/**
 * Callback: NDNd produced a Data packet on a node.
 */
static void
OnDataProduced(uint32_t nodeId, uint32_t dataSize)
{
    NS_LOG_FUNCTION(nodeId << dataSize);
    auto it = g_dataProducedCallbacks.find(nodeId);
    if (it != g_dataProducedCallbacks.end())
    {
        it->second(dataSize);
    }
}

/**
 * Callback: NDNd delivered Data to a consumer application face.
 */
static void
OnDataReceived(uint32_t nodeId, uint32_t dataSize, const char* dataName, uint32_t nameLen)
{
    NS_LOG_FUNCTION(nodeId << dataSize);
    auto it = g_dataReceivedCallbacks.find(nodeId);
    if (it != g_dataReceivedCallbacks.end())
    {
        std::string name(dataName, nameLen);
        it->second(dataSize, name);
    }
}

/*
 * Initialize the Go bridge with ns-3 callbacks.
 */
void
InitGoBridge()
{
    NS_LOG_FUNCTION_NOARGS();
    NdndSimInit(&OnSendPacket, &OnScheduleEvent, &OnCancelEvent, &OnGetTimeNs,
                &OnDataProduced, &OnDataReceived);
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
    g_dataProducedCallbacks.clear();
    g_dataReceivedCallbacks.clear();
}

void
RegisterDataProducedCallback(uint32_t nodeId, std::function<void(uint32_t)> cb)
{
    g_dataProducedCallbacks[nodeId] = std::move(cb);
}

void
RegisterDataReceivedCallback(uint32_t nodeId,
                              std::function<void(uint32_t, const std::string&)> cb)
{
    g_dataReceivedCallbacks[nodeId] = std::move(cb);
}

} // namespace ndndsim
} // namespace ns3
