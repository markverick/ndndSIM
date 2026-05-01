/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * C bridge header for communicating with the NDNd Go simulation library.
 * This header declares the functions exported by the Go shared library
 * and the callback types that ns-3 must provide.
 */

#ifndef NDNDSIM_GO_BRIDGE_H
#define NDNDSIM_GO_BRIDGE_H

#include <cstdint>

#ifdef __cplusplus
extern "C"
{
#endif

    /*
     * Callback types - implemented by ns-3 C++ code, called by Go.
     */

    /** Called when NDNd wants to send a packet out on a network interface. */
    typedef void (*NdndSimSendPacketFunc)(uint32_t nodeId,
                                          uint32_t ifIndex,
                                          const void* data,
                                          uint32_t dataLen);

    /** Called when NDNd wants to schedule a future event via ns-3. */
    typedef void (*NdndSimScheduleEventFunc)(uint32_t nodeId,
                                              int64_t delayNs,
                                              uint64_t eventId);

    /** Called when NDNd wants to cancel a previously scheduled event. */
    typedef void (*NdndSimCancelEventFunc)(uint64_t eventId);

    /** Called when NDNd needs the current simulation time in nanoseconds. */
    typedef int64_t (*NdndSimGetTimeNsFunc)(void);

    /** Called when NDNd producer generates a Data packet. */
    typedef void (*NdndSimDataProducedFunc)(uint32_t nodeId, uint32_t dataSize);

    /** Called when NDNd delivers Data to a consumer application. */
    typedef void (*NdndSimDataReceivedFunc)(uint32_t nodeId, uint32_t dataSize,
                                            const char* dataName, uint32_t nameLen);

    /** Called once when DV routing has converged (all nodes reachable). */
    typedef void (*NdndSimRoutingConvergedFunc)(void);

    /*
     * Functions exported by the Go shared library.
     * These are implemented in sim/cgo_export.go.
     */

    /** Initialize the NDNd simulation runtime with callback pointers. */
    extern void NdndSimInit(NdndSimSendPacketFunc sendPacketCb,
                             NdndSimScheduleEventFunc scheduleEventCb,
                             NdndSimCancelEventFunc cancelEventCb,
                             NdndSimGetTimeNsFunc getTimeNsCb,
                             NdndSimDataProducedFunc dataProducedCb,
                             NdndSimDataReceivedFunc dataReceivedCb,
                             NdndSimRoutingConvergedFunc routingConvergedCb);

    /** Tell the Go runtime how many DV nodes exist so it can detect convergence. */
    extern void NdndSimSetTotalNodes(int totalNodes);

    /** Create a new NDNd simulation node. Returns 0 on success, -1 on error. */
    extern int NdndSimCreateNode(uint32_t nodeId);

    /** Destroy an NDNd simulation node. */
    extern void NdndSimDestroyNode(uint32_t nodeId);

    /** Add a network face for an ns-3 NetDevice interface. Returns the face ID. */
    extern uint64_t NdndSimAddFace(uint32_t nodeId, uint32_t ifIndex);

    /** Remove a network face for an ns-3 NetDevice interface. */
    extern void NdndSimRemoveFace(uint32_t nodeId, uint32_t ifIndex);

    /** Deliver a received packet to NDNd on a specific interface. */
    extern void NdndSimReceivePacket(uint32_t nodeId,
                                      uint32_t ifIndex,
                                      void* data,
                                      uint32_t dataLen);

    /** Add a FIB route on a node. */
    extern void NdndSimAddRoute(uint32_t nodeId,
                                 char* prefixStr,
                                 int prefixLen,
                                 uint64_t faceId,
                                 uint64_t cost);

    /** Remove a FIB route on a node. */
    extern void NdndSimRemoveRoute(uint32_t nodeId,
                                    char* prefixStr,
                                    int prefixLen,
                                    uint64_t faceId);

    /** Fire a previously scheduled event. Called by ns-3's event system. */
    extern void NdndSimFireEvent(uint32_t nodeId, uint64_t eventId);

    /** Get the internal application face ID for a node. */
    extern uint64_t NdndSimGetAppFaceId(uint32_t nodeId);

    /** Register a producer on a node that replies to Interests with Data. Returns 0 on success. */
    extern int NdndSimRegisterProducer(uint32_t nodeId,
                                        char* prefixStr,
                                        int prefixLen,
                                        uint32_t payloadSize,
                                        uint32_t freshnessMs);

    /** Announce a prefix to this node's DV router (no app installed). Returns 0 on success. */
    extern int NdndSimAnnouncePrefixToDv(uint32_t nodeId,
                                          char* prefixStr,
                                          int prefixLen);

    /** Withdraw a prefix from this node's DV router. Returns 0 on success. */
    extern int NdndSimWithdrawPrefixFromDv(uint32_t nodeId,
                                            char* prefixStr,
                                            int prefixLen);

    /** Register a Go-side consumer on a node that sends Interests for <prefix>/<seqno>.
     *  frequencyHz: sending rate (e.g. 10.0 for 10 Hz)
     *  lifetimeMs: Interest lifetime in milliseconds
     *  Returns 0 on success, -1 on error. */
    extern int NdndSimRegisterConsumer(uint32_t nodeId,
                                        char* prefixStr,
                                        int prefixLen,
                                        double frequencyHz,
                                        uint32_t lifetimeMs);

    /** Stop a Go-side consumer on a node, preventing further Interest sends. */
    extern void NdndSimStopConsumer(uint32_t nodeId);

    /** Start DV routing on a node. Returns 0 on success.
     *  cfgStr/cfgLen: JSON config overlay (empty = use defaults). */
    extern int NdndSimStartDv(uint32_t nodeId,
                               char* networkStr,
                               int networkLen,
                               char* routerStr,
                               int routerLen,
                               char* cfgStr,
                               int cfgLen);

    /** Stop DV routing on a node. */
    extern void NdndSimStopDv(uint32_t nodeId);

    /** Get the number of RIB entries on a node (for convergence detection).
     *  If prefixStr is non-NULL, counts only entries whose name starts with prefix. */
    extern int NdndSimGetRibEntryCount(uint32_t nodeId, char* prefixStr, int prefixLen);

    /** Get PrefixSync SVS suppression counters for a node's DV router.
     *  Returns 0 on success, -1 if DV is unavailable. */
    extern int NdndSimGetDvSuppressionStats(uint32_t nodeId,
                                            uint64_t* enter,
                                            uint64_t* ok,
                                            uint64_t* fail);

    /** Get a newline-separated per-table metrics report for a node.
     *  Each line is: category,table,entry_count */
    extern char* NdndSimGetTableMetricsReport(uint32_t nodeId);

    /** Free a string returned by NdndSimGetTableMetricsReport. */
    extern void NdndSimFreeCString(char* value);

    /** Get DV convergence for a prefix as
     *  (last AddRemotePrefix receive time - first GlobalAnnounce origin time),
     *  in nanoseconds of simulation time. Returns -1 if unavailable. */
    extern int64_t NdndSimGetDvUpdateSpanNs(char* prefixStr, int prefixLen);

    /** Get routing convergence time in nanoseconds of simulation time.
     *  Convergence = time from first RouterReachable event to the event
     *  that makes ALL nodes have routes to ALL other nodes.
     *  totalNodes must match the number of DV-enabled nodes.
     *  Returns -1 if not all nodes have converged. */
    extern int64_t NdndSimGetRoutingConvergenceNs(int totalNodes);

    /** Get the total number of PrefixEventAddRemotePrefix events received
     *  since simulation start.  C++ may poll this value at regular intervals
     *  to detect when prefix propagation has stabilised (count stops growing). */
    extern int64_t NdndSimGetPrefixRemoteAddCount(void);

    /** Destroy all nodes and clean up the simulation runtime. */
    extern void NdndSimDestroy(void);

    /** Export the DV routing state of all nodes to a JSON snapshot file.
     *  Returns 0 on success, -1 on error. */
    extern int NdndSimExportSnapshot(const char* path);

    /** Import DV routing state for all nodes from a JSON snapshot file.
     *  Must be called after NdndSimStartDv for all nodes but before the
     *  simulator advances time.  Returns 0 on success, -1 on error. */
    extern int NdndSimImportSnapshot(const char* path);

#ifdef __cplusplus
}

#include <functional>
#include <string>

namespace ns3
{
namespace ndndsim
{

/** Initialize the Go bridge with ns-3 callbacks. */
void InitGoBridge();

/** Cleanup the Go bridge. */
void DestroyGoBridge();

/** Register a callback invoked when Go produces Data on a node. */
void RegisterDataProducedCallback(uint32_t nodeId, std::function<void(uint32_t)> cb);

/** Register a callback invoked when Go delivers Data to a consumer on a node.
 *  The callback receives (dataSize, dataName). */
void RegisterDataReceivedCallback(uint32_t nodeId,
                                   std::function<void(uint32_t, const std::string&)> cb);

/** Register a callback invoked once when DV routing converges.
 *  Must be called before Simulator::Run(). */
void RegisterRoutingConvergedCallback(std::function<void()> cb);

} // namespace ndndsim
} // namespace ns3

#endif

#endif /* NDNDSIM_GO_BRIDGE_H */
