/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndConsumer: Simple NDN consumer that periodically sends Interests.
 */

#ifndef NDNDSIM_CONSUMER_H
#define NDNDSIM_CONSUMER_H

#include "../model/ndndsim-app.h"

#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/random-variable-stream.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"
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
    void SendInterest();

    std::string m_prefix;    ///< NDN name prefix
    double m_frequency;      ///< Interest sending frequency (Hz)
    Time m_lifetime;         ///< Interest lifetime (ndnSIM: LifeTime)
    std::string m_randomize; ///< Randomization: "none", "uniform", "exponential"
    uint32_t m_seqNo;        ///< Current sequence number
    EventId m_sendEvent;     ///< Periodic send event
    Ptr<RandomVariableStream> m_random; ///< Random variable for inter-Interest gaps

    /// Trace for sent Interests
    TracedCallback<uint32_t /* seqNo */> m_interestSentTrace;

    /// Trace for received Data
    TracedCallback<uint32_t /* dataSize */> m_dataReceivedTrace;
};

} // namespace ndndsim
} // namespace ns3

#endif /* NDNDSIM_CONSUMER_H */
