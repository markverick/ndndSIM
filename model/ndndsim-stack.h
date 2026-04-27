/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndStack: Per-node NDNd network stack, installed as an ns-3 Object
 * aggregated to a Node. Manages the lifecycle of the Go-side NDNd instance,
 * creates faces for each NetDevice, and handles packet receive callbacks.
 */

#ifndef NDNDSIM_STACK_H
#define NDNDSIM_STACK_H

#include "ns3/net-device.h"
#include "ns3/node.h"
#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/ptr.h"

#include <map>

namespace ns3
{
namespace ndndsim
{

/**
 * \brief NDNd protocol stack for a single ns-3 Node.
 *
 * When aggregated to a Node, this object creates the Go-side NDNd
 * forwarder instance, registers a face per NetDevice, and hooks into
 * NetDevice receive callbacks so that incoming NDN packets are delivered
 * to the forwarder.
 */
class NdndStack : public Object
{
  public:
    static TypeId GetTypeId();

    NdndStack();
    ~NdndStack() override;

    /**
     * Install the NDNd stack on the associated node.
     * Creates the Go-side node and faces.
     */
    void Install();

    /**
     * Add a FIB route on this node.
     * \param prefix NDN name prefix (URI format, e.g., "/ndn/example")
     * \param faceId face ID returned by the Go forwarder
     * \param cost route cost
     */
    void AddRoute(const std::string& prefix, uint64_t faceId, uint64_t cost);

    /**
     * Remove a FIB route on this node.
     */
    void RemoveRoute(const std::string& prefix, uint64_t faceId);

    /**
     * Register a producer prefix using the phase-appropriate local forwarding
     * table, then announce the prefix to DV for propagation to remote nodes.
     */
    void RegisterProducer(const std::string& prefix);

    /**
     * Announce a prefix to this node's DV router (routing-only, no app).
     * DV will advertise it to all neighbours.
     */
    void AnnouncePrefixToDv(const std::string& prefix);

    /**
     * Withdraw a previously announced prefix from this node's DV router.
     */
    void WithdrawPrefixFromDv(const std::string& prefix);

    /**
     * Remove the NDNd face bound to a NetDevice interface.
     */
    void DeactivateInterface(uint32_t ifIndex);

    /**
     * Recreate the NDNd face bound to a NetDevice interface.
     */
    void ReactivateInterface(uint32_t ifIndex);

    /**
     * Get the face ID for a specific NetDevice interface index.
     * Returns 0 if not found.
     */
    uint64_t GetFaceId(uint32_t ifIndex) const;

    /**
     * Get the number of RIB entries on this node.
     * If prefix is non-empty, counts only entries whose name starts with prefix.
     * Useful for convergence detection: after DV converges in an N-node
     * network, each node should have at least N entries under the network prefix.
     */
    int GetRibEntryCount(const std::string& prefix = "") const;

    /**
     * Get PrefixSync SVS suppression counters for this node's DV router.
     * Returns true on success.
     */
    bool GetDvSuppressionStats(uint64_t& enter, uint64_t& ok, uint64_t& fail) const;

    /**
     * Get a newline-separated per-table metrics report for this node.
      * Each line is: category,table,entry_count.
     */
    std::string GetTableMetricsReport() const;

  protected:
    void DoDispose() override;
    void NotifyNewAggregate() override;

  private:
    /**
     * Callback for packets received on a NetDevice.
     */
    void ReceiveFromDevice(Ptr<NetDevice> device,
                            Ptr<const Packet> packet,
                            uint16_t protocol,
                            const Address& sender,
                            const Address& receiver,
                            NetDevice::PacketType packetType);

    Ptr<Node> m_node;
    bool m_installed;

    /// Map from NetDevice interface index to Go face ID
    std::map<uint32_t, uint64_t> m_faceIds;
};

} // namespace ndndsim
} // namespace ns3

#endif /* NDNDSIM_STACK_H */
