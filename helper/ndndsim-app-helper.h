/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndAppHelper: Helper to install NDN applications on nodes.
 */

#ifndef NDNDSIM_APP_HELPER_H
#define NDNDSIM_APP_HELPER_H

#include "ns3/application-container.h"
#include "ns3/node-container.h"
#include "ns3/object-factory.h"
#include "ns3/ptr.h"

#include <string>

namespace ns3
{
namespace ndndsim
{

/**
 * \brief Helper to install NDN applications on nodes.
 *
 * Usage:
 * \code
 *   NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
 *   consumerHelper.SetAttribute("Prefix", StringValue("/ndn/test"));
 *   consumerHelper.Install(consumer);
 * \endcode
 */
class NdndAppHelper
{
  public:
    /**
     * Create an app helper for the given application TypeId.
     */
    explicit NdndAppHelper(const std::string& typeId);
    ~NdndAppHelper();

    /**
     * Set an attribute on the application.
     */
    void SetAttribute(const std::string& name, const AttributeValue& value);

    /**
     * Install the application on a single node.
     */
    ApplicationContainer Install(Ptr<Node> node) const;

    /**
     * Install the application on multiple nodes.
     */
    ApplicationContainer Install(NodeContainer nodes) const;

  private:
    ObjectFactory m_factory;
};

} // namespace ndndsim
} // namespace ns3

#endif /* NDNDSIM_APP_HELPER_H */
