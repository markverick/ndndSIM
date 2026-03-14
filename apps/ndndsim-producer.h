/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndProducer: Simple NDN producer that replies to Interests with Data.
 */

#ifndef NDNDSIM_PRODUCER_H
#define NDNDSIM_PRODUCER_H

#include "../model/ndndsim-app.h"

#include "ns3/string.h"
#include "ns3/uinteger.h"
#include "ns3/traced-callback.h"

namespace ns3
{
namespace ndndsim
{

/**
 * \brief Simple NDN producer that satisfies Interests with Data packets.
 *
 * Registers a prefix in the Go-side FIB and handles incoming Interests
 * by producing Data packets with a configurable payload size.
 */
class NdndProducer : public NdndApp
{
  public:
    static TypeId GetTypeId();

    NdndProducer();
    ~NdndProducer() override;

  protected:
    void OnStart() override;
    void OnStop() override;

  private:
    std::string m_prefix;       ///< NDN name prefix to serve
    uint32_t m_payloadSize;     ///< Size of Data payload in bytes

    /// Trace for satisfied Interests
    TracedCallback<uint32_t /* payloadSize */> m_dataSentTrace;
};

} // namespace ndndsim
} // namespace ns3

#endif /* NDNDSIM_PRODUCER_H */
