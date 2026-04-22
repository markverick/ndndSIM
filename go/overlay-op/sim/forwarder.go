package sim

import (
	"sync"
	"sync/atomic"
	"time"

	_ndndsim "github.com/named-data/ndndsim"
	"github.com/named-data/ndnd/fw/defn"
	"github.com/named-data/ndnd/fw/dispatch"
	"github.com/named-data/ndnd/fw/fw"
	"github.com/named-data/ndnd/fw/table"
	enc "github.com/named-data/ndnd/std/encoding"
	spec_mgmt "github.com/named-data/ndnd/std/ndn/mgmt_2022"
)

// globalFaceID is a process-wide atomic counter ensuring face IDs are unique
// across all simulated nodes. This is critical because faces are registered in
// the global dispatch.FaceDispatch table shared by all forwarder threads.
var globalFaceID atomic.Uint64

const (
	// Maintenance interval for PIT/CS expiry and dead nonce list cleanup.
	simMaintenanceInterval = 100 * time.Millisecond
)

// SimForwarder wraps a real fw.Thread to provide per-node NDN forwarding
// in simulation mode.  At main@51774b8 there is no PrefixEgressTable (PET)
// or separate MulticastStrategyTable; only a single per-node FIB is used.
type SimForwarder struct {
	thread *fw.Thread

	clock Clock

	// Per-node hooks (goroutine-local state: FIB, clock, scheduler).
	hooks *_ndndsim.NodeHooks

	// Per-node RIB (routes go through here so readvertise fires)
	rib *table.RibTable

	// Per-node FIB: stored here and registered in the node hooks so that
	// goroutine-local simFib() returns the right instance.
	fib table.FibStrategy

	// Per-node face table (face ID -> DispatchFace)
	faces  map[uint64]*DispatchFace
	faceMu sync.Mutex

	// Scheduled maintenance event
	maintEvent EventID

	// nodeMu serialises all forwarding-pipeline calls for this node.
	// nodeHolder tracks the goroutine ID of the current lock holder so that
	// re-entrant calls from the same goroutine (clock callback → Express →
	// ReceivePacket → withNodeFib) can proceed without deadlocking.
	nodeMu     sync.Mutex
	nodeHolder int64 // 0 = unlocked
}

// NewSimForwarder creates a new simulation forwarder backed by a real fw.Thread.
// At main@51774b8 there is no PET; all forwarding goes through the FIB.
func NewSimForwarder(clock Clock, hooks *_ndndsim.NodeHooks) *SimForwarder {
	rib := &table.RibTable{}
	rib.InitRoot()

	fib := table.NewFibStrategyTree()

	// Register per-node FIB in hooks so that goroutine-local simFib()
	// returns the correct instance after BindNode is called.
	hooks.Fib = fib

	fwd := &SimForwarder{
		clock: clock,
		hooks: hooks,
		fib:   fib,
		faces: make(map[uint64]*DispatchFace),
		rib:   rib,
	}

	// Create a real forwarding thread (ID 0 -- single-threaded in sim).
	fwd.thread = fw.NewThread(0)
	fwd.thread.SetFib(fib)

	return fwd
}

// Start schedules periodic maintenance.
func (fwd *SimForwarder) Start() {
	fwd.scheduleMaintenance()
}

// Stop cancels scheduled maintenance.
func (fwd *SimForwarder) Stop() {
	if fwd.maintEvent != 0 {
		fwd.clock.Cancel(fwd.maintEvent)
		fwd.maintEvent = 0
	}
}

func (fwd *SimForwarder) scheduleMaintenance() {
	fwd.maintEvent = fwd.clock.Schedule(simMaintenanceInterval, func() {
		fwd.withNodeFib(func() {
			fwd.thread.RunMaintenance()
		})
		fwd.scheduleMaintenance()
	})
}

// --- Face management ---

// AddFace creates a new DispatchFace, registers it in the global dispatch table,
// and returns its face ID.
func (fwd *SimForwarder) AddFace(scope defn.Scope, linkType defn.LinkType, sendFunc FwSendFunc) uint64 {
	fwd.faceMu.Lock()
	defer fwd.faceMu.Unlock()

	id := globalFaceID.Add(1)

	face := NewDispatchFace(id, scope, linkType, sendFunc)
	fwd.faces[id] = face
	dispatch.AddFace(id, face)

	return id
}

// faceCleanable is implemented by FIB types that support per-face cleanup.
type faceCleanable interface {
	CleanUpFace(faceID uint64)
}

// RemoveFace removes a face from both the local table and global dispatch.
func (fwd *SimForwarder) RemoveFace(id uint64) {
	fwd.faceMu.Lock()
	defer fwd.faceMu.Unlock()

	fwd.withNodeFib(func() {
		fwd.rib.CleanUpFace(id)
	})
	// Remove nexthops for this face from the per-node FIB.
	if fc, ok := fwd.fib.(faceCleanable); ok {
		fc.CleanUpFace(id)
	}

	delete(fwd.faces, id)
	dispatch.RemoveFace(id)
}

// GetFace returns a face by ID.
func (fwd *SimForwarder) GetFace(id uint64) *DispatchFace {
	fwd.faceMu.Lock()
	defer fwd.faceMu.Unlock()
	return fwd.faces[id]
}

// --- RIB/FIB management ---

// lockNode acquires the per-node forwarding lock.
func (fwd *SimForwarder) lockNode() bool {
	id := _ndndsim.GoroutineID()
	if atomic.LoadInt64(&fwd.nodeHolder) == id {
		return false
	}
	fwd.nodeMu.Lock()
	atomic.StoreInt64(&fwd.nodeHolder, id)
	return true
}

func (fwd *SimForwarder) unlockNode() {
	atomic.StoreInt64(&fwd.nodeHolder, 0)
	fwd.nodeMu.Unlock()
}

// withNodeFib ensures that the current goroutine is bound to this node's hooks
// for the duration of f(), and serialises concurrent callers.
func (fwd *SimForwarder) withNodeFib(f func()) {
	locked := fwd.lockNode()
	if locked {
		defer fwd.unlockNode()
	}
	if _ndndsim.GetHooks() == fwd.hooks {
		f()
		return
	}
	prev := _ndndsim.SwapNode(fwd.hooks)
	defer _ndndsim.RestoreNode(prev)
	f()
}

// SetMulticastStrategy applies a forwarding strategy for a prefix.
// At main@51774b8 there is no separate multicast strategy table; this applies
// to the regular FIB as a best-effort substitute.
func (fwd *SimForwarder) SetMulticastStrategy(prefix enc.Name, strategy enc.Name) {
	fwd.fib.SetStrategyEnc(prefix, strategy)
}

// AddRoute adds a route through this node's RIB so that readvertise fires.
func (fwd *SimForwarder) AddRoute(name enc.Name, faceID uint64, cost uint64) {
	fwd.AddDirectRoute(name, faceID, cost)
}

// AddDirectRoute installs a direct prefix route.
// At main@51774b8 there is no PET; routes are installed only in the FIB.
func (fwd *SimForwarder) AddDirectRoute(name enc.Name, faceID uint64, cost uint64) {
	fwd.fib.InsertNextHopEnc(name, faceID, cost)
	fwd.AddRouteWithOrigin(name, faceID, cost, 0)
}

// AddRouteWithOrigin adds a route with a specific origin value.
func (fwd *SimForwarder) AddRouteWithOrigin(name enc.Name, faceID uint64, cost uint64, origin uint64) {
	fwd.AddRouteWithFlags(name, faceID, cost, origin, uint64(spec_mgmt.RouteFlagChildInherit))
}

// AddRouteWithFlags adds a route with explicit flags.
func (fwd *SimForwarder) AddRouteWithFlags(name enc.Name, faceID uint64, cost uint64, origin uint64, flags uint64) {
	fwd.withNodeFib(func() {
		fwd.rib.AddEncRoute(name, &table.Route{
			FaceID: faceID,
			Cost:   cost,
			Origin: origin,
			Flags:  flags,
		})
	})
}

// SetStrategy sets the forwarding strategy for a prefix.
func (fwd *SimForwarder) SetStrategy(prefix enc.Name, strategy enc.Name) {
	fwd.fib.SetStrategyEnc(prefix, strategy)
}

// RemoveRoute removes a route through this node's RIB so that readvertise fires.
func (fwd *SimForwarder) RemoveRoute(name enc.Name, faceID uint64) {
	fwd.RemoveDirectRoute(name, faceID)
}

// RemoveDirectRoute removes a direct prefix route.
func (fwd *SimForwarder) RemoveDirectRoute(name enc.Name, faceID uint64) {
	fwd.fib.RemoveNextHopEnc(name, faceID)
	fwd.RemoveRouteWithOrigin(name, faceID, 0)
}

// RemoveRouteWithOrigin removes a route with a specific origin value.
func (fwd *SimForwarder) RemoveRouteWithOrigin(name enc.Name, faceID uint64, origin uint64) {
	fwd.withNodeFib(func() {
		fwd.rib.RemoveRouteEnc(name, faceID, origin)
	})
}

// --- Packet processing ---

// ReceivePacket is the main entry point for packets arriving from ns-3.
func (fwd *SimForwarder) ReceivePacket(faceID uint64, frame []byte) {
	// Bind per-node hooks so that goroutine-local simFib returns this
	// node's instance.  SwapNode/RestoreNode correctly restores any prior
	// binding held by the caller rather than unconditionally unbinding.
	if fwd.hooks != nil {
		prev := _ndndsim.SwapNode(fwd.hooks)
		defer _ndndsim.RestoreNode(prev)
	}
	face := fwd.GetFace(faceID)
	if face == nil || face.State() != defn.Up {
		return
	}

	wire := enc.Wire{frame}
	parsed, err := defn.ParseFwPacket(enc.NewWireView(wire), false)
	if err != nil {
		return
	}

	pkt := &defn.Pkt{
		IncomingFaceID: faceID,
	}

	if parsed.LpPacket != nil {
		lp := parsed.LpPacket
		pkt.PitToken = lp.PitToken
		pkt.CongestionMark = lp.CongestionMark
		pkt.NextHopFaceID = lp.NextHopFaceId

		fragment := lp.Fragment
		if len(fragment) == 0 {
			return
		}
		inner, err := defn.ParseFwPacket(enc.NewWireView(fragment), false)
		if err != nil {
			return
		}
		pkt.Raw = fragment
		pkt.L3 = inner
	} else {
		pkt.Raw = wire
		pkt.L3 = parsed
	}

	if pkt.L3 == nil || (pkt.L3.Interest == nil && pkt.L3.Data == nil) {
		return
	}

	if pkt.L3.Interest != nil {
		pkt.Name = pkt.L3.Interest.NameV
	} else if pkt.L3.Data != nil {
		pkt.Name = pkt.L3.Data.NameV
	}

	fwd.withNodeFib(func() {
		fwd.thread.ProcessPacket(pkt)
	})
}

// Thread returns the underlying fw.Thread (for testing/debug access).
func (fwd *SimForwarder) Thread() *fw.Thread {
	return fwd.thread
}
