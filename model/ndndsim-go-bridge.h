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

    /*
     * Functions exported by the Go shared library.
     * These are implemented in sim/cgo_export.go.
     */

    /** Initialize the NDNd simulation runtime with callback pointers. */
    extern void NdndSimInit(NdndSimSendPacketFunc sendPacketCb,
                             NdndSimScheduleEventFunc scheduleEventCb,
                             NdndSimCancelEventFunc cancelEventCb,
                             NdndSimGetTimeNsFunc getTimeNsCb);

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
                                        uint32_t payloadSize);

    /** Destroy all nodes and clean up the simulation runtime. */
    extern void NdndSimDestroy(void);

#ifdef __cplusplus
}

namespace ns3
{
namespace ndndsim
{

/** Initialize the Go bridge with ns-3 callbacks. */
void InitGoBridge();

/** Cleanup the Go bridge. */
void DestroyGoBridge();

} // namespace ndndsim
} // namespace ns3

#endif

#endif /* NDNDSIM_GO_BRIDGE_H */
