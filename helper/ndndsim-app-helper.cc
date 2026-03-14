/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndAppHelper implementation.
 */

#include "ndndsim-app-helper.h"

#include "ns3/log.h"
#include "ns3/node.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("NdndAppHelper");

namespace ndndsim
{

NdndAppHelper::NdndAppHelper(const std::string& typeId)
{
    m_factory.SetTypeId(typeId);
}

NdndAppHelper::~NdndAppHelper()
{
}

void
NdndAppHelper::SetAttribute(const std::string& name, const AttributeValue& value)
{
    m_factory.Set(name, value);
}

ApplicationContainer
NdndAppHelper::Install(Ptr<Node> node) const
{
    ApplicationContainer apps;
    Ptr<Application> app = m_factory.Create<Application>();
    node->AddApplication(app);
    apps.Add(app);
    return apps;
}

ApplicationContainer
NdndAppHelper::Install(NodeContainer nodes) const
{
    ApplicationContainer apps;
    for (auto it = nodes.Begin(); it != nodes.End(); ++it)
    {
        apps.Add(Install(*it));
    }
    return apps;
}

} // namespace ndndsim
} // namespace ns3
