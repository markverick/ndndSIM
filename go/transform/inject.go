// inject.go: code-injection rules that append new declarations to upstream files.
//
// Each applyXxxExtensions function calls injectDecls with a named snippet
// constant defined below.  Keeping the snippets as named constants makes them
// easy to locate and edit without navigating past function signatures.
package main

import (
	"go/ast"
	"go/token"

	"golang.org/x/tools/go/ast/astutil"
)

// ---------------------------------------------------------------------------
// Code snippets (valid Go source minus package declaration)
// ---------------------------------------------------------------------------

// fibStrategyTreeSnippet is injected into fw/table/fib-strategy-tree.go.
const fibStrategyTreeSnippet = `
// NewFibStrategyTree creates a standalone FibStrategyTree for per-node simulation use.
func NewFibStrategyTree() *FibStrategyTree {
	return newStrategyTableTree(defn.DEFAULT_STRATEGY)
}

// NewMulticastStrategyTree creates a standalone FibStrategyTree with BROADCAST
// strategy, matching the global MulticastStrategyTable.
func NewMulticastStrategyTree() *FibStrategyTree {
	return newStrategyTableTree(defn.BROADCAST_STRATEGY)
}

// CleanUpFace removes all nexthop entries for faceID from the FibStrategyTree.
func (f *FibStrategyTree) CleanUpFace(faceID uint64) {
	f.mutex.Lock()
	defer f.mutex.Unlock()

	var affected []*fibStrategyTreeEntry
	f.walk(func(entry *fibStrategyTreeEntry) {
		for _, nh := range entry.nexthops {
			if nh.Nexthop == faceID {
				affected = append(affected, entry)
				return
			}
		}
	})
	for _, entry := range affected {
		for i, nh := range entry.nexthops {
			if nh.Nexthop == faceID {
				entry.nexthops = append(entry.nexthops[:i], entry.nexthops[i+1:]...)
				break
			}
		}
		entry.pruneIfEmpty()
	}
}
`

// fibHashTableSnippet is injected into fw/table/fib-strategy-hashtable.go.
const fibHashTableSnippet = `
// CleanUpFace removes all nexthop entries for faceID from the FibStrategyHashTable.
func (f *FibStrategyHashTable) CleanUpFace(faceID uint64) {
	f.fibStrategyRWMutex.Lock()
	defer f.fibStrategyRWMutex.Unlock()

	for _, entry := range f.realTable {
		for i, nh := range entry.nexthops {
			if nh.Nexthop == faceID {
				entry.nexthops = append(entry.nexthops[:i], entry.nexthops[i+1:]...)
				f.pruneTables(entry)
				break
			}
		}
	}
}
`

// petConstructorSnippet is injected into fw/table/pet.go.
const petConstructorSnippet = `
// NewPrefixEgressTable creates a properly initialized PrefixEgressTable.
// Called by the rulePetGlobalPointer-transformed var Pet initializer and
// by sim/forwarder.go for per-node PET instances.
func NewPrefixEgressTable() *PrefixEgressTable {
	return &PrefixEgressTable{
		root: petNode{
			children: make(map[uint64]*petNode),
		},
	}
}
`

// ribSimSnippet is injected into fw/table/rib.go.
// Caller must set ndndsimUsed=true so that _ndndsim is imported.
const ribSimSnippet = `
// simFib returns the per-node FIB for the current goroutine.
// Called by transformer-injected code in this file that replaces bare
// FibStrategyTable references (same-package usage, no "table." prefix).
func simFib() FibStrategy {
	h := _ndndsim.GetHooks()
	if h.Fib == nil {
		return FibStrategyTable
	}
	return h.Fib.(FibStrategy)
}

// InitRoot initialises the RIB root entry for a fresh per-node RibTable.
// Called by sim/forwarder.go after allocating a new &RibTable{}.
func (r *RibTable) InitRoot() {
	r.root.children = make(map[uint64]*RibEntry)
}
`

// svsSimSnippet is injected into std/sync/svs.go.
// All required imports (sync/atomic, time, _ndndsim) are already present
// after the ruleSimTicker and global time.Now rewrites.
const svsSimSnippet = `
// SuppressStats holds counters for SVS Interest suppression decisions.
type SuppressStats struct {
	// Enter counts how many sync Interests entered the suppression window.
	Enter uint64
	// Ok counts how many sync Interests were sent (not suppressed).
	Ok uint64
	// Fail counts how many sync Interests were suppressed.
	Fail uint64
}

// SuppressionStats returns the current suppression statistics.
// In this baseline implementation the counters are always zero.
func (s *SvSync) SuppressionStats() SuppressStats {
	return SuppressStats{}
}

// simTicker implements the same channel-based interface as *time.Ticker but
// schedules ticks via the per-goroutine AfterFunc hook so they follow the
// simulation clock.
type simTicker struct {
	C         chan time.Time
	period    time.Duration
	afterFunc func(time.Duration, func()) func()
	cancel    func()
	stopped   atomic.Bool
}

// newSimTicker creates a simTicker that fires every d using the AfterFunc hook
// bound to the current goroutine (sim clock) or time.AfterFunc as a production
// fallback.  Called by transformer-generated code that replaces time.NewTicker.
func newSimTicker(d time.Duration) *simTicker {
	h := _ndndsim.GetHooks()
	af := h.AfterFunc
	if af == nil {
		af = func(d time.Duration, f func()) func() {
			t := time.AfterFunc(d, f)
			return func() { t.Stop() }
		}
	}
	t := &simTicker{
		C:         make(chan time.Time, 1),
		period:    d,
		afterFunc: af,
	}
	t.schedule(d)
	return t
}

func (t *simTicker) schedule(d time.Duration) {
	t.cancel = t.afterFunc(d, func() {
		if t.stopped.Load() {
			return
		}
		select {
		case t.C <- time.Time{}:
		default:
		}
		t.schedule(t.period)
	})
}

// Reset restarts the ticker with a new period.
func (t *simTicker) Reset(d time.Duration) {
	t.stopped.Store(false)
	if t.cancel != nil {
		t.cancel()
	}
	t.period = d
	t.schedule(d)
}

// Stop cancels the ticker.
func (t *simTicker) Stop() {
	t.stopped.Store(true)
	if t.cancel != nil {
		t.cancel()
		t.cancel = nil
	}
}
`

// threadSimSnippet is injected into fw/fw/thread.go.
// sync is added via astutil.AddImport; _ndndsim, defn, and table are already
// imported after the global rewrites.
const threadSimSnippet = `
// simFib returns the per-node FIB for the goroutine currently executing.
// Called by transformer-generated code that replaces table.FibStrategyTable.
func simFib() table.FibStrategy {
	h := _ndndsim.GetHooks()
	if h.Fib == nil {
		return table.FibStrategyTable
	}
	return h.Fib.(table.FibStrategy)
}

// simPet returns the per-node PET for the goroutine currently executing.
// Called by transformer-generated code that replaces table.Pet / &table.Pet.
func simPet() *table.PrefixEgressTable {
	h := _ndndsim.GetHooks()
	if h.Pet == nil {
		return table.Pet
	}
	return h.Pet.(*table.PrefixEgressTable)
}

// simMulticastFib returns the per-node multicast FIB for the current goroutine.
// Called by transformer-generated code that replaces table.MulticastStrategyTable.
func simMulticastFib() table.FibStrategy {
	h := _ndndsim.GetHooks()
	if h.MulticastFib == nil {
		return table.MulticastStrategyTable
	}
	return h.MulticastFib.(table.FibStrategy)
}

// per-node FIB/PET side-tables keyed by *Thread to avoid modifying Thread struct.
var (
	simFibMu  sync.RWMutex
	simFibMap = map[*Thread]table.FibStrategy{}
	simPetMu  sync.RWMutex
	simPetMap = map[*Thread]*table.PrefixEgressTable{}
)

// SetFib records a per-node FIB for simulation; stored in a side-table.
func (t *Thread) SetFib(fib table.FibStrategy) {
	simFibMu.Lock()
	simFibMap[t] = fib
	simFibMu.Unlock()
}

// SetPet records a per-node PET for simulation; stored in a side-table.
func (t *Thread) SetPet(pet *table.PrefixEgressTable) {
	simPetMu.Lock()
	simPetMap[t] = pet
	simPetMu.Unlock()
}

// Fib returns the per-node FIB if set, otherwise falls back to the hook/global.
func (t *Thread) Fib() table.FibStrategy {
	simFibMu.RLock()
	fib := simFibMap[t]
	simFibMu.RUnlock()
	if fib != nil {
		return fib
	}
	return simFib()
}

// Pet returns the per-node PET if set, otherwise falls back to the hook/global.
func (t *Thread) Pet() *table.PrefixEgressTable {
	simPetMu.RLock()
	pet := simPetMap[t]
	simPetMu.RUnlock()
	if pet != nil {
		return pet
	}
	return simPet()
}

// ProcessPacket synchronously processes a single packet through the forwarding
// pipeline.  Used by the simulation engine instead of the channel-based Run() loop.
func (t *Thread) ProcessPacket(pkt *defn.Pkt) {
	if pkt.L3.Interest != nil {
		t.processIncomingInterest(pkt)
	} else if pkt.L3.Data != nil {
		t.processIncomingData(pkt)
	}
}

// RunMaintenance performs one maintenance cycle (dead nonce expiry, PIT/CS update).
// Called periodically by the simulation clock instead of the tickers in Run().
func (t *Thread) RunMaintenance() {
	t.deadNonceList.RemoveExpiredEntries()
	t.pitCS.Update()
}
`

// nfdcSimSnippet is injected into dv/nfdc/nfdc.go.
// Caller must set ndndsimUsed=true so that _ndndsim is imported.
const nfdcSimSnippet = `
// simExec is called by transformer-generated code instead of the raw channel
// send "m.channel <- cmd".  In synchronous mode (simulation) the command is
// executed inline; otherwise it is forwarded to the real async channel.
func (m *NfdMgmtThread) simExec(cmd NfdMgmtCmd) {
	if _ndndsim.IsSynchronous() {
		for i := 0; i < cmd.Retries || cmd.Retries < 0; i++ {
			_, err := m.engine.ExecMgmtCmd(cmd.Module, cmd.Cmd, cmd.Args)
			if err != nil {
				log.Error(m, "Forwarder command failed (sync)", "err", err,
					"module", cmd.Module, "cmd", cmd.Cmd)
			} else {
				break
			}
		}
		return
	}
	m.channel <- cmd
}
`

// prefixSimSnippet is injected into dv/dv/prefix.go.
// ndn_sync is already imported in prefix.go.
const prefixSimSnippet = `
// SuppressionStats returns the SVS suppression statistics for the prefix sync.
func (pfx *PrefixModule) SuppressionStats() ndn_sync.SuppressStats {
	svs := pfx.pfxSvs.SVS()
	if svs == nil {
		return ndn_sync.SuppressStats{}
	}
	return svs.SuppressionStats()
}
`

// routerSimSnippet is injected into dv/dv/router.go.
// _ndndsim is already imported after the global go→_ndndsim.Go rewrite.
// ndn_sync is added via addNamedImport in applyRouterSimExtensions.
const routerSimSnippet = `
// simNewKeyChain is called by transformer-generated code replacing
// keychain.NewKeyChain(uri, store).  Returns a pre-built keychain from hooks
// if available (simulation), otherwise opens the real file-backed keychain.
func simNewKeyChain(uri string, store ndn.Store) (ndn.KeyChain, error) {
	h := _ndndsim.GetHooks()
	if h.KeyChain != nil {
		return h.KeyChain.(ndn.KeyChain), nil
	}
	return keychain.NewKeyChain(uri, store)
}

// Init initialises the DV router without blocking.
// Simulation-compatible alternative to Start(): performs all setup but returns
// immediately; the caller drives heartbeat and deadcheck via the sim clock.
func (dv *Router) Init() error {
	log.Info(dv, "Initializing DV router (sim)", "version", utils.NDNdVersion)

	// Reset advert.bootTime with the simulation clock now that hooks are set up.
	// NewRouter uses time.Now() (wall clock); we need the sim clock here.
	// Use max(x, 1) to avoid bootTime=0 at simulation epoch.
	dv.advert = advertModule{
		dv:       dv,
		bootTime: max(uint64(_ndndsim.Now().Unix()), 1),
		seq:      0,
		objDir:   storage.NewMemoryFifoDir(32),
	}

	// Start object client.
	dv.client.Start()

	// Register interest handlers (no configureFace — sim faces are pre-wired).
	if err := dv.register(); err != nil {
		return err
	}

	// Start prefix daemon.
	dv.pfx.Start()

	// Add self to the RIB and generate the initial advertisement.
	dv.rib.Set(dv.config.RouterName(), dv.config.RouterName(), 0)
	dv.advert.generate()

	// Initialise prefix egress state.
	dv.pfx.Reset()

	// Start the NFD management thread in a real goroutine (long-lived loop).
	_ndndsim.Go(func() { dv.nfdc.Start() })

	return nil
}

// RunHeartbeat sends a sync Interest to all neighbours (simulation tick).
func (dv *Router) RunHeartbeat() {
	dv.advert.sendSyncInterest()
}

// RunDeadcheck checks for dead neighbours and prunes routes (simulation tick).
func (dv *Router) RunDeadcheck() {
	dv.checkDeadNeighbors()
}

// Cleanup tears down the DV router (simulation shutdown).
func (dv *Router) Cleanup() {
	if dv.pfx != nil {
		dv.pfx.Stop()
	}
	dv.client.Stop()
	log.Info(dv, "Cleaned up DV router (sim)")
}

// Nfdc returns the NFD management thread.
func (dv *Router) Nfdc() *nfdc.NfdMgmtThread {
	return dv.nfdc
}

// PrefixSyncSuppressionStats returns SVS suppression statistics.
func (dv *Router) PrefixSyncSuppressionStats() ndn_sync.SuppressStats {
	if dv.pfx == nil {
		return ndn_sync.SuppressStats{}
	}
	return dv.pfx.SuppressionStats()
}
`

// ---------------------------------------------------------------------------
// Injection functions
// ---------------------------------------------------------------------------

func applyFibStrategyTreeExtensions(file *ast.File, fset *token.FileSet) bool {
	injectDecls(file, fset, fibStrategyTreeSnippet)
	return true
}

func applyFibHashTableExtensions(file *ast.File, fset *token.FileSet) bool {
	injectDecls(file, fset, fibHashTableSnippet)
	return true
}

func applyPetConstructor(file *ast.File, fset *token.FileSet) bool {
	injectDecls(file, fset, petConstructorSnippet)
	return true
}

// applyRibSimExtensions injects simFib() + InitRoot() into fw/table/rib.go.
// Caller must set ndndsimUsed=true so that _ndndsim is imported.
func applyRibSimExtensions(file *ast.File, fset *token.FileSet) bool {
	injectDecls(file, fset, ribSimSnippet)
	return true
}

func applySvsSimExtensions(file *ast.File, fset *token.FileSet) bool {
	injectDecls(file, fset, svsSimSnippet)
	return true
}

// applyThreadSimExtensions injects simFib/simPet/simMulticastFib + Thread
// sim methods into fw/fw/thread.go and adds the "sync" import.
func applyThreadSimExtensions(file *ast.File, fset *token.FileSet) bool {
	injectDecls(file, fset, threadSimSnippet)
	// thread.go has sync/atomic but not sync itself.
	astutil.AddImport(fset, file, "sync")
	return true
}

// applyNfdcSimExtensions injects simExec into dv/nfdc/nfdc.go.
// Caller must set ndndsimUsed=true so that _ndndsim is imported.
func applyNfdcSimExtensions(file *ast.File, fset *token.FileSet) bool {
	injectDecls(file, fset, nfdcSimSnippet)
	return true
}

func applyPrefixSimExtensions(file *ast.File, fset *token.FileSet) bool {
	injectDecls(file, fset, prefixSimSnippet)
	return true
}

// applyRouterSimExtensions injects simNewKeyChain + Router sim methods into
// dv/dv/router.go and adds the ndn_sync import.
func applyRouterSimExtensions(file *ast.File, fset *token.FileSet) bool {
	injectDecls(file, fset, routerSimSnippet)
	// ndn_sync is not in the upstream router.go; needed for injected methods.
	addNamedImport(file, "ndn_sync", "github.com/named-data/ndnd/std/sync")
	return true
}

// ---------------------------------------------------------------------------
// Onephase snippets (named-data/ndnd@main@51774b8)
// ---------------------------------------------------------------------------

// fibStrategyTreeSnippetOp is injected into fw/table/fib-strategy-tree.go for
// the onephase build.  At 51774b8 there is no newStrategyTableTree() helper,
// so we construct the FibStrategyTree struct directly.  Children are a slice
// (not a map) and there is no walk() helper, so CleanUpFace uses a closure.
const fibStrategyTreeSnippetOp = `
// NewFibStrategyTree creates a standalone FibStrategyTree for per-node simulation use.
func NewFibStrategyTree() *FibStrategyTree {
	t := new(FibStrategyTree)
	t.root = new(fibStrategyTreeEntry)
	t.root.component = enc.Component{}
	t.root.strategy = defn.DEFAULT_STRATEGY
	t.root.name = enc.Name{}
	return t
}

// NewMulticastStrategyTree creates a standalone FibStrategyTree for per-node
// simulation use.  At main@51774b8 there is no separate multicast strategy;
// we return a tree with the default strategy as a best-effort substitute.
func NewMulticastStrategyTree() *FibStrategyTree {
	t := new(FibStrategyTree)
	t.root = new(fibStrategyTreeEntry)
	t.root.component = enc.Component{}
	t.root.strategy = defn.DEFAULT_STRATEGY
	t.root.name = enc.Name{}
	return t
}

// CleanUpFace removes all nexthop entries for faceID from the FibStrategyTree.
func (f *FibStrategyTree) CleanUpFace(faceID uint64) {
	f.mutex.Lock()
	defer f.mutex.Unlock()
	var clean func(e *fibStrategyTreeEntry)
	clean = func(e *fibStrategyTreeEntry) {
		if e == nil {
			return
		}
		for i, nh := range e.nexthops {
			if nh.Nexthop == faceID {
				e.nexthops = append(e.nexthops[:i], e.nexthops[i+1:]...)
				break
			}
		}
		for _, child := range e.children {
			clean(child)
		}
	}
	clean(f.root)
}
`

// threadSimSnippetOp is injected into fw/fw/thread.go for the onephase build.
// At main@51774b8 there is no PrefixEgressTable (pet.go doesn't exist) and no
// MulticastStrategyTable global, so those are omitted.
const threadSimSnippetOp = `
// simFib returns the per-node FIB for the goroutine currently executing.
// Called by transformer-generated code that replaces table.FibStrategyTable.
func simFib() table.FibStrategy {
	h := _ndndsim.GetHooks()
	if h.Fib == nil {
		return table.FibStrategyTable
	}
	return h.Fib.(table.FibStrategy)
}

// per-node FIB side-table keyed by *Thread to avoid modifying Thread struct.
var (
	simFibMu  sync.RWMutex
	simFibMap = map[*Thread]table.FibStrategy{}
)

// SetFib records a per-node FIB for simulation; stored in a side-table.
func (t *Thread) SetFib(fib table.FibStrategy) {
	simFibMu.Lock()
	simFibMap[t] = fib
	simFibMu.Unlock()
}

// Fib returns the per-node FIB if set, otherwise falls back to the hook/global.
func (t *Thread) Fib() table.FibStrategy {
	simFibMu.RLock()
	fib := simFibMap[t]
	simFibMu.RUnlock()
	if fib != nil {
		return fib
	}
	return simFib()
}

// ProcessPacket synchronously processes a single packet through the forwarding
// pipeline.  Used by the simulation engine instead of the channel-based Run() loop.
func (t *Thread) ProcessPacket(pkt *defn.Pkt) {
	if pkt.L3.Interest != nil {
		t.processIncomingInterest(pkt)
	} else if pkt.L3.Data != nil {
		t.processIncomingData(pkt)
	}
}

// RunMaintenance performs one maintenance cycle (dead nonce expiry, PIT/CS update).
// Called periodically by the simulation clock instead of the tickers in Run().
func (t *Thread) RunMaintenance() {
	t.deadNonceList.RemoveExpiredEntries()
	t.pitCS.Update()
}
`

// routerSimSnippetOp is injected into dv/dv/router.go for the onephase build.
// At main@51774b8, dv.pfx is *table.PrefixTable (no Start/Stop) and the SVS
// is managed directly via dv.pfxSvs (*ndn_sync.SvsALO).
// ndn_sync is already imported by the pristine router.go at 51774b8.
const routerSimSnippetOp = `
// simNewKeyChain is called by transformer-generated code replacing
// keychain.NewKeyChain(uri, store).  Returns a pre-built keychain from hooks
// if available (simulation), otherwise opens the real file-backed keychain.
func simNewKeyChain(uri string, store ndn.Store) (ndn.KeyChain, error) {
	h := _ndndsim.GetHooks()
	if h.KeyChain != nil {
		return h.KeyChain.(ndn.KeyChain), nil
	}
	return keychain.NewKeyChain(uri, store)
}

// Init initialises the DV router without blocking.
// Simulation-compatible alternative to Start(): performs all setup but returns
// immediately; the caller drives heartbeat and deadcheck via the sim clock.
func (dv *Router) Init() error {
	log.Info(dv, "Initializing DV router (sim)", "version", utils.NDNdVersion)

	// Reset advert.bootTime with the simulation clock now that hooks are set up.
	dv.advert = advertModule{
		dv:       dv,
		bootTime: max(uint64(_ndndsim.Now().Unix()), 1),
		seq:      0,
		objDir:   storage.NewMemoryFifoDir(32),
	}

	// Start object client.
	dv.client.Start()

	// Register interest handlers (no configureFace — sim faces are pre-wired).
	if err := dv.register(); err != nil {
		return err
	}

	// Start prefix table sync (SVS-based in main@51774b8).
	dv.pfxSvs.Start()

	// Add self to the RIB and generate the initial advertisement.
	dv.rib.Set(dv.config.RouterName(), dv.config.RouterName(), 0)
	dv.advert.generate()

	// Reset prefix table.
	dv.pfx.Reset()

	// Start the NFD management thread in a real goroutine (long-lived loop).
	_ndndsim.Go(func() { dv.nfdc.Start() })

	return nil
}

// RunHeartbeat sends a sync Interest to all neighbours (simulation tick).
func (dv *Router) RunHeartbeat() {
	dv.advert.sendSyncInterest()
}

// RunDeadcheck checks for dead neighbours and prunes routes (simulation tick).
func (dv *Router) RunDeadcheck() {
	dv.checkDeadNeighbors()
}

// Cleanup tears down the DV router (simulation shutdown).
func (dv *Router) Cleanup() {
	dv.pfxSvs.Stop()
	dv.client.Stop()
	log.Info(dv, "Cleaned up DV router (sim)")
}

// Nfdc returns the NFD management thread.
func (dv *Router) Nfdc() *nfdc.NfdMgmtThread {
	return dv.nfdc
}

// PrefixSyncSuppressionStats returns SVS suppression statistics.
// At main@51774b8 PrefixTable has no SuppressionStats; return empty.
func (dv *Router) PrefixSyncSuppressionStats() ndn_sync.SuppressStats {
	return ndn_sync.SuppressStats{}
}
`

// ---------------------------------------------------------------------------
// Onephase injection functions
// ---------------------------------------------------------------------------

func applyFibStrategyTreeExtensionsOp(file *ast.File, fset *token.FileSet) bool {
	injectDecls(file, fset, fibStrategyTreeSnippetOp)
	return true
}

// applyThreadSimExtensionsOp injects onephase sim methods into fw/fw/thread.go.
// No PET or multicast table (they don't exist at main@51774b8).
func applyThreadSimExtensionsOp(file *ast.File, fset *token.FileSet) bool {
	injectDecls(file, fset, threadSimSnippetOp)
	// thread.go has sync/atomic but not sync itself.
	astutil.AddImport(fset, file, "sync")
	return true
}

// applyRouterSimExtensionsOp injects onephase Router sim methods into
// dv/dv/router.go.  ndn_sync is already imported at main@51774b8.
func applyRouterSimExtensionsOp(file *ast.File, fset *token.FileSet) bool {
	injectDecls(file, fset, routerSimSnippetOp)
	// ndn_sync already imported at 51774b8; addNamedImport is idempotent.
	addNamedImport(file, "ndn_sync", "github.com/named-data/ndnd/std/sync")
	return true
}
