/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * Bridge between ns-3 C++ and NDNd Go simulation library.
 * Implements the callback functions that Go calls into.
 */

#include "ndndsim-go-bridge.h"

#include "ns3/log.h"
#include "ns3/ndndsim-link-tracer.h"
#include "ns3/node.h"
#include "ns3/node-list.h"
#include "ns3/simulator.h"

#include <cstring>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("NdndSimGoBridge");

namespace ndndsim
{

/*
 * Track pending/canceled Go event IDs. We cannot use ns-3 EventId here
 * because Schedule()/Cancel() are thread-unsafe; Go worker threads must use
 * ScheduleWithContext(), and cancellation is implemented logically when the
 * trampoline fires on the simulator thread.
 */
static std::unordered_set<uint64_t> g_pendingEvents;
static std::unordered_set<uint64_t> g_canceledEvents;
static std::mutex g_bridgeMutex;
static bool g_bridgeActive = false;

/*
 * Per-node callbacks for Data produced (producer) and Data received (consumer).
 * Apps register via RegisterDataProducedCallback / RegisterDataReceivedCallback.
 */
static std::unordered_map<uint32_t, std::function<void(uint32_t)>> g_dataProducedCallbacks;
static std::unordered_map<uint32_t, std::vector<std::function<void(uint32_t, const std::string&)>>> g_dataReceivedCallbacks;
static std::function<void()> g_routingConvergedCallback;

/**
 * Callback: NDNd wants to send a packet on an ns-3 NetDevice.
 * This is called from Go code during packet processing.
 */
static void
OnSendPacket(uint32_t nodeId, uint32_t ifIndex, const void* data, uint32_t dataLen)
{
    NS_LOG_FUNCTION(nodeId << ifIndex << dataLen);

    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if (!g_bridgeActive)
        {
            return;
        }
    }

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

    // Tag with NDN payload size so MacTx callbacks can skip the L2 header.
    NdnPayloadTag tag;
    tag.SetPayloadSize(dataLen);
    pkt->AddPacketTag(tag);

    // Send as broadcast (NDN typically uses broadcast on shared media).
    // For point-to-point links, the destination address doesn't matter.
    bool ok = dev->Send(pkt, dev->GetBroadcast(), 0x8624); // NDN EtherType
    if (!ok)
    {
        NS_LOG_WARN("OnSendPacket: dev->Send FAILED (queue full?) node="
                     << nodeId << " ifIndex=" << ifIndex
                     << " pktSize=" << dataLen);
    }
}

/**
 * Callback: NDNd wants to fire a previously scheduled event.
 * This trampoline is scheduled by OnScheduleEvent and calls back into Go.
 */
static void
FireEventTrampoline(uint32_t nodeId, uint64_t eventId)
{
    NS_LOG_FUNCTION(nodeId << eventId);
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        g_pendingEvents.erase(eventId);
        if (g_canceledEvents.erase(eventId) != 0)
        {
            return;
        }
        if (!g_bridgeActive)
        {
            return;
        }
    }
    NdndSimFireEvent(nodeId, eventId);
}

/**
 * Callback: NDNd wants to schedule a future event via ns-3.
 */
static void
OnScheduleEvent(uint32_t nodeId, int64_t delayNs, uint64_t eventId)
{
    NS_LOG_FUNCTION(nodeId << delayNs << eventId);

    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if (!g_bridgeActive)
        {
            return;
        }
        g_pendingEvents.insert(eventId);
        g_canceledEvents.erase(eventId);
    }

    Time delay = NanoSeconds(delayNs);
    Simulator::ScheduleWithContext(nodeId, delay, &FireEventTrampoline, nodeId, eventId);
}

/**
 * Callback: NDNd wants to cancel a previously scheduled event.
 */
static void
OnCancelEvent(uint64_t eventId)
{
    NS_LOG_FUNCTION(eventId);
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if (!g_bridgeActive)
        {
            return;
        }
        if (g_pendingEvents.find(eventId) != g_pendingEvents.end())
        {
            g_canceledEvents.insert(eventId);
        }
    }
}

/**
 * Callback: NDNd wants to know the current simulation time.
 */
static int64_t
OnGetTimeNs()
{
    std::lock_guard<std::mutex> lock(g_bridgeMutex);
    if (!g_bridgeActive)
    {
        return 0;
    }
    return Simulator::Now().GetNanoSeconds();
}

/**
 * Callback: NDNd produced a Data packet on a node.
 */
static void
OnDataProduced(uint32_t nodeId, uint32_t dataSize)
{
    NS_LOG_FUNCTION(nodeId << dataSize);
    std::function<void(uint32_t)> cb;
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if (!g_bridgeActive)
        {
            return;
        }
        auto it = g_dataProducedCallbacks.find(nodeId);
        if (it != g_dataProducedCallbacks.end())
        {
            cb = it->second;
        }
    }
    if (cb)
    {
        cb(dataSize);
    }
}

/**
 * Callback: NDNd delivered Data to a consumer application face.
 */
static void
OnDataReceived(uint32_t nodeId, uint32_t dataSize, const char* dataName, uint32_t nameLen)
{
    NS_LOG_FUNCTION(nodeId << dataSize);
    std::vector<std::function<void(uint32_t, const std::string&)>> callbacks;
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if (!g_bridgeActive)
        {
            return;
        }
        auto it = g_dataReceivedCallbacks.find(nodeId);
        if (it != g_dataReceivedCallbacks.end())
        {
            callbacks = it->second;
        }
    }
    if (!callbacks.empty())
    {
        std::string name(dataName, nameLen);
        for (auto& cb : callbacks)
        {
            cb(dataSize, name);
        }
    }
}

/**
 * Callback: DV routing has converged (all nodes have routes to all others).
 */
static void
OnRoutingConverged()
{
    NS_LOG_FUNCTION_NOARGS();
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        if (!g_bridgeActive)
        {
            return;
        }
        cb = g_routingConvergedCallback;
    }
    if (cb)
    {
        // Use ScheduleWithContext (thread-safe) instead of ScheduleNow which
        // requires the caller to be on the ns-3 main thread.  The routing
        // convergence observer fires from a Go goroutine, so ScheduleNow
        // would trigger an NS_ASSERT abort.
        Simulator::ScheduleWithContext(Simulator::NO_CONTEXT, Time(0), [cb = std::move(cb)]() { cb(); });
    }
}

/*
 * Initialize the Go bridge with ns-3 callbacks.
 */
void
InitGoBridge()
{
    NS_LOG_FUNCTION_NOARGS();
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        g_bridgeActive = true;
    }
    NdndSimInit(&OnSendPacket, &OnScheduleEvent, &OnCancelEvent, &OnGetTimeNs,
                &OnDataProduced, &OnDataReceived, &OnRoutingConverged);
}

/*
 * Cleanup the Go bridge.
 */
void
DestroyGoBridge()
{
    NS_LOG_FUNCTION_NOARGS();
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        g_bridgeActive = false;
    }
    NdndSimDestroy();
    {
        std::lock_guard<std::mutex> lock(g_bridgeMutex);
        g_pendingEvents.clear();
        g_canceledEvents.clear();
        g_dataProducedCallbacks.clear();
        g_dataReceivedCallbacks.clear();
        g_routingConvergedCallback = nullptr;
    }
}

void
RegisterDataProducedCallback(uint32_t nodeId, std::function<void(uint32_t)> cb)
{
    std::lock_guard<std::mutex> lock(g_bridgeMutex);
    g_dataProducedCallbacks[nodeId] = std::move(cb);
}

void
RegisterDataReceivedCallback(uint32_t nodeId,
                              std::function<void(uint32_t, const std::string&)> cb)
{
    std::lock_guard<std::mutex> lock(g_bridgeMutex);
    g_dataReceivedCallbacks[nodeId].push_back(std::move(cb));
}

void
RegisterRoutingConvergedCallback(std::function<void()> cb)
{
    std::lock_guard<std::mutex> lock(g_bridgeMutex);
    NS_ASSERT_MSG(!g_routingConvergedCallback,
                  "RegisterRoutingConvergedCallback: callback already registered; "
                  "second registration would silently replace the first");
    g_routingConvergedCallback = std::move(cb);
}

} // namespace ndndsim
} // namespace ns3
