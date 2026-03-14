/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndConsumerZipf: NDN consumer that selects content IDs according to
 * a Zipf-Mandelbrot probability distribution.
 */

#ifndef NDNDSIM_CONSUMER_ZIPF_H
#define NDNDSIM_CONSUMER_ZIPF_H

#include "../model/ndndsim-app.h"

#include "ns3/event-id.h"
#include "ns3/random-variable-stream.h"
#include "ns3/traced-callback.h"

#include <vector>

namespace ns3
{
namespace ndndsim
{

/**
 * \brief NDN consumer using Zipf-Mandelbrot distribution for content selection.
 *
 * Instead of requesting sequential content IDs, this consumer picks a
 * content ID in [0, NumberOfContents) with probability proportional to
 * 1 / (k + q)^s  where k is the rank.
 *
 * Attributes:
 *   - Prefix: NDN name prefix
 *   - Frequency: Interest sending rate (Hz)
 *   - NumberOfContents: size of the content catalog
 *   - q: Zipf-Mandelbrot q parameter (default 0)
 *   - s: Zipf-Mandelbrot s parameter (default 0.7)
 */
class NdndConsumerZipf : public NdndApp
{
  public:
    static TypeId GetTypeId();

    NdndConsumerZipf();
    ~NdndConsumerZipf() override;

  protected:
    void OnStart() override;
    void OnStop() override;

  private:
    void SendInterest();
    void SetNumberOfContents(uint32_t numOfContents);
    uint32_t GetNextSeqNo();

    std::string m_prefix;
    double m_frequency;
    uint32_t m_numContents;
    double m_q;
    double m_s;
    EventId m_sendEvent;

    Ptr<UniformRandomVariable> m_rand;
    std::vector<double> m_cdf; ///< Precomputed CDF for Zipf-Mandelbrot

    TracedCallback<uint32_t /* seqNo */> m_interestSentTrace;
};

} // namespace ndndsim
} // namespace ns3

#endif /* NDNDSIM_CONSUMER_ZIPF_H */
