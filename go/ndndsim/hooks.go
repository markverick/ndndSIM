// Package ndndsim provides per-node simulation hooks for NDNd.
//
// At build time the ndndSIM transformer rewrites ndnd source:
//
//   go f()              →  ndndsim.Go(func() { f() })
//   time.Now()          →  ndndsim.Now()
//   time.AfterFunc(d,f) →  ndndsim.AfterFunc(d, f)
//   time.Sleep(d)       →  ndndsim.Sleep(d)
//   table.FibStrategyTable → simFib()  (package-local, from overlay)
//   table.Pet              → simPet()  (package-local, from overlay)
//
// At runtime the sim binds a NodeHooks to each goroutine via BindNode.
// All transformed calls read the hooks for the current goroutine; unbound
// goroutines transparently fall back to the production defaults.
package ndndsim

import (
	"os"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"
)

// NodeHooks holds all per-node simulation overrides.
// Zero / nil fields fall back to production defaults.
type NodeHooks struct {
	// GoFunc dispatches asynchronous work.
	// Default: go f()
	GoFunc func(func())

	// AfterFunc schedules f after d and returns a cancel function.
	// Default: time.AfterFunc.
	AfterFunc func(time.Duration, func()) func()

	// Now returns the current time for this node.
	// Default: time.Now.
	Now func() time.Time

	// Fib is the per-node FIB (fw/table.FibStrategy, stored as interface{}
	// to avoid an import cycle).  nil → use global FibStrategyTable.
	Fib interface{}

	// Pet is the per-node Prefix Egress Table (*fw/table.PrefixEgressTable). Note: this is the forwarder's PET, not the PSD prefix state.
	// nil → use global Pet.
	Pet interface{}

	// MulticastFib is the per-node multicast FIB (fw/table.FibStrategy, stored
	// as interface{} to avoid an import cycle).  nil → use global MulticastStrategyTable.
	MulticastFib interface{}

	// Bift is the per-node BIER forwarding table (*fw/bier.BiftState).
	// nil → use the global Bift.
	Bift interface{}

	// BierIndex is the per-node BFR-ID. BierIndexSet distinguishes the valid
	// index 0 from the production fallback.
	BierIndex    int
	BierIndexSet bool

	// KeyChain is a pre-built keychain (ndn.KeyChain) that bypasses
	// the KeyChainUri file-based setup.  nil → use KeyChainUri.
	KeyChain interface{}

	// Store is a pre-built object store (ndn.Store) that bypasses the
	// default MemoryStore allocation.  nil → allocate new MemoryStore.
	Store interface{}

	// Synchronous makes nfdc commands execute inline instead of via the
	// asynchronous channel.  Set to true in simulation.
	Synchronous bool

	// EnableFaceEvents controls whether the face-event polling loop runs.
	// Set to false in simulation; the sim layer manages faces directly.
	EnableFaceEvents bool

	// RouterName is the per-node router name (enc.Name, stored as interface{}
	// to avoid an import cycle with the ndnd module). nil → no per-node router
	// name; the global CfgRouterName() falls back to core.C.Fw.RouterName.
	// When set, ReceivePacket strips self-addressed EgressRouter from incoming
	// packets so the pipeline delivers them locally instead of transit-routing.
	RouterName interface{}
}

// productionHooks is returned for goroutines that are not bound to a node.
// They preserve 100% of ndnd's original production behaviour.
var productionHooks = &NodeHooks{
	GoFunc: func(f func()) { go f() },
	AfterFunc: func(d time.Duration, f func()) func() {
		t := time.AfterFunc(d, f)
		return func() { t.Stop() }
	},
	Now:              time.Now,
	EnableFaceEvents: true,
}

var (
	hooksMu  sync.RWMutex
	hooksMap = make(map[int64]*NodeHooks)
)

// goroutineID returns the numeric ID of the current goroutine by parsing the
// runtime stack header.  Overhead is ~150 ns, acceptable for simulation.
func goroutineID() int64 {
	var buf [32]byte
	n := runtime.Stack(buf[:], false)
	s := strings.TrimPrefix(string(buf[:n]), "goroutine ")
	i := strings.IndexByte(s, ' ')
	if i < 0 {
		return 0
	}
	id, _ := strconv.ParseInt(s[:i], 10, 64)
	return id
}

// GetHooks returns the NodeHooks bound to the current goroutine.
// Returns production defaults if no binding exists.
func GetHooks() *NodeHooks {
	hooksMu.RLock()
	h := hooksMap[goroutineID()]
	hooksMu.RUnlock()
	if h == nil {
		return productionHooks
	}
	return h
}

// GoroutineID returns the numeric ID of the current goroutine.
// Exported so that packages doing goroutine-local lock tracking do not need
// to duplicate the runtime-stack–parsing logic.
func GoroutineID() int64 {
	return goroutineID()
}

// BindNode associates h with the current goroutine.
// All ndndsim shim calls from this goroutine will use h.
func BindNode(h *NodeHooks) {
	hooksMu.Lock()
	hooksMap[goroutineID()] = h
	hooksMu.Unlock()
}

// UnbindNode removes the binding for the current goroutine.
func UnbindNode() {
	hooksMu.Lock()
	delete(hooksMap, goroutineID())
	hooksMu.Unlock()
}

// SwapNode atomically replaces the current goroutine's binding with h and
// returns the previous binding (productionHooks if the goroutine was not bound).
// Always pair with a deferred RestoreNode to safely restore the prior state.
func SwapNode(h *NodeHooks) *NodeHooks {
	id := goroutineID()
	hooksMu.Lock()
	prev := hooksMap[id]
	hooksMap[id] = h
	hooksMu.Unlock()
	if prev == nil {
		return productionHooks
	}
	return prev
}

// RestoreNode restores a binding previously saved by SwapNode.
// If prev is productionHooks (goroutine was unbound), the goroutine is unbound again.
func RestoreNode(prev *NodeHooks) {
	if prev == productionHooks {
		UnbindNode()
	} else {
		BindNode(prev)
	}
}

// --- Shim functions called by transformer-generated ndnd code ---

// CancelHandle is returned by AfterFunc. It mirrors *time.Timer so that
// transformer-generated code calling t.Stop() continues to compile and work.
type CancelHandle struct {
	cancel func()
}

// Stop cancels the scheduled function, equivalent to (*time.Timer).Stop.
// Returns true (the cancel-once semantics of time.Timer are not enforced).
func (c CancelHandle) Stop() bool {
	if c.cancel != nil {
		c.cancel()
	}
	return true
}

// Go dispatches f asynchronously, propagating the current node's hooks.
//
// In simulation (GoFunc = clock.Schedule) short-lived work runs as a
// 0-delay clock event — no goroutine is spawned and execution stays
// deterministic.  Outside simulation GoFunc defaults to "go f()" so the
// production code path is unchanged.
//
// Use GoLong() for long-lived blocking loops (SvSync.main, SvsALO.run,
// nfdc.Start) that must always run in a real goroutine because they never
// return until explicitly stopped.
func Go(f func()) {
	h := GetHooks()
	if h.GoFunc != nil {
		// GoFunc already captures hooks at the call site (see dv.go makeGoFunc
		// and node.go GoFunc).  Pass f directly; hook binding is GoFunc's job.
		h.GoFunc(f)
		return
	}
	// Fallback (should not happen — productionHooks always sets GoFunc).
	// In synchronous mode this means BindNode was not active when Go() was
	// called, which is a sim correctness bug (would spawn a real goroutine).
	if h.Synchronous {
		panic("ndndsim: Go() reached fallback in synchronous (DES) mode — " +
			"BindNode was not active; check all CGo/clock-callback entry points")
	}
	go func() {
		BindNode(h)
		defer UnbindNode()
		f()
	}()
}

// Sleep pauses for d.  In simulation (IsSynchronous) it is a no-op:
// real-wall sleeps are meaningless in a deterministic sim clock and would
// stall Advance() if called from a clock-event goroutine.  Outside
// simulation it calls time.Sleep(d) as normal.
func Sleep(d time.Duration) {
	if IsSynchronous() {
		return
	}
	time.Sleep(d)
}

// GoLong spawns f as a real goroutine regardless of the GoFunc hook.
// Use this for long-lived blocking loops (SvSync.main, SvsALO.run, nfdc.Start)
// that must not be scheduled as clock events because they never return until
// explicitly stopped via a channel signal.
//
// In synchronous (DES) mode GoLong panics: any call indicates a missed sim
// guard and would introduce a real goroutine that races with clock.Advance,
// breaking determinism. This enforces zero-tolerance for goroutines in DES.
func GoLong(f func()) {
	h := GetHooks()
	if h.Synchronous {
		panic("ndndsim: GoLong called in synchronous (DES) mode — " +
			"all long-lived loops must be guarded by IsSynchronous() checks")
	}
	go func() {
		BindNode(h)
		defer UnbindNode()
		f()
	}()
}

// AfterFunc schedules f to run after duration d, propagating the current
// node's hooks into the callback.  Returns a CancelHandle whose Stop method
// cancels the scheduled call — this mirrors the *time.Timer API so that
// transformer-generated code calling t.Stop() continues to compile.
func AfterFunc(d time.Duration, f func()) CancelHandle {
	h := GetHooks()
	cancel := h.AfterFunc(d, func() {
		BindNode(h)
		defer UnbindNode()
		f()
	})
	return CancelHandle{cancel: cancel}
}

// Now returns the current time for the node bound to this goroutine.
func Now() time.Time {
	return GetHooks().Now()
}

// EnableFaceEvents reports whether the face-event polling loop should run.
func EnableFaceEvents() bool {
	return GetHooks().EnableFaceEvents
}

// IsSynchronous reports whether nfdc should execute commands synchronously.
func IsSynchronous() bool {
	return GetHooks().Synchronous
}

// SvsMaxPipelineSize returns the SVS-ALO per-publisher fetch pipeline size.
//
// Empirically the production default of 10 throttles convergence on large
// topologies / high prefix counts.  An unlimited pipeline causes Interest
// bursts that overflow link queues (queue_size=100) when many objects must be
// fetched from a single publisher after a large burst of prefix announcements,
// causing retry exhaustion and permanent data loss.  20 is a balance: 2× faster
// than default, well under typical queue capacity.
// In production the default (0 → 10) applies.
//
// Override at runtime with the NDNDSIM_SVS_PIPELINE env var (simulation only).
func SvsMaxPipelineSize() uint64 {
	if GetHooks().Synchronous {
		if s := os.Getenv("NDNDSIM_SVS_PIPELINE"); s != "" {
			if v, err := strconv.ParseUint(s, 10, 64); err == nil {
				return v
			}
		}
		return 20
	}
	return 0
}

// pfxSvsDeliveryCallback is invoked whenever a prefix SVS publication is
// delivered to any node's subscription callback.  Used by the sim to record
// in-flight delivery timestamps for convergence detection.
var pfxSvsDeliveryCallback func()

// SetPfxSvsDeliveryCallback registers a function to be called whenever a
// prefix SVS publication is delivered to a subscription callback.
func SetPfxSvsDeliveryCallback(fn func()) {
	pfxSvsDeliveryCallback = fn
}

// NdndsimRecordPfxSvsDelivery invokes the registered prefix SVS delivery
// callback, if any.  Called by transformed dv/dv/prefix.go when a remote
// prefix publication is delivered via SVS.
func NdndsimRecordPfxSvsDelivery() {
	if pfxSvsDeliveryCallback != nil {
		pfxSvsDeliveryCallback()
	}
}

// NdndsimDebugPfxSvsCallbackRegistered returns whether the callback is registered.
func NdndsimDebugPfxSvsCallbackRegistered() bool {
	return pfxSvsDeliveryCallback != nil
}

// dvAdvReceiptCallback is invoked whenever a DV advertisement is received
// from a neighbor (in-flight arrival at a transit node).  Used by the sim to
// record in-flight delivery timestamps for DV convergence detection.
var dvAdvReceiptCallback func()

// SetDvAdvReceiptCallback registers a function to be called whenever a DV
// advertisement is received from a neighbor.
func SetDvAdvReceiptCallback(fn func()) {
	dvAdvReceiptCallback = fn
}

// NdndsimRecordDvAdvReceipt invokes the registered DV advertisement receipt
// callback, if any.  Called by transformed dv/dv/advert_data.go when a
// DV advertisement is received from a neighbor.
func NdndsimRecordDvAdvReceipt() {
	if dvAdvReceiptCallback != nil {
		dvAdvReceiptCallback()
	}
}
