// Package ndndsim provides per-node simulation hooks for NDNd.
//
// At build time the ndndSIM transformer rewrites ndnd source:
//
//   go f()              →  ndndsim.Go(func() { f() })
//   time.Now()          →  ndndsim.Now()
//   time.AfterFunc(d,f) →  ndndsim.AfterFunc(d, f)
//   table.FibStrategyTable → simFib()  (package-local, from overlay)
//   table.Pet              → simPet()  (package-local, from overlay)
//
// At runtime the sim binds a NodeHooks to each goroutine via BindNode.
// All transformed calls read the hooks for the current goroutine; unbound
// goroutines transparently fall back to the production defaults.
package ndndsim

import (
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

	// Pet is the per-node Prefix Egress Table (*fw/table.PrefixEgressTable).
	// nil → use global Pet.
	Pet interface{}

	// MulticastFib is the per-node multicast FIB (fw/table.FibStrategy, stored
	// as interface{} to avoid an import cycle).  nil → use global MulticastStrategyTable.
	MulticastFib interface{}

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

// Go launches f as a real goroutine, propagating the current node's hooks
// into that goroutine so that ndndsim.Now/AfterFunc/IsSynchronous work
// correctly inside f.
//
// Note: this always uses a real goroutine (not GoFunc / clock scheduling).
// GoFunc is reserved for the DV router's own single-step task dispatch
// (router.GoFunc = clock.Schedule).  Transformer-generated `go f()` calls
// need real goroutines so that long-lived loops (e.g. SvSync.main) don't
// block the simulation clock's Advance method.
func Go(f func()) {
	h := GetHooks()
	go func() {
		id := goroutineID()
		hooksMu.Lock()
		hooksMap[id] = h
		hooksMu.Unlock()
		defer func() {
			hooksMu.Lock()
			delete(hooksMap, id)
			hooksMu.Unlock()
		}()
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
		id := goroutineID()
		hooksMu.Lock()
		hooksMap[id] = h
		hooksMu.Unlock()
		defer func() {
			hooksMu.Lock()
			delete(hooksMap, id)
			hooksMu.Unlock()
		}()
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
