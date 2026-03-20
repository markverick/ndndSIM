/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndConsumer: Simple NDN consumer that periodically sends Interests.
 */

#ifndef NDNDSIM_CONSUMER_H
#define NDNDSIM_CONSUMER_H

#include "../model/ndndsim-app.h"

#include "ns3/nstime.h"
#include "ns3/traced-callback.h"

namespace ns3
{
namespace ndndsim
{

/**
 * \brief Simple NDN consumer that sends periodic Interests.
 *
 * Sends an Interest for prefix/seqno at a configurable rate.
 * Traces received Data packets.
 */
class NdndConsumer : public NdndApp
{
  public:
    static TypeId GetTypeId();

    NdndConsumer();
    ~NdndConsumer() override;

  protected:
    void OnStart() override;
    void OnStop() override;

  private:
    std::string m_prefix;    ///< NDN name prefix
    double m_frequency;      ///< Interest sending frequency (Hz)
    Time m_lifetime;         ///< Interest lifetime

    /// Trace for sent Interests
    TracedCallback<uint32_t /* seqNo */> m_interestSentTrace;

    /// Trace for received Data
    TracedCallback<uint32_t /* dataSize */> m_dataReceivedTrace;
};

} // namespace ndndsim
} // namespace ns3

#endif /* NDNDSIM_CONSUMER_H */
