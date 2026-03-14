/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndApp: Base class for NDN applications running on simulated nodes.
 * Subclass this to create Consumer, Producer, or custom NDN apps.
 */

#ifndef NDNDSIM_APP_H
#define NDNDSIM_APP_H

#include "ns3/application.h"
#include "ns3/ptr.h"

#include <string>

namespace ns3
{
namespace ndndsim
{

class NdndStack;

/**
 * \brief Base class for NDN applications running on NDNd nodes.
 *
 * NdndApp is an ns-3 Application that interacts with the NDNd stack.
 * Subclasses implement OnStart/OnStop to perform NDN operations.
 */
class NdndApp : public Application
{
  public:
    static TypeId GetTypeId();

    NdndApp();
    ~NdndApp() override;

    /**
     * Get the NDNd stack on this node.
     */
    Ptr<NdndStack> GetStack() const;

  protected:
    void DoDispose() override;
    void StartApplication() override;
    void StopApplication() override;

    /**
     * Called when the application starts. Override in subclasses.
     */
    virtual void OnStart();

    /**
     * Called when the application stops. Override in subclasses.
     */
    virtual void OnStop();

  private:
    bool m_active;
};

} // namespace ndndsim
} // namespace ns3

#endif /* NDNDSIM_APP_H */
