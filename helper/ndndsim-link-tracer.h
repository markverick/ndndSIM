/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndLinkTracer: Measures link traffic classified by NDN packet type
 * (DV routing, prefix sync, management, user Interest/Data).
 */

#ifndef NDNDSIM_LINK_TRACER_H
#define NDNDSIM_LINK_TRACER_H

#include "ns3/event-id.h"
#include "ns3/net-device.h"
#include "ns3/net-device-container.h"
#include "ns3/nstime.h"
#include "ns3/packet.h"
#include "ns3/ptr.h"
#include "ns3/tag.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace ns3
{
namespace ndndsim
{

/**
 * \brief Tag attached to every NDN packet in OnSendPacket.
 *
 * Stores the NDN payload size so that MacTx callbacks can skip
 * whatever L2 header the device prepended.
 */
class NdnPayloadTag : public Tag
{
  public:
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;

    void SetPayloadSize(uint32_t size) { m_size = size; }
    uint32_t GetPayloadSize() const { return m_size; }

    void Serialize(TagBuffer i) const override;
    void Deserialize(TagBuffer i) override;
    uint32_t GetSerializedSize() const override;
    void Print(std::ostream& os) const override;

  private:
    uint32_t m_size = 0;
};

/**
 * Traffic categories derived from NDN packet type + name prefix.
 */
enum class TrafficCategory : uint8_t
{
    DvAdvert = 0, ///< DV advertisement sync & data  (/localhop/.../32=DV/...)
    PFS,          ///< onephase prefix-table sync     (/.../32=DV/32=PFS/...)
    PSD,          ///< twophase prefix-state sync     (/.../32=DV/32=PSD/...)
    Mgmt,          ///< Local management               (/localhost/nlsr/...)
    UserInterest,  ///< User/application Interest
    UserData,      ///< User/application Data
    Other,         ///< Unrecognised / malformed
    COUNT          ///< sentinel — number of categories
};

enum class TrafficPacketType : uint8_t
{
    Interest = 0,
    Data,
    Unknown,
};

struct TrafficPacketInfo
{
    TrafficCategory category = TrafficCategory::Other;
    TrafficPacketType packetType = TrafficPacketType::Unknown;
};

static constexpr size_t kNumCategories = static_cast<size_t>(TrafficCategory::COUNT);

/**
 * \brief Periodically logs link traffic classified by NDN type to CSV.
 *
 * Connects to the MacTx trace source on any NetDevice and parses the
 * raw NDN TLV to classify each packet.  L2-header agnostic — works
 * with PointToPoint, CSMA, Wi-Fi, etc.
 *
 * CSV columns: Time,DvAdvert_Pkts,DvAdvert_Bytes,PFS_Pkts,PFS_Bytes,PSD_Pkts,PSD_Bytes,
 *              Mgmt_Pkts,Mgmt_Bytes,UserInterest_Pkts,UserInterest_Bytes,
 *              UserData_Pkts,UserData_Bytes,Other_Pkts,Other_Bytes
 */
class NdndLinkTracer
{
  public:
    /// Create an interval-sampled tracer (original behaviour).
    static std::shared_ptr<NdndLinkTracer> Create(const std::string& file, Time period);
    static std::shared_ptr<NdndLinkTracer> Create(const std::string& file,
                                                  Time period,
                                                  const std::string& node,
                                                  const std::string& peer,
                                                  bool append);

    /// Create a per-packet event tracer that logs every packet with its timestamp.
    static std::shared_ptr<NdndLinkTracer> CreatePerPacket(const std::string& file);
    static std::shared_ptr<NdndLinkTracer> CreatePerPacket(const std::string& file,
                                                           const std::string& node,
                                                           const std::string& peer,
                                                           bool append);

    ~NdndLinkTracer();

    void ConnectLink(NetDeviceContainer devices);
    void ConnectLink(NetDeviceContainer devices,
                     const std::string& node,
                     const std::string& peer);
    void ConnectDevice(Ptr<NetDevice> dev);
    void Stop();

  private:
    NdndLinkTracer(const std::string& file, Time period);
    NdndLinkTracer(const std::string& file,
                   Time period,
                   const std::string& node,
                   const std::string& peer,
                   bool append);
    NdndLinkTracer(const std::string& file); // per-packet mode
    NdndLinkTracer(const std::string& file,
                   const std::string& node,
                   const std::string& peer,
                   bool append);

    void MacTxCallback(Ptr<const Packet> packet);
    void MacTxCallbackWithContext(std::string context, Ptr<const Packet> packet);
    void WriteStats();
    void ScheduleNext();

    /// Classify a raw NDN TLV buffer.
    static TrafficCategory Classify(const uint8_t* buf, uint32_t len);
    static TrafficPacketInfo ClassifyDetailed(const uint8_t* buf, uint32_t len);

    std::ofstream m_out;
    Time m_period;
    EventId m_event;
    bool m_perPacket = false;
    std::string m_node;
    std::string m_peer;

    struct Counters
    {
        uint64_t packets = 0;
        uint64_t bytes = 0;
    };

    struct LinkCounters
    {
        std::string node;
        std::string peer;
        std::array<Counters, kNumCategories> counters;
    };

    TrafficCategory ClassifyPacket(Ptr<const Packet> packet,
                                   uint32_t* lpBytes,
                                   TrafficPacketType* packetType = nullptr) const;
    void CountPacket(std::array<Counters, kNumCategories>& counters,
                     TrafficCategory cat,
                     uint32_t lpBytes);

    std::array<Counters, kNumCategories> m_counters;
    std::vector<LinkCounters> m_linkCounters;
};

} // namespace ndndsim
} // namespace ns3

#endif /* NDNDSIM_LINK_TRACER_H */
