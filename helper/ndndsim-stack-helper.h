/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndStackHelper: Helper to install NDNd stacks and configure routing.
 */

#ifndef NDNDSIM_STACK_HELPER_H
#define NDNDSIM_STACK_HELPER_H

#include "ns3/net-device-container.h"
#include "ns3/node-container.h"
#include "ns3/ptr.h"

#include <string>
#include <vector>

namespace ns3
{
namespace ndndsim
{

class NdndStack;

/**
 * \brief Helper to install NDNd stacks on ns-3 nodes.
 *
 * Usage:
 * \code
 *   NdndStackHelper stackHelper;
 *   stackHelper.Install(nodes);
 *   stackHelper.AddRoute(nodes.Get(0), "/ndn/prefix", 0, 1);
 * \endcode
 */
class NdndStackHelper
{
  public:
    NdndStackHelper();
    ~NdndStackHelper();

    /**
     * Initialize the Go bridge. Must be called once before any Install().
     */
    static void InitializeBridge();

    /**
     * Destroy the Go bridge. Call at the end of simulation.
     */
    static void DestroyBridge();

    /**
     * Install NDNd stack on all nodes in the container.
     */
    void Install(NodeContainer nodes) const;

    /**
     * Install NDNd stack on a single node.
     */
    Ptr<NdndStack> Install(Ptr<Node> node) const;

    /**
     * Add all routes needed for a simple point-to-point topology.
     * For each pair of directly connected nodes, creates bidirectional
     * routes for all registered prefixes.
     *
     * \param prefix the name prefix
     * \param nodes container of nodes that should have this route
     */
    static void AddRoutesToAll(const std::string& prefix, NodeContainer nodes);

    /**
     * Add a specific route on a node.
     *
     * \param node the node
     * \param prefix NDN name prefix (URI format)
     * \param ifIndex the NetDevice interface index
     * \param cost route cost
     */
    static void AddRoute(Ptr<Node> node,
                           const std::string& prefix,
                           uint32_t ifIndex,
                           uint64_t cost);

    /**
     * Add a specific route on a node using face ID directly.
     */
    static void AddRoute(Ptr<Node> node,
                           const std::string& prefix,
                           uint64_t faceId,
                           uint64_t cost);

    /**
     * Calculate shortest-path routes from each producer to all other nodes.
     *
     * Uses Dijkstra's algorithm on the ns-3 channel graph with link delay
     * as the metric. On each node, installs a FIB entry pointing to the
     * interface that leads toward the producer along the shortest path.
     *
     * \param prefix NDN name prefix to route
     * \param producers nodes that serve this prefix (producers)
     * \param allNodes all nodes in the topology
     */
    static void CalculateRoutes(const std::string& prefix,
                                 NodeContainer producers,
                                 NodeContainer allNodes);

    /**
     * Enable DV routing on all nodes in the container.
     * Each node gets a DV router that discovers routes dynamically
     * via advertisement exchange on its connected faces.
     *
     * \param network the NDN network prefix (e.g., "/ndn")
     * \param nodes container of nodes to enable DV on
     * \param dvConfigJSON JSON config overlay for DV (empty = defaults)
     */
    static void EnableDvRouting(const std::string& network, NodeContainer nodes,
                                 const std::string& dvConfigJSON = "");
};

} // namespace ndndsim
} // namespace ns3

#endif /* NDNDSIM_STACK_HELPER_H */
