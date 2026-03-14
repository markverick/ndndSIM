/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndApp implementation.
 */

#include "ndndsim-app.h"

#include "ndndsim-stack.h"

#include "ns3/log.h"
#include "ns3/node.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("NdndApp");

namespace ndndsim
{

NS_OBJECT_ENSURE_REGISTERED(NdndApp);

TypeId
NdndApp::GetTypeId()
{
    static TypeId tid = TypeId("ns3::ndndsim::NdndApp")
                            .SetParent<Application>()
                            .SetGroupName("NdndSIM")
                            .AddConstructor<NdndApp>();
    return tid;
}

NdndApp::NdndApp()
    : m_active(false)
{
}

NdndApp::~NdndApp()
{
}

void
NdndApp::DoDispose()
{
    Application::DoDispose();
}

Ptr<NdndStack>
NdndApp::GetStack() const
{
    Ptr<Node> node = GetNode();
    if (!node)
    {
        return nullptr;
    }
    return node->GetObject<NdndStack>();
}

void
NdndApp::StartApplication()
{
    NS_LOG_FUNCTION(this);
    m_active = true;
    OnStart();
}

void
NdndApp::StopApplication()
{
    NS_LOG_FUNCTION(this);
    m_active = false;
    OnStop();
}

void
NdndApp::OnStart()
{
    // Override in subclasses
}

void
NdndApp::OnStop()
{
    // Override in subclasses
}

} // namespace ndndsim
} // namespace ns3
