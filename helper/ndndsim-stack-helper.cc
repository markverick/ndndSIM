/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndStackHelper implementation.
 */

#include "ndndsim-stack-helper.h"

#include "ns3/log.h"
#include "ns3/node.h"

#include "../model/ndndsim-go-bridge.h"
#include "../model/ndndsim-stack.h"

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

} // namespace ndndsim
} // namespace ns3
