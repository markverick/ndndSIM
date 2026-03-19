/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndStackHelper implementation.
 */

#include "ndndsim-stack-helper.h"

#include "ns3/channel.h"
#include "ns3/log.h"
#include "ns3/node.h"

#include "../model/ndndsim-go-bridge.h"
#include "../model/ndndsim-stack.h"

#include <limits>
#include <queue>
#include <unordered_map>
#include <vector>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("NdndStackHelper");

namespace ndndsim
{

NdndStackHelper::NdndStackHelper()
{
}

NdndStackHelper::~NdndStackHelper()
{
}

void
NdndStackHelper::InitializeBridge()
{
    NS_LOG_FUNCTION_NOARGS();
    InitGoBridge();
}

void
NdndStackHelper::DestroyBridge()
{
    NS_LOG_FUNCTION_NOARGS();
    DestroyGoBridge();
}

void
NdndStackHelper::Install(NodeContainer nodes) const
{
    for (auto it = nodes.Begin(); it != nodes.End(); ++it)
    {
        Install(*it);
    }
}

Ptr<NdndStack>
NdndStackHelper::Install(Ptr<Node> node) const
{
    NS_LOG_FUNCTION(node->GetId());

    // Check if already installed
    Ptr<NdndStack> existing = node->GetObject<NdndStack>();
    if (existing)
    {
        NS_LOG_WARN("NDNd stack already installed on node " << node->GetId());
        return existing;
    }

    Ptr<NdndStack> stack = CreateObject<NdndStack>();
    node->AggregateObject(stack);
    stack->Install();

    return stack;
}

void
NdndStackHelper::AddRoutesToAll(const std::string& prefix, NodeContainer nodes)
{
    for (auto it = nodes.Begin(); it != nodes.End(); ++it)
    {
        Ptr<Node> node = *it;
        Ptr<NdndStack> stack = node->GetObject<NdndStack>();
        if (!stack)
        {
            NS_LOG_WARN("No NDNd stack on node " << node->GetId() << ", skipping route");
            continue;
        }

        // Add route for the prefix on each device/face
        for (uint32_t i = 0; i < node->GetNDevices(); ++i)
        {
            uint64_t faceId = stack->GetFaceId(i);
            if (faceId != 0)
            {
                stack->AddRoute(prefix, faceId, 1);
            }
        }
    }
}

void
NdndStackHelper::AddRoute(Ptr<Node> node,
                            const std::string& prefix,
                            uint32_t ifIndex,
                            uint64_t cost)
{
    Ptr<NdndStack> stack = node->GetObject<NdndStack>();
    NS_ASSERT_MSG(stack, "No NDNd stack on node " << node->GetId());

    uint64_t faceId = stack->GetFaceId(ifIndex);
    NS_ASSERT_MSG(faceId != 0, "No face for ifIndex " << ifIndex << " on node " << node->GetId());

    stack->AddRoute(prefix, faceId, cost);
}

void
NdndStackHelper::AddRoute(Ptr<Node> node,
                            const std::string& prefix,
                            uint64_t faceId,
                            uint64_t cost)
{
    Ptr<NdndStack> stack = node->GetObject<NdndStack>();
    NS_ASSERT_MSG(stack, "No NDNd stack on node " << node->GetId());

    stack->AddRoute(prefix, faceId, cost);
}

void
NdndStackHelper::CalculateRoutes(const std::string& prefix,
                                   NodeContainer producers,
                                   NodeContainer allNodes)
{
    NS_LOG_FUNCTION(prefix);

    // Edge: (neighborNodeId, localIfIndex, linkDelay in microseconds)
    struct Edge
    {
        uint32_t neighbor;
        uint32_t ifIndex;
        double metric; // link delay in microseconds
    };
    std::unordered_map<uint32_t, std::vector<Edge>> adj;

    for (auto it = allNodes.Begin(); it != allNodes.End(); ++it)
    {
        Ptr<Node> node = *it;
        uint32_t nodeId = node->GetId();

        for (uint32_t i = 0; i < node->GetNDevices(); ++i)
        {
            Ptr<NetDevice> dev = node->GetDevice(i);
            if (!dev)
            {
                continue;
            }
            Ptr<Channel> channel = dev->GetChannel();
            if (!channel)
            {
                continue;
            }

            // Use channel propagation delay as link metric.
            // GetDelay() is available on PointToPointChannel and similar.
            // Fall back to 1.0 if the channel has no delay attribute.
            double metric = 1.0;
            TimeValue delayVal;
            if (channel->GetAttributeFailSafe("Delay", delayVal))
            {
                metric = delayVal.Get().GetMicroSeconds();
                if (metric <= 0)
                {
                    metric = 1.0;
                }
            }

            for (std::size_t c = 0; c < channel->GetNDevices(); ++c)
            {
                Ptr<NetDevice> remoteDev = channel->GetDevice(c);
                if (!remoteDev || remoteDev->GetNode() == node)
                {
                    continue;
                }
                uint32_t neighborId = remoteDev->GetNode()->GetId();
                adj[nodeId].push_back({neighborId, i, metric});
            }
        }
    }

    // Dijkstra from each producer
    for (auto pit = producers.Begin(); pit != producers.End(); ++pit)
    {
        Ptr<Node> producer = *pit;
        uint32_t producerId = producer->GetId();

        // dist[nodeId] = shortest distance from producer
        std::unordered_map<uint32_t, double> dist;
        // nextHopIf[nodeId] = the local interface on nodeId toward the producer
        std::unordered_map<uint32_t, uint32_t> nextHopIf;

        // Min-heap: (distance, nodeId)
        using PQEntry = std::pair<double, uint32_t>;
        std::priority_queue<PQEntry, std::vector<PQEntry>, std::greater<PQEntry>> pq;

        dist[producerId] = 0.0;
        pq.push({0.0, producerId});

        while (!pq.empty())
        {
            auto [d, curr] = pq.top();
            pq.pop();

            if (d > dist[curr])
            {
                continue; // stale entry
            }

            for (auto& edge : adj[curr])
            {
                double newDist = d + edge.metric;
                auto it = dist.find(edge.neighbor);
                if (it == dist.end() || newDist < it->second)
                {
                    dist[edge.neighbor] = newDist;

                    // Find the interface on the neighbor that connects back to curr
                    for (auto& reverseEdge : adj[edge.neighbor])
                    {
                        if (reverseEdge.neighbor == curr)
                        {
                            nextHopIf[edge.neighbor] = reverseEdge.ifIndex;
                            break;
                        }
                    }

                    pq.push({newDist, edge.neighbor});
                }
            }
        }

        // Install routes on each non-producer node
        for (auto nit = allNodes.Begin(); nit != allNodes.End(); ++nit)
        {
            Ptr<Node> node = *nit;
            uint32_t nodeId = node->GetId();

            if (nodeId == producerId)
            {
                continue;
            }

            auto ifIt = nextHopIf.find(nodeId);
            if (ifIt == nextHopIf.end())
            {
                NS_LOG_WARN("No route from node " << nodeId << " to producer " << producerId
                                                    << " for prefix " << prefix);
                continue;
            }

            Ptr<NdndStack> stack = node->GetObject<NdndStack>();
            if (!stack)
            {
                NS_LOG_WARN("No NDNd stack on node " << nodeId);
                continue;
            }

            uint32_t ifIndex = ifIt->second;
            uint64_t faceId = stack->GetFaceId(ifIndex);
            if (faceId == 0)
            {
                NS_LOG_WARN("No face for ifIndex " << ifIndex << " on node " << nodeId);
                continue;
            }

            uint64_t cost = static_cast<uint64_t>(dist[nodeId]);
            stack->AddRoute(prefix, faceId, cost);
            NS_LOG_DEBUG("Route " << prefix << " on node " << nodeId
                                   << " -> face " << faceId << " (ifIndex=" << ifIndex
                                   << ", cost=" << cost << ")");
        }
    }
}

void
NdndStackHelper::EnableDvRouting(const std::string& network, NodeContainer nodes,
                                  const std::string& dvConfigJSON)
{
    NS_LOG_FUNCTION(network);

    for (auto it = nodes.Begin(); it != nodes.End(); ++it)
    {
        Ptr<Node> node = *it;
        uint32_t nodeId = node->GetId();

        Ptr<NdndStack> stack = node->GetObject<NdndStack>();
        if (!stack)
        {
            NS_LOG_WARN("No NDNd stack on node " << nodeId);
            continue;
        }

        // Build router name: network + "/nodeN"
        std::string routerName = network + "/node" + std::to_string(nodeId);

        int rc = NdndSimStartDv(nodeId,
                                 const_cast<char*>(network.c_str()),
                                 static_cast<int>(network.size()),
                                 const_cast<char*>(routerName.c_str()),
                                 static_cast<int>(routerName.size()),
                                 const_cast<char*>(dvConfigJSON.c_str()),
                                 static_cast<int>(dvConfigJSON.size()));
        if (rc != 0)
        {
            NS_LOG_ERROR("Failed to start DV routing on node " << nodeId);
            continue;
        }

        NS_LOG_INFO("DV routing enabled on node " << nodeId << " router=" << routerName);
    }
}

} // namespace ndndsim
} // namespace ns3
