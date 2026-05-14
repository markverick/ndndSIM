package sim

import (
	"fmt"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"

	dv_table "github.com/named-data/ndnd/dv/table"
	enc "github.com/named-data/ndnd/std/encoding"
	"github.com/named-data/ndnd/std/ndn"
	sig "github.com/named-data/ndnd/std/security/signer"
	"github.com/named-data/ndnd/std/types/optional"
	_ndndsim "github.com/named-data/ndndsim"
)

/*
#include "ndndsim_sim.h"
#include <stdlib.h>
*/
import "C"

// --- NS-3 Clock Implementation ---

// globalNextEvID is a process-wide atomic counter so that every Ns3Clock
// produces unique event IDs.  The C++ side stores them in a global
// unordered_map keyed by event ID, so collisions between different nodes
// would cause one node's Cancel to silently remove another node's event.
var globalNextEvID atomic.Uint64

// Ns3Clock implements the Clock interface using ns-3's simulation time.
type Ns3Clock struct {
	nodeID uint32
	mu     sync.Mutex
	events map[EventID]func()
}

// NewNs3Clock creates a clock for a specific node.
func NewNs3Clock(nodeID uint32) *Ns3Clock {
	return &Ns3Clock{
		nodeID: nodeID,
		events: make(map[EventID]func()),
	}
}

func (c *Ns3Clock) Now() time.Time {
	ns := int64(C.callGetTimeNs())
	return time.Unix(0, ns)
}

func (c *Ns3Clock) Schedule(delay time.Duration, callback func()) EventID {
	id := EventID(globalNextEvID.Add(1))

	c.mu.Lock()
	c.events[id] = callback
	c.mu.Unlock()

	delayNs := delay.Nanoseconds()
	if delayNs < 0 {
		delayNs = 0
	}

	C.callScheduleEvent(C.uint32_t(c.nodeID), C.int64_t(delayNs), C.uint64_t(id))
	return id
}

func (c *Ns3Clock) Cancel(id EventID) {
	// Cancel on the C++ scheduler first. If the cancel succeeds the event will
	// never fire; if it fails (already fired / already executing) FireEvent
	// will still find and invoke the callback because we haven't removed it yet.
	C.callCancelEvent(C.uint64_t(id))

	c.mu.Lock()
	delete(c.events, id)
	c.mu.Unlock()
}

// FireEvent is called by ns-3 when a scheduled event fires.
func (c *Ns3Clock) FireEvent(id EventID) {
	c.mu.Lock()
	cb, ok := c.events[id]
	if ok {
		delete(c.events, id)
	}
	c.mu.Unlock()

	if ok && cb != nil {
		cb()
	}
}

// --- Global Runtime (singleton for CGo access) ---

var (
	globalRuntime *Runtime
	globalClocks  sync.Map // nodeID -> *Ns3Clock

	// consumerFlags maps nodeID to all active stop flags for that node's consumer
	// loops.  Using a slice (not sync.Map with a single *int32) ensures that
	// multiple startConsumerLoop calls for the same node can all be stopped via
	// NdndSimStopConsumer.  Guarded by consumerMu.
	consumerMu    sync.Mutex
	consumerFlags map[uint32][]*int32

	// Prefix propagation: total count of PrefixEventAddRemotePrefix events
	// received since simulation start. Used by C++ to detect when prefix
	// propagation has stabilised (count stops growing).
	prefixRemoteAddCount atomic.Int64

	// lastDvAdvReceiptNs is the ns-3 simulation time (nanoseconds) of the most
	// recent DV advertisement received from a neighbor at any node.  Tracks
	// when advertisements pass through transit nodes (in-flight).  Returns -1
	// before the first receipt.
	lastDvAdvReceiptNs atomic.Int64

	// lastPfxSvsDeliveryNs is the ns-3 simulation time (nanoseconds) of the most
	// recent prefix SVS publication delivery to any node's subscription callback.
	// This fires as prefix Data passes through each transit node (in-flight), not
	// when it is added to a routing table. Returns -1 before the first delivery.
	lastPfxSvsDeliveryNs atomic.Int64
)

// --- Exported CGo functions called by ns-3 C++ code ---

//export NdndSimInit
func NdndSimInit(
	sendPacketCb C.NdndSimSendPacketFunc,
	scheduleEventCb C.NdndSimScheduleEventFunc,
	cancelEventCb C.NdndSimCancelEventFunc,
	getTimeNsCb C.NdndSimGetTimeNsFunc,
	dataProducedCb C.NdndSimDataProducedFunc,
	dataReceivedCb C.NdndSimDataReceivedFunc,
) {
	C.setSendPacketCb(sendPacketCb)
	C.setScheduleEventCb(scheduleEventCb)
	C.setCancelEventCb(cancelEventCb)
	C.setGetTimeNsCb(getTimeNsCb)
	C.setDataProducedCb(dataProducedCb)
	C.setDataReceivedCb(dataReceivedCb)

	globalRuntime = NewRuntime()

	consumerMu.Lock()
	consumerFlags = make(map[uint32][]*int32)
	consumerMu.Unlock()

	prefixRemoteAddCount.Store(0)
	lastPfxSvsDeliveryNs.Store(-1)
	lastDvAdvReceiptNs.Store(-1)

	_ndndsim.SetPfxSvsDeliveryCallback(func() {
		ns := _ndndsim.Now().UnixNano()
		lastPfxSvsDeliveryNs.Store(ns)
		// Debug: log every 100th delivery
		count := prefixRemoteAddCount.Load()
		if count%100 == 0 {
			fmt.Printf("PFX_DELIVERY: count=%d ns=%d\n", count, ns)
		}
	})

	_ndndsim.SetDvAdvReceiptCallback(func() {
		lastDvAdvReceiptNs.Store(_ndndsim.Now().UnixNano())
	})

	dv_table.SetPrefixEventObserver(func(ev dv_table.PrefixEvent) {
		if ev.Kind == dv_table.PrefixEventAddRemotePrefix {
			prefixRemoteAddCount.Add(1)
		}
	})
}

//export NdndSimCreateNode
func NdndSimCreateNode(nodeId C.uint32_t) C.int {
	if globalRuntime == nil {
		return -1
	}

	clock := NewNs3Clock(uint32(nodeId))

	// Create node with its own clock
	node := NewNode(uint32(nodeId), clock)

	// Set the Data received callback so the engine notifies C++
	if eng, ok := node.Engine().(*SimEngine); ok {
		nid := nodeId
		eng.onDataReceived = func(nodeID uint32, dataSize uint32, dataName string) {
			cName := C.CString(dataName)
			C.callDataReceived(nid, C.uint32_t(dataSize), cName, C.uint32_t(len(dataName)))
			C.free(unsafe.Pointer(cName))
		}
	}

	globalRuntime.AddNode(uint32(nodeId), node)

	if err := node.Start(); err != nil {
		// Clean up the node registration so a later NdndSimCreateNode with the
		// same ID starts from a clean state. The clock is not yet in globalClocks
		// so no NdndSimFireEvent can reach it.
		globalRuntime.DestroyNode(uint32(nodeId))
		return -1
	}

	// Only expose the clock after Start() succeeds. This ensures NdndSimFireEvent
	// cannot dispatch into a node that is not yet fully initialised.
	globalClocks.Store(uint32(nodeId), clock)
	return 0
}

//export NdndSimDestroyNode
func NdndSimDestroyNode(nodeId C.uint32_t) {
	if globalRuntime == nil {
		return
	}
	globalRuntime.DestroyNode(uint32(nodeId))
	globalClocks.Delete(uint32(nodeId))
	// Stop and remove any consumer loops for this node so that a subsequent
	// NdndSimCreateNode with the same ID doesn't inherit stale stop flags.
	consumerMu.Lock()
	flags := consumerFlags[uint32(nodeId)]
	delete(consumerFlags, uint32(nodeId))
	consumerMu.Unlock()
	for _, f := range flags {
		atomic.StoreInt32(f, 1)
	}
}

//export NdndSimAddFace
func NdndSimAddFace(nodeId C.uint32_t, ifIndex C.uint32_t) C.uint64_t {
	if globalRuntime == nil {
		return 0
	}
	node := globalRuntime.GetNode(uint32(nodeId))
	if node == nil {
		return 0
	}

	nid := uint32(nodeId)
	iidx := uint32(ifIndex)
	faceID := node.AddNetworkFace(iidx, func(faceID uint64, frame []byte) {
		// NDNd -> ns-3: send packet through callback
		if len(frame) == 0 {
			return
		}
		C.callSendPacket(
			C.uint32_t(nid),
			C.uint32_t(iidx),
			unsafe.Pointer(&frame[0]),
			C.uint32_t(len(frame)),
		)
	})
	return C.uint64_t(faceID)
}

//export NdndSimRemoveFace
func NdndSimRemoveFace(nodeId C.uint32_t, ifIndex C.uint32_t) {
	if globalRuntime == nil {
		return
	}
	node := globalRuntime.GetNode(uint32(nodeId))
	if node == nil {
		return
	}
	node.RemoveNetworkFace(uint32(ifIndex))
}

//export NdndSimReceivePacket
func NdndSimReceivePacket(nodeId C.uint32_t, ifIndex C.uint32_t, data unsafe.Pointer, dataLen C.uint32_t) {
	if globalRuntime == nil {
		return
	}
	node := globalRuntime.GetNode(uint32(nodeId))
	if node == nil {
		return
	}

	// Copy the data (C++ memory may be freed after this call)
	frame := C.GoBytes(data, C.int(dataLen))
	// ReceiveOnInterface calls BindNode/UnbindNode internally.
	node.ReceiveOnInterface(uint32(ifIndex), frame)
}

//export NdndSimAddRoute
func NdndSimAddRoute(nodeId C.uint32_t, prefixStr *C.char, prefixLen C.int, faceId C.uint64_t, cost C.uint64_t) {
	if globalRuntime == nil {
		return
	}
	node := globalRuntime.GetNode(uint32(nodeId))
	if node == nil {
		return
	}

	prefix := C.GoStringN(prefixStr, prefixLen)
	name, err := parseNameFromString(prefix)
	if err != nil {
		return
	}
	node.AddRoute(name, uint64(faceId), uint64(cost))
}

//export NdndSimRemoveRoute
func NdndSimRemoveRoute(nodeId C.uint32_t, prefixStr *C.char, prefixLen C.int, faceId C.uint64_t) {
	if globalRuntime == nil {
		return
	}
	node := globalRuntime.GetNode(uint32(nodeId))
	if node == nil {
		return
	}

	prefix := C.GoStringN(prefixStr, prefixLen)
	name, err := parseNameFromString(prefix)
	if err != nil {
		return
	}
	node.RemoveRoute(name, uint64(faceId))
}

//export NdndSimFireEvent
func NdndSimFireEvent(nodeId C.uint32_t, eventId C.uint64_t) {
	val, ok := globalClocks.Load(uint32(nodeId))
	if !ok {
		return
	}
	if globalRuntime == nil {
		return
	}
	clock := val.(*Ns3Clock)
	// Bind the node's hooks so that _ndndsim.AfterFunc/Go calls within the
	// callback chain use the DES clock rather than real time.AfterFunc.
	// Without this, callbacks scheduled via SimTimer.Schedule (e.g. the
	// SimEngine.Express timeout) run without a node binding, causing
	// _ndndsim.AfterFunc to fall back to productionHooks which uses
	// time.AfterFunc — creating real goroutines that call ns-3 C++ APIs
	// from non-main threads (SIGSEGV / NS_ASSERT).
	//
	// SwapNode/RestoreNode is used instead of BindNode/UnbindNode so that
	// callbacks which install their own BindNode+UnbindNode (GoFunc, AfterFunc,
	// heartbeat, deadcheck, maintenance) still work correctly: SwapNode saves
	// and RestoreNode restores the prior state even after inner UnbindNode.
	if node := globalRuntime.GetNode(uint32(nodeId)); node != nil {
		prev := _ndndsim.SwapNode(node.Hooks())
		defer _ndndsim.RestoreNode(prev)
	}
	clock.FireEvent(EventID(eventId))
}

//export NdndSimStartDv
func NdndSimStartDv(nodeId C.uint32_t, networkStr *C.char, networkLen C.int, routerStr *C.char, routerLen C.int, cfgStr *C.char, cfgLen C.int) C.int {
	if globalRuntime == nil {
		return -1
	}
	node := globalRuntime.GetNode(uint32(nodeId))
	if node == nil {
		return -1
	}

	network := C.GoStringN(networkStr, networkLen)
	router := C.GoStringN(routerStr, routerLen)
	cfgJSON := C.GoStringN(cfgStr, cfgLen)

	if err := node.StartDv(network, router, cfgJSON); err != nil {
		return -1
	}
	return 0
}

//export NdndSimStopDv
func NdndSimStopDv(nodeId C.uint32_t) {
	if globalRuntime == nil {
		return
	}
	node := globalRuntime.GetNode(uint32(nodeId))
	if node == nil {
		return
	}
	node.StopDv()
}

//export NdndSimDestroy
func NdndSimDestroy() {
	dv_table.SetPrefixEventObserver(nil)

	prefixRemoteAddCount.Store(0)
	lastPfxSvsDeliveryNs.Store(-1)
	lastDvAdvReceiptNs.Store(-1)

	// Remove stale clock and consumer-flag entries left by DestroyAll.
	// NdndSimDestroyNode removes them individually; NdndSimDestroy must
	// clean up the remainder so a subsequent re-init starts with empty maps.
	globalClocks.Range(func(k, _ any) bool {
		globalClocks.Delete(k)
		return true
	})

	consumerMu.Lock()
	consumerFlags = make(map[uint32][]*int32)
	consumerMu.Unlock()

	// Allow face.Initialize() and table.Initialize() to run again on
	// next re-init (they may reset internal queues/state that degrade
	// across simulation runs).
	simInitMu.Lock()
	simInitDone = false
	simInitMu.Unlock()

	// Clear the global trust root so a subsequent NdndSimInit with a
	// different network prefix doesn't inherit stale certificates.
	ResetSimTrust()

	if globalRuntime != nil {
		globalRuntime.DestroyAll()
		globalRuntime = nil
	}
}

//export NdndSimSetTotalNodes
func NdndSimSetTotalNodes(totalNodes C.int) {
	// DV routing convergence now uses in-flight advertisement detection via
	// NdndSimGetLastDvAdvReceiptNs; this function is a no-op.
}

// NdndSimGetPrefixRemoteAddCount returns the total number of
// PrefixEventAddRemotePrefix events received since the simulation started.
// C++ callers may poll this value at regular intervals: when the count
// stops increasing, prefix propagation has stabilised.
//
//export NdndSimGetPrefixRemoteAddCount
func NdndSimGetPrefixRemoteAddCount() C.int64_t {
	return C.int64_t(prefixRemoteAddCount.Load())
}

// NdndSimGetConvergenceMetric returns a phase-agnostic prefix convergence
// counter. It iterates all simulation nodes and sums a table that reflects
// the actual installed prefix state:
//
//   - twophase (ndnd@dv2): sums forwarder_pet entry counts. The PET is
//     updated synchronously with DV prefix events, so this is the exact
//     count of installed egress-prefix mappings.
//   - onephase (ndnd@main): sums forwarder_fib entry counts. The FIB is
//     installed asynchronously (DV event → RIB update → FIB install), so
//     using the DV event counter (NdndSimGetPrefixRemoteAddCount) would fire
//     before the FIB is fully populated.  The FIB count is the ground truth.
//
// Phase detection: forwarder_pet appears in SimTableMetrics() only for
// twophase nodes (simPhaseTableMetrics returns it unconditionally); it is
// absent in onephase. The function therefore detects the phase automatically.
//
// C++ callers poll this value at regular intervals; once it has been
// unchanged for stableWindow / traceInterval rounds, prefix convergence is
// complete and the simulation can be stopped.
//
//export NdndSimGetConvergenceMetric
func NdndSimGetConvergenceMetric() C.int64_t {
	if globalRuntime == nil {
		return 0
	}
	var fibTotal, petTotal int64
	hasPET := false
	globalRuntime.IterNodes(func(_ uint32, node *Node) {
		metrics := node.Forwarder.SimTableMetrics()
		if dv := node.DvRouter(); dv != nil {
			metrics = append(metrics, dv.Router().SimTableMetrics()...)
		}
		for _, m := range metrics {
			switch m.Table {
			case "forwarder_fib":
				fibTotal += int64(m.EntryCount)
			case "forwarder_pet":
				petTotal += int64(m.EntryCount)
				hasPET = true
			}
		}
	})
	if hasPET {
		return C.int64_t(petTotal)
	}
	return C.int64_t(fibTotal)
}

// NdndSimGetLastPfxSvsDeliveryNs returns the ns-3 simulation time (nanoseconds)
// of the most recent prefix SVS publication delivery to any node's subscription
// callback.  This fires as prefix Data passes through each transit node
// (in-flight), not when it arrives at the destination.  Returns -1 before the
// first delivery.  C++ uses this to detect prefix-Data silence: once
// (now_ns - lastPfxSvsDeliveryNs) exceeds the sync period + epsilon, no prefix
// SVS Data is in-flight and prefix convergence is complete.
//
//export NdndSimGetLastPfxSvsDeliveryNs
func NdndSimGetLastPfxSvsDeliveryNs() C.int64_t {
	return C.int64_t(lastPfxSvsDeliveryNs.Load())
}

// NdndSimGetLastDvAdvReceiptNs returns the ns-3 simulation time (nanoseconds)
// of the most recent DV advertisement received from a neighbor at any node.
// Returns -1 before the first receipt.  C++ uses this to detect DV
// advertisement silence: once (now_ns - lastDvAdvReceiptNs) exceeds the
// heartbeat interval + epsilon, no DV advertisements are in-flight and
// DV routing convergence is complete.
//
//export NdndSimGetLastDvAdvReceiptNs
func NdndSimGetLastDvAdvReceiptNs() C.int64_t {
	return C.int64_t(lastDvAdvReceiptNs.Load())
}

//export NdndSimGetAppFaceId
func NdndSimGetAppFaceId(nodeId C.uint32_t) C.uint64_t {
	if globalRuntime == nil {
		return 0
	}
	node := globalRuntime.GetNode(uint32(nodeId))
	if node == nil {
		return 0
	}
	return C.uint64_t(node.AppFaceID())
}

//export NdndSimRegisterProducer
func NdndSimRegisterProducer(nodeId C.uint32_t, prefixStr *C.char, prefixLen C.int, payloadSize C.uint32_t, freshnessMs C.uint32_t) C.int {
	if globalRuntime == nil {
		return -1
	}
	node := globalRuntime.GetNode(uint32(nodeId))
	if node == nil {
		return -1
	}

	prefix := C.GoStringN(prefixStr, prefixLen)
	name, err := parseNameFromString(prefix)
	if err != nil {
		return -1
	}

	engine := node.Engine()
	pSize := int(payloadSize)
	freshness := time.Duration(freshnessMs) * time.Millisecond
	dataSigner := sig.NewSha256Signer()

	handler := func(args ndn.InterestHandlerArgs) {
		// Fill content with bytes from the Interest name repeated cyclically.
		// This makes each Data payload unique per Interest (deterministic but
		// non-zero), so cache collisions and duplicate deliveries are detectable.
		content := make([]byte, pSize)
		if pSize > 0 {
			nameBytes := []byte(args.Interest.Name().String())
			if len(nameBytes) > 0 {
				for i := range content {
					content[i] = nameBytes[i%len(nameBytes)]
				}
			}
		}
		dataConfig := &ndn.DataConfig{
			ContentType: optional.Some(ndn.ContentTypeBlob),
		}
		if freshness > 0 {
			dataConfig.Freshness = optional.Some(freshness)
		}
		data, err := engine.Spec().MakeData(
			args.Interest.Name(),
			dataConfig,
			enc.Wire{content},
			dataSigner,
		)
		if err != nil {
			return
		}
		args.Reply(data.Wire)
		// Notify C++ that Data was produced
		C.callDataProduced(nodeId, C.uint32_t(data.Wire.Length()))
	}

	if err := engine.AttachHandler(name, handler); err != nil {
		return -1
	}

	// Register the producer prefix with the local forwarder so incoming
	// Interests can reach the app face. twophase uses the PET; onephase uses
	// a direct FIB entry.
	engine.RegisterRoute(name)

	// If DV is running, announce this prefix so it propagates to neighbors
	node.AnnouncePrefixToDv(name, 0)

	return 0
}

//export NdndSimGetRibEntryCount
func NdndSimGetRibEntryCount(nodeId C.uint32_t, prefixStr *C.char, prefixLen C.int) C.int {
	if globalRuntime == nil {
		return 0
	}
	node := globalRuntime.GetNode(uint32(nodeId))
	if node == nil {
		return 0
	}

	entries := node.Forwarder.rib.GetAllEntries()

	// No prefix filter -- count all entries
	if prefixStr == nil || int(prefixLen) == 0 {
		return C.int(len(entries))
	}

	// Count only entries whose name starts with the given prefix
	prefix := C.GoStringN(prefixStr, prefixLen)
	filterName, err := parseNameFromString(prefix)
	if err != nil {
		return 0
	}

	count := 0
	for _, entry := range entries {
		if filterName.IsPrefix(entry.Name) {
			count++
		}
	}
	return C.int(count)
}

//export NdndSimGetDvSuppressionStats
func NdndSimGetDvSuppressionStats(nodeId C.uint32_t, enter *C.uint64_t, ok *C.uint64_t, fail *C.uint64_t) C.int {
	if globalRuntime == nil || enter == nil || ok == nil || fail == nil {
		return -1
	}
	node := globalRuntime.GetNode(uint32(nodeId))
	if node == nil {
		return -1
	}
	dv := node.DvRouter()
	if dv == nil {
		return -1
	}

	stats := dv.PrefixSyncSuppressionStats()
	*enter = C.uint64_t(stats.Enter)
	*ok = C.uint64_t(stats.Ok)
	*fail = C.uint64_t(stats.Fail)
	return 0
}

// NdndSimGetTotalPendingFetchInterests returns the total number of in-flight
// prefix SVS data fetch Interests across all DV routers. This is the sum of
// (Pending - Known) for each router's pfxSvs instance. A non-zero value
// indicates there are Interests that have been sent but not yet satisfied.
//
// C++ callers use this to detect in-flight prefix data: the silence checker
// should not stop the simulation as long as there are pending Interests, even
// if no deliveries have been received recently.
//
//export NdndSimGetTotalPendingFetchInterests
func NdndSimGetTotalPendingFetchInterests() C.uint64_t {
	if globalRuntime == nil {
		return 0
	}
	var total uint64
	globalRuntime.IterNodes(func(_ uint32, node *Node) {
		if dv := node.DvRouter(); dv != nil {
			total += dv.Router().NumPendingFetchInterests()
		}
	})
	return C.uint64_t(total)
}

func tableMetricsReport(node *Node) string {
	metrics := node.Forwarder.SimTableMetrics()
	if dv := node.DvRouter(); dv != nil {
		metrics = append(metrics, dv.Router().SimTableMetrics()...)
	}

	var builder strings.Builder
	for _, metric := range metrics {
		builder.WriteString(metric.Category)
		builder.WriteByte(',')
		builder.WriteString(metric.Table)
		builder.WriteByte(',')
		builder.WriteString(strconv.Itoa(metric.EntryCount))
		builder.WriteByte('\n')
	}
	return builder.String()
}

//export NdndSimGetTableMetricsReport
func NdndSimGetTableMetricsReport(nodeId C.uint32_t) *C.char {
	if globalRuntime == nil {
		return nil
	}
	node := globalRuntime.GetNode(uint32(nodeId))
	if node == nil {
		return nil
	}
	return C.CString(tableMetricsReport(node))
}

//export NdndSimFreeCString
func NdndSimFreeCString(value *C.char) {
	if value != nil {
		C.free(unsafe.Pointer(value))
	}
}

//export NdndSimAnnouncePrefixToDv
func NdndSimAnnouncePrefixToDv(nodeId C.uint32_t, prefixStr *C.char, prefixLen C.int) C.int {
	if globalRuntime == nil {
		return -1
	}
	node := globalRuntime.GetNode(uint32(nodeId))
	if node == nil {
		return -1
	}

	prefix := C.GoStringN(prefixStr, prefixLen)
	name, err := parseNameFromString(prefix)
	if err != nil {
		return -1
	}

	node.AnnouncePrefixToDv(name, 0)
	return 0
}

//export NdndSimWithdrawPrefixFromDv
func NdndSimWithdrawPrefixFromDv(nodeId C.uint32_t, prefixStr *C.char, prefixLen C.int) C.int {
	if globalRuntime == nil {
		return -1
	}
	node := globalRuntime.GetNode(uint32(nodeId))
	if node == nil {
		return -1
	}

	prefix := C.GoStringN(prefixStr, prefixLen)
	name, err := parseNameFromString(prefix)
	if err != nil {
		return -1
	}

	node.WithdrawPrefixFromDv(name)
	return 0
}

//export NdndSimRegisterConsumer
func NdndSimRegisterConsumer(nodeId C.uint32_t, prefixStr *C.char, prefixLen C.int, frequencyHz C.double, lifetimeMs C.uint32_t) C.int {
	if globalRuntime == nil {
		return -1
	}
	node := globalRuntime.GetNode(uint32(nodeId))
	if node == nil {
		return -1
	}

	prefix := C.GoStringN(prefixStr, prefixLen)
	name, err := parseNameFromString(prefix)
	if err != nil {
		return -1
	}

	engine := node.Engine()
	lifetime := time.Duration(lifetimeMs) * time.Millisecond

	val, ok := globalClocks.Load(uint32(nodeId))
	if !ok {
		return -1
	}
	clock := val.(*Ns3Clock)

	stopped := startConsumerLoop(engine, clock, uint32(nodeId), name, float64(frequencyHz), lifetime)
	consumerMu.Lock()
	consumerFlags[uint32(nodeId)] = append(consumerFlags[uint32(nodeId)], stopped)
	consumerMu.Unlock()

	return 0
}

//export NdndSimStopConsumer
func NdndSimStopConsumer(nodeId C.uint32_t) {
	consumerMu.Lock()
	flags := consumerFlags[uint32(nodeId)]
	delete(consumerFlags, uint32(nodeId))
	consumerMu.Unlock()
	for _, f := range flags {
		atomic.StoreInt32(f, 1)
	}
}
