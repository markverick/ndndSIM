/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndTopologyReader: Reads topology files in the standard ndnSIM
 * annotated-topology format and creates nodes + point-to-point links.
 */

#ifndef NDNDSIM_TOPOLOGY_READER_H
#define NDNDSIM_TOPOLOGY_READER_H

#include "ns3/net-device-container.h"
#include "ns3/node-container.h"
#include "ns3/ptr.h"

#include <map>
#include <string>
#include <vector>

namespace ns3
{
namespace ndndsim
{

/**
 * \brief Reads topology files in the standard ndnSIM annotated format.
 *
 * File format (same as old ndnSIM AnnotatedTopologyReader):
 *
 * \code
 * # Comments start with '#', blank lines are ignored
 *
 * router
 *
 * # node   comment   yPos   xPos   [mpi-partition]
 * Node0    NA        3      1
 * Node1    NA        3      2
 *
 * link
 *
 * # srcNode  dstNode  bandwidth  metric  delay  queue  [lossRate]
 * Node0      Node1    1Mbps      1       10ms   10
 * \endcode
 *
 * Usage:
 * \code
 *   NdndTopologyReader reader;
 *   reader.SetFileName("topology.txt");
 *   NodeContainer nodes = reader.Read();
 *   // nodes are named, accessible via Names::Find<Node>("Node0")
 * \endcode
 */
class NdndTopologyReader
{
  public:
    /// Information about a link parsed from the topology file.
    struct LinkInfo
    {
        Ptr<Node> fromNode;
        Ptr<Node> toNode;
        std::string fromName;
        std::string toName;
        NetDeviceContainer devices;
        std::string dataRate;
        std::string delay;
        uint16_t metric;
        uint32_t maxPackets;
        std::string lossRate;
    };

    NdndTopologyReader();
    ~NdndTopologyReader();

    /**
     * Set the topology filename to read.
     */
    void SetFileName(const std::string& fileName);

    /**
     * Read the topology file and create nodes/links.
     * \return NodeContainer with all created nodes.
     */
    NodeContainer Read();

    /**
     * Get all nodes created by Read().
     */
    NodeContainer GetNodes() const;

    /**
     * Get all links created by Read().
     */
    const std::vector<LinkInfo>& GetLinks() const;

  private:
    std::string m_fileName;
    NodeContainer m_nodes;
    std::vector<LinkInfo> m_links;
    std::map<std::string, Ptr<Node>> m_nodeMap;
};

} // namespace ndndsim
} // namespace ns3

#endif /* NDNDSIM_TOPOLOGY_READER_H */
