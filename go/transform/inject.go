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

// simTicker implements the same interface as *time.Ticker but schedules ticks
// via the per-goroutine AfterFunc hook so they follow the simulation clock.
// When callback is non-nil, ticks invoke it directly instead of sending to C,
// eliminating the need for a goroutine to read the channel.
type simTicker struct {
	C         chan time.Time
	period    time.Duration
	afterFunc func(time.Duration, func()) func()
	cancel    func()
	stopped   atomic.Bool
	callback  func() // direct tick handler in sim mode (no channel read needed)
	version   atomic.Uint64 // incremented by Reset/Stop; guards against double-reschedule
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
	v := t.version.Load()
	t.cancel = t.afterFunc(d, func() {
		if t.stopped.Load() {
			return
		}
		if t.callback != nil {
			// Sim mode: call handler directly.  Use version to detect whether
			// the callback called Reset() — if so, it already rescheduled and
			// we must NOT auto-reschedule here (would produce a duplicate tick).
			t.callback()
			if t.version.Load() == v {
				// Callback did not Reset/Stop — keep the periodic tick going.
				t.schedule(t.period)
			}
		} else {
			select {
			case t.C <- time.Time{}:
			default:
			}
			t.schedule(t.period)
		}
	})
}

// Reset restarts the ticker with a new period.
func (t *simTicker) Reset(d time.Duration) {
	t.version.Add(1) // signal that we rescheduled — suppress auto-reschedule in handler
	t.stopped.Store(false)
	if t.cancel != nil {
		t.cancel()
	}
	t.period = d
	t.schedule(d)
}

// Stop cancels the ticker.
func (t *simTicker) Stop() {
	t.version.Add(1) // signal a version change so any running handler skips auto-reschedule
	t.stopped.Store(true)
	if t.cancel != nil {
		t.cancel()
		t.cancel = nil
	}
}

// simStart replaces main() in simulation mode.
// Called by transformer-generated code: if _ndndsim.IsSynchronous() { s.simStart() }
// Sets up the periodic timer and initial sync interest without running a goroutine.
func (s *SvSync) simStart() {
	s.running.Store(true)

	// Register face-up callback (matches main() behaviour).
	s.faceCancel = s.o.Client.Engine().Face().OnUp(func() {
		_ndndsim.AfterFunc(100*time.Millisecond, s.sendSyncInterest)
	})

	// Send initial Sync Interest (matches main() behaviour).
	if s.o.Passive {
		_ndndsim.Go(func() { s.loadPassiveWires() })
	} else {
		_ndndsim.Go(func() { s.sendSyncInterest() })
	}

	// Register timerExpired as the direct tick callback so the simTicker
	// fires it via AfterFunc without needing a goroutine to read ticker.C.
	s.ticker.callback = s.timerExpired
}

// SimStartQuiet starts SVS handlers for a synthetic-ready simulation without
// sending the initial Sync Interest or scheduling periodic sync traffic.
func (s *SvSync) SimStartQuiet() (err error) {
	err = s.o.Client.Engine().AttachHandler(s.prefix,
		func(args ndn.InterestHandlerArgs) {
			s.onSyncInterest(args.Interest)
		})
	if err != nil {
		return err
	}
	s.running.Store(true)
	return nil
}

// simRecvSv processes an incoming state vector directly, bypassing the recvSv
// channel.  Called by transformer-generated code replacing s.recvSv <- sv.
func (s *SvSync) simRecvSv(sv svSyncRecvSvArgs) {
	s.onReceiveStateVector(sv)
}

// simStop tears down the SvSync instance in simulation mode.
// Called by transformer-generated code replacing s.stop <- struct{}{} in Stop().
func (s *SvSync) simStop() {
	s.o.Client.Engine().DetachHandler(s.prefix)
	s.running.Store(false)
	s.ticker.Stop()
	if s.faceCancel != nil {
		s.faceCancel()
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

// bierSimSnippet is injected into fw/bier/bier.go for per-node simulation
// state.  Caller must set ndndsimUsed=true so _ndndsim is imported.
const bierSimSnippet = `
// simFib returns the per-node FIB for BIER's BuildFromFib path.
func simFib() table.FibStrategy {
	h := _ndndsim.GetHooks()
	if h.Fib == nil {
		return table.FibStrategyTable
	}
	return h.Fib.(table.FibStrategy)
}

// SimBift returns the per-node BIFT for the current simulation node.
func SimBift() *BiftState {
	h := _ndndsim.GetHooks()
	if h.Bift == nil {
		return Bift
	}
	return h.Bift.(*BiftState)
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

// RestorePfxEntries populates the local PrefixEgreState with a previously
// exported set of prefix snapshot entries. Should be called after Init() and
// before pfxSvs.Start() so the prefix state is consistent from the start.
func (pfx *PrefixModule) RestorePfxEntries(entries []table.PrefixSnapshotEntry) {
	pfx.mu.Lock()
	defer pfx.mu.Unlock()
	for _, e := range entries {
		router := pfx.pfx.GetRouter(e.Router)
		router.Prefixes[e.Name.TlvStr()] = &table.PrefixEntry{
			Name:           e.Name.Clone(),
			Multicast:      e.Multicast,
			ValidityPeriod: e.ValidityPeriod,
			NextHops:       append([]table.PrefixNextHop(nil), e.NextHops...),
		}
	}
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

	// Add self to the RIB and generate the initial advertisement.
	dv.rib.Set(dv.config.RouterName(), dv.config.RouterName(), 0)
	dv.advert.generate()

	// Initialise prefix egress state (table only; SVS not yet started).
	dv.pfx.Reset()

	// nfdc.Start() is NOT launched in simulation: all management commands
	// are executed synchronously via simExec (ruleNfdcChannelSend transform).
	// Starting the goroutine would violate the zero-goroutine constraint.

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
	// nfdc.Stop() is intentionally omitted: nfdc.Start() was never called in
	// sim mode, so there is no goroutine to signal (and sending to the stop
	// channel would deadlock).
	log.Info(dv, "Cleaned up DV router (sim)")
}

// _pfxStarted tracks which Router instances have had pfxSvs.Start() called.
var _pfxStarted sync.Map // *Router → struct{}

// _pfxCancel stores the cancel function for the pending pfxSvs startup timer.
var _pfxCancel sync.Map // *Router → func()

// _pfxReachable tracks the last-known reachable-router count per Router.
var _pfxReachable sync.Map // *Router → int

// startPfxOnce implements a deferred startup of the prefix SVS daemon.
// It is called from runConvergenceHook() in notify.go on every RIB update,
// with the current number of reachable routers passed as reachableCount.
//
// PSD subscriptions are set up here for all known neighbors before pfxSvs.Start().
// We call the three sub-components of PrefixModule.Start() directly instead of
// Start() itself because Start() calls pfx.pfx.Reset() which wipes announcements
// queued before convergence.
func (dv *Router) startPfxOnce(reachableCount int) {
	// Fast path: pfxSvs already started.
	if _, ok := _pfxStarted.Load(dv); ok {
		return
	}
	if _, loaded := _pfxStarted.LoadOrStore(dv, struct{}{}); loaded {
		return
	}
	// Cancel any pending debounce timer.
	if prev, ok := _pfxCancel.Load(dv); ok {
		prev.(func())()
	}

	// Subscribe to PSD routers before starting pfxSvs.
	selfName := dv.config.RouterName()
	for _, ns := range dv.neighbors.GetAll() {
		router := ns.Name
		if router.Equal(selfName) {
			continue
		}
		routerHash := router.Hash()
		if _, ok := dv.pfx.pfxSubs[routerHash]; ok {
			continue
		}
		dv.pfx.pfxSeen[routerHash] = router.Clone()
		dv.pfx.pfxSubs[routerHash] = router.Clone()
		if dv.pfx.replicatePsd && dv.pfx.nfdc != nil {
			route := dv.pfx.pfxGroup.Append(router...)
			dv.pfx.nfdc.Exec(nfdc.NfdMgmtCmd{
				Module:  "pet",
				Cmd:     "add-egress",
				Args:    &mgmt.ControlArgs{Name: route, Egress: &mgmt.EgressRecord{Name: router.Clone()}},
				Retries: -1,
			})
		}
		dv.pfx.pfxSvs.SubscribePublisher(router, func(sp ndn_sync.SvsPub) {
			_ndndsim.NdndsimRecordPfxSvsDelivery()
			dv.pfx.mu.Lock()
			_, petOps := dv.pfx.processUpdate(sp.Content)
			dv.pfx.mu.Unlock()
			dv.pfx.applyPetOps(petOps)
		})
	}

	dv.pfx.pfxSvs.Start()
	dv.pfx.startFaceEvents()
	dv.pfx.startPrefixPrune()
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

// LinkMulticastPrefixes returns prefixes that must be forwarded to all link
// faces for DV sync to reach neighbors in the simulation harness.
func (dv *Router) LinkMulticastPrefixes() []enc.Name {
	return []enc.Name{
		dv.config.AdvertisementSyncPrefix(),
		dv.pfx.SyncPrefix(),
	}
}

// MgmtPrefix returns the management prefix for this DV router instance.
// This is phase-specific (/localhost/dv in twophase, /localhost/nlsr in onephase)
// and must be used when constructing management Interest names.
func (dv *Router) MgmtPrefix() enc.Name {
	return dv.config.MgmtPrefix()
}

// PrefixAnnounceCmd returns the command path components appended after MgmtPrefix
// to form a prefix-announce management Interest.
// Twophase: ["prefix", "announce"] → /localhost/dv/prefix/announce/<params>
func (dv *Router) PrefixAnnounceCmd() enc.Name {
	return enc.Name{
		enc.NewGenericComponent("prefix"),
		enc.NewGenericComponent("announce"),
	}
}

// PrefixWithdrawCmd returns the command path components appended after MgmtPrefix
// to form a prefix-withdraw management Interest.
// Twophase: ["prefix", "withdraw"] → /localhost/dv/prefix/withdraw/<params>
func (dv *Router) PrefixWithdrawCmd() enc.Name {
	return enc.Name{
		enc.NewGenericComponent("prefix"),
		enc.NewGenericComponent("withdraw"),
	}
}

// NumPendingFetchInterests returns the total number of in-flight data fetch
// Interests across all prefix SVS publishers.
func (dv *Router) NumPendingFetchInterests() uint64 {
	if dv.pfx == nil || dv.pfx.pfxSvs == nil {
		return 0
	}
	return dv.pfx.pfxSvs.NumPendingFetchInterests()
}

func simBfrIDFromRouterName(name enc.Name) (int, bool) {
	if len(name) == 0 {
		return 0, false
	}
	lastComp := name[len(name)-1].String()
	var id int
	// Try "node%d" format (grid topologies)
	if n, _ := fmt.Sscanf(lastComp, "node%d", &id); n == 1 {
		return id, true
	}
	// Try "rf%d" format (Rocketfuel topologies)
	if n, _ := fmt.Sscanf(lastComp, "rf%d", &id); n == 1 {
		return id, true
	}
	// Try generic trailing digits (any format ending with digits)
	for i := len(lastComp) - 1; i >= 0; i-- {
		if lastComp[i] < '0' || lastComp[i] > '9' {
			if n, _ := fmt.Sscanf(lastComp[i:], "%d", &id); n == 1 {
				return id, true
			}
			break
		}
	}
	return 0, false
}

func (dv *Router) RegisterSimBierRouters() {
	dv.mutex.Lock()
	routers := []enc.Name{dv.config.RouterName()}
	for _, entry := range dv.rib.Entries() {
		routers = append(routers, entry.Name().Clone())
	}
	dv.mutex.Unlock()

	registeredCount := 0
	for _, router := range routers {
		if id, ok := simBfrIDFromRouterName(router); ok {
			bier.SimBift().RegisterRouter(router, id)
			registeredCount++
		}
	}
	bier.SimBift().BuildFromFib()
	log.Info(dv, "registerSimBierRouters: ", "numRouters", len(routers), "numRegistered", registeredCount)
}

type SyntheticRoute struct {
	Dest    enc.Name
	NextHop enc.Name
	Cost    uint64
	FaceId  uint64
}

var _syntheticRoutingStable sync.Map

func (dv *Router) syntheticRoutingIsStable() bool {
	_, ok := _syntheticRoutingStable.Load(dv)
	return ok
}

func (dv *Router) SeedSyntheticRoutes(routes []SyntheticRoute) {
	dv.mutex.Lock()
	for _, route := range routes {
		dv.neighbors.SimSetNeighborFace(route.NextHop, route.FaceId)
		dv.rib.Set(route.Dest, route.NextHop, route.Cost)
	}
	dv.mutex.Unlock()

	dv.updateFib()
	dv.advert.generate()
	dv.simSeedPsdPrefix()

	_syntheticRoutingStable.Store(dv, struct{}{})
	log.Info(dv, "seedSyntheticRoutes", "numRoutes", len(routes))
}

func (dv *Router) StartPfxIfSyntheticStable() {
	if _, ok := _syntheticRoutingStable.LoadAndDelete(dv); !ok {
		return
	}
	reachableCount := 0
	dv.mutex.Lock()
	for _, entry := range dv.rib.Entries() {
		if !entry.Name().Equal(dv.config.RouterName()) {
			reachableCount++
		}
	}
	dv.mutex.Unlock()
	dv.startPfxSyntheticReady(reachableCount)
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

// applySvsAloInitialStatePatch patches std/sync/svs_alo_initial_state.go
// to skip entries with seqNo=0. This prevents the PES SVS from trying
// to fetch snapshots from publishers that haven't published anything yet.
func applySvsAloInitialStatePatch(file *ast.File, fset *token.FileSet) bool {
	// Find and patch the s.state.Set call in parseInstanceState
	for _, decl := range file.Decls {
		funcDecl, ok := decl.(*ast.FuncDecl)
		if !ok || funcDecl.Name.Name != "parseInstanceState" {
			continue
		}
		// Find the outer for-range loop
		var rangeStmt *ast.RangeStmt
		for _, stmt := range funcDecl.Body.List {
			if r, ok := stmt.(*ast.RangeStmt); ok {
				rangeStmt = r
				break
			}
		}
		if rangeStmt == nil {
			return false
		}
		// Find the inner for-range loop (the seqNoEntries loop)
		var innerRangeStmt *ast.RangeStmt
		for _, stmt := range rangeStmt.Body.List {
			if r, ok := stmt.(*ast.RangeStmt); ok {
				innerRangeStmt = r
				break
			}
		}
		if innerRangeStmt == nil {
			return false
		}
		// Find the s.state.Set call in the inner loop body
		for _, stmt := range innerRangeStmt.Body.List {
			exprStmt, ok := stmt.(*ast.ExprStmt)
			if !ok {
				continue
			}
			call, ok := exprStmt.X.(*ast.CallExpr)
			if !ok {
				continue
			}
			sel, ok := call.Fun.(*ast.SelectorExpr)
			if !ok || sel.Sel.Name != "Set" {
				continue
			}
			// s.state.Set has sel.X as SelectorExpr{SelectorExpr{X:Ident"s", Sel:Ident"state"}, Sel:Ident"Set"}
			// We need to check: sel.X is SelectorExpr and sel.X.X is Ident"s" and sel.X.Sel is Ident"state"
			stateSel, ok := sel.X.(*ast.SelectorExpr)
			if !ok {
				continue
			}
			if stateSel.Sel.Name != "state" {
				continue
			}
			sIdent, ok := stateSel.X.(*ast.Ident)
			if !ok || sIdent.Name != "s" {
				continue
			}
			// Found s.state.Set(...), insert the check before it
			check := &ast.IfStmt{
				Cond: &ast.BinaryExpr{
					X:  &ast.SelectorExpr{X: &ast.Ident{Name: "seqEntry"}, Sel: &ast.Ident{Name: "SeqNo"}},
					Op: token.EQL,
					Y:  &ast.Ident{Name: "0"},
				},
				Body: &ast.BlockStmt{
					List: []ast.Stmt{&ast.BranchStmt{Tok: token.CONTINUE}},
				},
			}
			innerRangeStmt.Body.List = append([]ast.Stmt{check}, innerRangeStmt.Body.List...)
			return true
		}
	}
	return false
}

// applySvsAloOnSvsUpdatePatch patches std/sync/svs_alo.go
// to skip triggering fetches when a newly discovered publisher has Latest=1
// (seqNo=1 from snapshot that wasn't in the restored state).
func applySvsAloOnSvsUpdatePatch(file *ast.File) bool {
	// No-op: the actual fix is in applySvsAloConsumeCheckPatch
	return true
}

// applySvsAloConsumeCheckPatch patches std/sync/svs_alo_data.go.
// After "fstate := entry.Value" inside the for loop, inserts:
//
//	if fstate.Known == 0 && fstate.Latest <= 1 {
//		continue
//	}
//
// This prevents consumeCheck from trying to fetch publishers that were added
// to state via onSvsUpdate but weren't in the snapshot (parseInstanceState skips
// seqNo=0 entries). The sync may report Latest=1, but we shouldn't fetch seq=1
// until we've successfully received it once (Known > 0). This avoids the
// "retries exhausted, segment number=0" error when a new publisher joins.
func applySvsAloConsumeCheckPatch(file *ast.File) bool {
	// Find consumeCheck function
	var consumeCheckFunc *ast.FuncDecl
	for _, decl := range file.Decls {
		funcDecl, ok := decl.(*ast.FuncDecl)
		if !ok || funcDecl.Name.Name != "consumeCheck" {
			continue
		}
		consumeCheckFunc = funcDecl
		break
	}
	if consumeCheckFunc == nil {
		return false
	}

	// Find: fstate := entry.Value inside a for range loop
	var parentRange *ast.RangeStmt
	var fstateAssignIdx int

	for _, stmt := range consumeCheckFunc.Body.List {
		rangeStmt, ok := stmt.(*ast.RangeStmt)
		if !ok {
			continue
		}
		for i, s := range rangeStmt.Body.List {
			assign, ok := s.(*ast.AssignStmt)
			if !ok || assign.Tok != token.DEFINE {
				continue
			}
			if len(assign.Lhs) == 1 && len(assign.Rhs) == 1 {
				ident, ok := assign.Lhs[0].(*ast.Ident)
				if !ok || ident.Name != "fstate" {
					continue
				}
				sel, ok := assign.Rhs[0].(*ast.SelectorExpr)
				if !ok {
					continue
				}
				ident2, ok := sel.X.(*ast.Ident)
				if !ok || ident2.Name != "entry" || sel.Sel.Name != "Value" {
					continue
				}
				parentRange = rangeStmt
				fstateAssignIdx = i
				goto insert
			}
		}
	}
	return false

insert:
	// Insert guard after fstate := entry.Value in the for loop body.
	// Skip fetch when Known=0 AND Latest<=1. Known=0 means we haven't received
	// anything from this publisher. If Latest=1, the sync said seq=1 exists
	// but parseInstanceState skipped it (seqNo=0 entries not in snapshot).
	// In that case, we should not try to fetch seq=1 either - wait for the
	// publisher to re-advertise it after the next publish.
	guard := &ast.IfStmt{
		Cond: &ast.BinaryExpr{
			X: &ast.BinaryExpr{
				X: &ast.SelectorExpr{
					X:   ast.NewIdent("fstate"),
					Sel: ast.NewIdent("Known"),
				},
				Op: token.EQL,
				Y:  &ast.BasicLit{Kind: token.INT, Value: "0"},
			},
			Op: token.LAND,
			Y: &ast.BinaryExpr{
				X: &ast.SelectorExpr{
					X:   ast.NewIdent("fstate"),
					Sel: ast.NewIdent("Latest"),
				},
				Op: token.LEQ,
				Y:  &ast.BasicLit{Kind: token.INT, Value: "1"},
			},
		},
		Body: &ast.BlockStmt{
			List: []ast.Stmt{&ast.BranchStmt{Tok: token.CONTINUE}},
		},
	}

	newList := make([]ast.Stmt, len(parentRange.Body.List)+1)
	copy(newList[:fstateAssignIdx+1], parentRange.Body.List[:fstateAssignIdx+1])
	newList[fstateAssignIdx+1] = guard
	copy(newList[fstateAssignIdx+2:], parentRange.Body.List[fstateAssignIdx+1:])
	parentRange.Body.List = newList
	return true
}

// svsAloSimSnippet is injected into std/sync/svs_alo.go.
// Provides direct-delivery helpers that replace the channel-based run() loop
// in simulation mode.  All types (SvsALO, SvsPub, enc.Name) are already
// visible in the package; no extra imports required.
const svsAloSimSnippet = `
// simQueuePub delivers pub directly to its subscribers, bypassing the outpipe
// channel.  Called by transformer-generated code replacing s.outpipe <- pub.
func (s *SvsALO) simQueuePub(pub SvsPub) {
	for _, subscription := range pub.subcribers {
		subscription(pub)
	}
}

// simQueueError delivers err directly to the error callback, bypassing the
// errpipe channel.  Called by transformer-generated code.
func (s *SvsALO) simQueueError(err error) {
	if s.onError != nil {
		s.onError(err)
	}
}

// simQueuePubl delivers name directly to the publisher callback, bypassing the
// publpipe channel.  Called by transformer-generated code replacing
// s.publpipe <- name.
func (s *SvsALO) simQueuePubl(name enc.Name) {
	if s.onPublisher != nil {
		s.onPublisher(name)
	}
}

// simStop is a no-op in simulation mode: run() is never started so there is
// no goroutine to signal.  Called by transformer-generated code replacing
// s.stop <- struct{}{} in Stop().
func (s *SvsALO) simStop() {}

// SimStartQuiet starts the underlying SVS handler without emitting bootstrap
// sync traffic. The next publication still advertises immediately.
func (s *SvsALO) SimStartQuiet() error {
	s.opts.Snapshot = &SnapshotNull{}
	return s.svs.SimStartQuiet()
}

// NumPendingFetchInterests returns the total number of in-flight data fetch
// Interests across all publishers. This is the sum of (Pending - Known) for
// each publisher. A non-zero value indicates there are Interests that have
// been sent but not yet satisfied.
func (s *SvsALO) NumPendingFetchInterests() uint64 {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	totalPending := uint64(0)
	for node := range s.state.Iter() {
		for _, entry := range s.state[node.TlvStr()] {
			totalPending += entry.Value.Pending - entry.Value.Known
		}
	}
	return totalPending
}

// GetAllPublishers returns a map of all known publisher names (as strings).
// Used by snapshot import to trigger consumeCheck after restoring state.
func (s *SvsALO) GetAllPublishers() map[string]bool {
	s.mutex.Lock()
	defer s.mutex.Unlock()

	publishers := make(map[string]bool)
	for name := range s.state.Iter() {
		publishers[name.TlvStr()] = true
	}
	return publishers
}

// ConsumeCheckForPublisherByString triggers a consumeCheck for a specific publisher
// by string name. Used after snapshot import to start fetching prefix data.
func (s *SvsALO) ConsumeCheckForPublisherByString(name string) {
	s.mutex.Lock()
	defer s.mutex.Unlock()
	routerName, err := enc.NameFromTlvStr(name)
	if err != nil {
		return
	}
	s.consumeCheck(routerName)
}
`

// applySvsAloSnapContentStruct patches std/sync/svs_alo.go to add snapContent field.
func applySvsAloSnapContentStruct(file *ast.File, fset *token.FileSet) bool {
	modified := false
	ast.Inspect(file, func(n ast.Node) bool {
		typeSpec, ok := n.(*ast.TypeSpec)
		if !ok || typeSpec.Name.Name != "SvsALO" {
			return true
		}
		structType, ok := typeSpec.Type.(*ast.StructType)
		if !ok {
			return true
		}
		// Check if snapContent already exists
		for _, field := range structType.Fields.List {
			if len(field.Names) > 0 && field.Names[0].Name == "snapContent" {
				return false // already exists
			}
		}
		// Add snapContent field
		newField := &ast.Field{
			Names: []*ast.Ident{ast.NewIdent("snapContent")},
			Type: &ast.MapType{
				Key: ast.NewIdent("string"),
				Value: &ast.MapType{
					Key:   ast.NewIdent("uint64"),
					Value: ast.NewIdent("enc.Wire"),
				},
			},
		}
		structType.Fields.List = append(structType.Fields.List, newField)
		modified = true
		return false
	})
	return modified
}

// svsAloSnapContentHelpersSnippet contains helper functions for snap content storage.
const svsAloSnapContentHelpersSnippet = `
// storeSnapContent stores content for snapshot persistence.
func (s *SvsALO) storeSnapContent(node enc.Name, seq uint64, content enc.Wire) {
	nodeHash := node.TlvStr()
	if s.snapContent == nil {
		s.snapContent = make(map[string]map[uint64]enc.Wire)
	}
	if s.snapContent[nodeHash] == nil {
		s.snapContent[nodeHash] = make(map[uint64]enc.Wire)
	}
	s.snapContent[nodeHash][seq] = content
}

// initSnapContent initializes snapContent in NewSvsALO.
func initSnapContent(s *SvsALO) {
	s.snapContent = make(map[string]map[uint64]enc.Wire)
}

// encodeSnapContent encodes snapContent for persistence.
// Format: [magic:4][count:4][{hashLen:1,hash:N][boot:8][seq:8][contentLen:4][content:M}]...
func (s *SvsALO) encodeSnapContent() []byte {
	var result []byte
	result = append(result, 0xCA, 0xFE, 0xC0, 0xDE) // magic prefix

	total := 0
	for _, entries := range s.snapContent {
		total += len(entries)
	}
	result = append(result,
		byte(total>>24), byte(total>>16),
		byte(total>>8), byte(total))

	for nameHash, entries := range s.snapContent {
		for seq, content := range entries {
			boot := s.BootTime()
			for _, entry := range s.state[nameHash] {
				boot = entry.Boot
				break
			}
			hashBytes := []byte(nameHash)
			result = append(result, byte(len(hashBytes)))
			result = append(result, hashBytes...)
			result = append(result,
				byte(boot>>56), byte(boot>>48), byte(boot>>40), byte(boot>>32),
				byte(boot>>24), byte(boot>>16), byte(boot>>8), byte(boot))
			result = append(result,
				byte(seq>>56), byte(seq>>48), byte(seq>>40), byte(seq>>32),
				byte(seq>>24), byte(seq>>16), byte(seq>>8), byte(seq))
			// Flatten Wire ([][]byte) to single []byte
			var contentBytes []byte
			for _, b := range content {
				contentBytes = append(contentBytes, b...)
			}
			result = append(result,
				byte(len(contentBytes)>>24), byte(len(contentBytes)>>16),
				byte(len(contentBytes)>>8), byte(len(contentBytes)))
			result = append(result, contentBytes...)
		}
	}
	return result
}

// decodeSnapContent decodes snapContent from wire and populates PendingPubs.
func (s *SvsALO) decodeSnapContent(wire []byte) {
	if len(wire) < 8 {
		return
	}
	if wire[0] != 0xCA || wire[1] != 0xFE || wire[2] != 0xC0 || wire[3] != 0xDE {
		return
	}
	offset := 4
	count := uint32(wire[offset])<<24 | uint32(wire[offset+1])<<16 |
		uint32(wire[offset+2])<<8 | uint32(wire[offset+3])
	offset += 4

	s.snapContent = make(map[string]map[uint64]enc.Wire)

	for i := uint32(0); i < count && offset < len(wire); i++ {
		hashLen := int(wire[offset])
		offset++
		if offset+hashLen > len(wire) {
			break
		}
		nameHash := string(wire[offset : offset+hashLen])
		offset += hashLen
		if offset+8 > len(wire) {
			break
		}
		boot := uint64(wire[offset])<<56 | uint64(wire[offset+1])<<48 |
			uint64(wire[offset+2])<<40 | uint64(wire[offset+3])<<32 |
			uint64(wire[offset+4])<<24 | uint64(wire[offset+5])<<16 |
			uint64(wire[offset+6])<<8 | uint64(wire[offset+7])
		offset += 8
		if offset+8 > len(wire) {
			break
		}
		seq := uint64(wire[offset])<<56 | uint64(wire[offset+1])<<48 |
			uint64(wire[offset+2])<<40 | uint64(wire[offset+3])<<32 |
			uint64(wire[offset+4])<<24 | uint64(wire[offset+5])<<16 |
			uint64(wire[offset+6])<<8 | uint64(wire[offset+7])
		offset += 8
		if offset+4 > len(wire) {
			break
		}
		contentLen := uint32(wire[offset])<<24 | uint32(wire[offset+1])<<16 |
			uint32(wire[offset+2])<<8 | uint32(wire[offset+3])
		offset += 4
		if offset+int(contentLen) > len(wire) {
			break
		}
		// Convert []byte to enc.Wire ([][]byte) - single buffer
		content := enc.Wire{wire[offset : offset+int(contentLen)]}
		offset += int(contentLen)

		if s.snapContent[nameHash] == nil {
			s.snapContent[nameHash] = make(map[uint64]enc.Wire)
		}
		s.snapContent[nameHash][seq] = content

		// Populate PendingPubs for immediate delivery
		entry := s.state.Get(nameHash, boot)
		if entry.PendingPubs == nil {
			entry.PendingPubs = make(map[uint64]SvsPub)
		}
		entry.PendingPubs[seq] = SvsPub{
			Content:  content,
			BootTime: boot,
			SeqNum:   seq,
		}
		s.state.Set(nameHash, boot, entry)
	}
}
`

// applySvsAloSnapContentHelpers injects the helper functions.
func applySvsAloSnapContentHelpers(file *ast.File, fset *token.FileSet) bool {
	injectDecls(file, fset, svsAloSnapContentHelpersSnippet)
	return true
}

// applySvsAloSnapContentInit patches NewSvsALO to call initSnapContent.
func applySvsAloSnapContentInit(file *ast.File, fset *token.FileSet) bool {
	modified := false
	ast.Inspect(file, func(n ast.Node) bool {
		funcDecl, ok := n.(*ast.FuncDecl)
		if !ok || funcDecl.Name.Name != "NewSvsALO" {
			return true
		}
		// Find the s := &SvsALO{...} assignment and add initSnapContent call after it
		for i, stmt := range funcDecl.Body.List {
			assign, ok := stmt.(*ast.AssignStmt)
			if !ok || len(assign.Lhs) != 1 {
				continue
			}
			// Look for "s := &SvsALO{"
			ident, ok := assign.Lhs[0].(*ast.Ident)
			if !ok || ident.Name != "s" {
				continue
			}
			// Check RHS is a unary expr (&) containing a composite literal
			unaryExpr, ok := assign.Rhs[0].(*ast.UnaryExpr)
			if !ok || unaryExpr.Op != token.AND {
				continue
			}
			compLit, ok := unaryExpr.X.(*ast.CompositeLit)
			if !ok {
				continue
			}
			// Check it's SvsALO type (could be just &SvsALO or &SvsALO{...})
			if len(compLit.Elts) == 0 {
				// Empty composite literal, find closing } in source
				continue
			}
			// Find the closing brace of the struct literal
			// The statement ends at the closing }
			// Add initSnapContent call after this statement
			callStmt := &ast.ExprStmt{
				X: &ast.CallExpr{
					Fun:  &ast.Ident{Name: "initSnapContent"},
					Args: []ast.Expr{ast.NewIdent("s")},
				},
			}
			newList := make([]ast.Stmt, len(funcDecl.Body.List)+1)
			copy(newList[:i+1], funcDecl.Body.List[:i+1])
			newList[i+1] = callStmt
			copy(newList[i+2:], funcDecl.Body.List[i+1:])
			funcDecl.Body.List = newList
			modified = true
			return false
		}
		return true
	})
	return modified
}

// applySvsAloStoreContentData patches produceObject in svs_alo_data.go to call storeSnapContent.
func applySvsAloStoreContentData(file *ast.File, fset *token.FileSet) bool {
	modified := false
	ast.Inspect(file, func(n ast.Node) bool {
		funcDecl, ok := n.(*ast.FuncDecl)
		if !ok || funcDecl.Name.Name != "produceObject" {
			return true
		}
		// Find the s.state.Set(...) call and add storeSnapContent call after it
		for i, stmt := range funcDecl.Body.List {
			call, ok := stmt.(*ast.ExprStmt)
			if !ok {
				continue
			}
			callExpr, ok := call.X.(*ast.CallExpr)
			if !ok {
				continue
			}
			sel, ok := callExpr.Fun.(*ast.SelectorExpr)
			if !ok || sel.Sel.Name != "Set" {
				continue
			}
			// Check it's s.state.Set(...)
			recv, ok := sel.X.(*ast.SelectorExpr)
			if !ok || recv.Sel.Name != "state" {
				continue
			}
			callStmt := &ast.ExprStmt{
				X: &ast.CallExpr{
					Fun: &ast.SelectorExpr{
						X:   ast.NewIdent("s"),
						Sel: ast.NewIdent("storeSnapContent"),
					},
					Args: []ast.Expr{
						ast.NewIdent("node"),
						ast.NewIdent("seq"),
						ast.NewIdent("content"),
					},
				},
			}
			newList := make([]ast.Stmt, len(funcDecl.Body.List)+1)
			copy(newList[:i+1], funcDecl.Body.List[:i+1])
			newList[i+1] = callStmt
			copy(newList[i+2:], funcDecl.Body.List[i+1:])
			funcDecl.Body.List = newList
			modified = true
			return false
		}
		return true
	})
	return modified
}

// applySvsAloSnapContentState patches instanceState and parseInstanceState in place.
func applySvsAloSnapContentState(file *ast.File, fset *token.FileSet) bool {
	// Delete the old instanceState and parseInstanceState functions
	var newDecls []ast.Decl
	for _, decl := range file.Decls {
		funcDecl, ok := decl.(*ast.FuncDecl)
		if ok && (funcDecl.Name.Name == "instanceState" || funcDecl.Name.Name == "parseInstanceState") {
			continue // skip these
		}
		newDecls = append(newDecls, decl)
	}
	file.Decls = newDecls

	// Clear comments that might be associated with deleted functions
	file.Comments = nil

	// Now inject the new functions
	injectDecls(file, fset, `
func (s *SvsALO) instanceState() enc.Wire {
	state := spec_svsps.InstanceState{
		Name:          s.opts.Name,
		BootstrapTime: s.BootTime(),
		StateVector: s.state.Encode(func(state svsDataState) uint64 {
			return state.Known
		}),
	}
	_snapEncoded := state.Encode()
	return append(_snapEncoded, enc.Wire{s.encodeSnapContent()}...)
}

func (s *SvsALO) parseInstanceState(wire enc.Wire) error {
	// Flatten wire to find content boundary
	var _snapWire []byte
	for _, buf := range wire {
		_snapWire = append(_snapWire, buf...)
	}

	// Find content start by magic marker, decode content if present
	_snapContentStart := -1
	for i := 0; i < len(_snapWire)-3; i++ {
		if _snapWire[i] == 0xCA && _snapWire[i+1] == 0xFE && _snapWire[i+2] == 0xC0 && _snapWire[i+3] == 0xDE {
			_snapContentStart = i
			break
		}
	}

	var _metaWire enc.Wire
	if _snapContentStart >= 0 {
		// Content is present - find buffer index for content start
		_metaBufCount := 0
		_byteAcc := 0
		for bufIdx, buf := range wire {
			if _byteAcc+len(buf) > _snapContentStart {
				_metaBufCount = bufIdx
				break
			}
			_byteAcc += len(buf)
		}
		_metaWire = wire[:_metaBufCount]
		s.decodeSnapContent(_snapWire[_snapContentStart:])
	} else {
		// No content marker - parse entire wire as metadata
		_metaWire = wire
	}

	initState, err := spec_svsps.ParseInstanceState(enc.NewWireView(_metaWire), true)
	if err != nil {
		return err
	}

	if !initState.Name.Equal(s.opts.Name) {
		return fmt.Errorf("initial state name mismatch: %v != %v", initState.Name, s.opts.Name)
	}

	s.opts.Svs.BootTime = initState.BootstrapTime
	s.opts.Svs.InitialState = initState.StateVector

	for _, entry := range initState.StateVector.Entries {
		hash := entry.Name.TlvStr()
		for _, seqEntry := range entry.SeqNoEntries {
			if seqEntry.SeqNo == 0 {
				continue
			}
			s.state.Set(hash, seqEntry.BootstrapTime, svsDataState{
				Known:   seqEntry.SeqNo,
				Latest:  seqEntry.SeqNo,
				Pending: seqEntry.SeqNo,
			})
		}
	}

	return nil
}
`)
	return true
}

// applySvsChannels transforms channel operations in std/sync/svs.go:
//   - s.recvSv <- sv  →  s.simRecvSv(sv)
//   - s.stop <- struct{}{}  →  s.simStop()
func applySvsAloSimExtensions(file *ast.File, fset *token.FileSet) bool {
	injectDecls(file, fset, svsAloSimSnippet)
	return true
}

func applySvsChannels(file *ast.File) bool {
	modified := false
	astutil.Apply(file, func(c *astutil.Cursor) bool {
		send, ok := c.Node().(*ast.SendStmt)
		if !ok {
			return true
		}
		sel, ok := send.Chan.(*ast.SelectorExpr)
		if !ok {
			return true
		}
		switch sel.Sel.Name {
		case "recvSv":
			// s.recvSv <- sv  →  s.simRecvSv(sv)
			c.Replace(&ast.ExprStmt{X: &ast.CallExpr{
				Fun:  &ast.SelectorExpr{X: sel.X, Sel: ast.NewIdent("simRecvSv")},
				Args: []ast.Expr{send.Value},
			}})
			modified = true
		case "stop":
			// s.stop <- struct{}{}  →  s.simStop()
			c.Replace(&ast.ExprStmt{X: &ast.CallExpr{
				Fun: &ast.SelectorExpr{X: sel.X, Sel: ast.NewIdent("simStop")},
			}})
			modified = true
		}
		return true
	}, nil)
	return modified
}

// applySvsAloChannels transforms channel operations in std/sync/svs_alo.go:
//   - s.stop <- struct{}{}  →  s.simStop()
//   - s.publpipe <- name    →  s.simQueuePubl(name)
func applySvsAloChannels(file *ast.File) bool {
	modified := false
	astutil.Apply(file, func(c *astutil.Cursor) bool {
		send, ok := c.Node().(*ast.SendStmt)
		if !ok {
			return true
		}
		sel, ok := send.Chan.(*ast.SelectorExpr)
		if !ok {
			return true
		}
		switch sel.Sel.Name {
		case "stop":
			// s.stop <- struct{}{}  →  s.simStop()
			c.Replace(&ast.ExprStmt{X: &ast.CallExpr{
				Fun: &ast.SelectorExpr{X: sel.X, Sel: ast.NewIdent("simStop")},
			}})
			modified = true
		case "publpipe":
			// s.publpipe <- name  →  s.simQueuePubl(name)
			c.Replace(&ast.ExprStmt{X: &ast.CallExpr{
				Fun:  &ast.SelectorExpr{X: sel.X, Sel: ast.NewIdent("simQueuePubl")},
				Args: []ast.Expr{send.Value},
			}})
			modified = true
		}
		return true
	}, nil)
	return modified
}

// applySvsAloDataChannels transforms channel operations in std/sync/svs_alo_data.go:
//   - s.outpipe <- pub                          →  s.simQueuePub(pub)
//   - select { case s.errpipe <- err: default:}  →  s.simQueueError(err)
func applySvsAloDataChannels(file *ast.File) bool {
	modified := false
	astutil.Apply(file, func(c *astutil.Cursor) bool {
		switch n := c.Node().(type) {
		case *ast.SendStmt:
			sel, ok := n.Chan.(*ast.SelectorExpr)
			if !ok || sel.Sel.Name != "outpipe" {
				return true
			}
			// s.outpipe <- pub  →  s.simQueuePub(pub)
			c.Replace(&ast.ExprStmt{X: &ast.CallExpr{
				Fun:  &ast.SelectorExpr{X: sel.X, Sel: ast.NewIdent("simQueuePub")},
				Args: []ast.Expr{n.Value},
			}})
			modified = true

		case *ast.SelectStmt:
			// Detect: select { case s.errpipe <- err: ... default: }
			// Replace the whole SelectStmt with s.simQueueError(err).
			if n.Body == nil || len(n.Body.List) == 0 {
				return true
			}
			for _, clause := range n.Body.List {
				cc, ok := clause.(*ast.CommClause)
				if !ok || cc.Comm == nil {
					continue
				}
				send, ok := cc.Comm.(*ast.SendStmt)
				if !ok {
					continue
				}
				sel, ok := send.Chan.(*ast.SelectorExpr)
				if !ok || sel.Sel.Name != "errpipe" {
					continue
				}
				// Found the errpipe select — replace with direct delivery.
				c.Replace(&ast.ExprStmt{X: &ast.CallExpr{
					Fun:  &ast.SelectorExpr{X: sel.X, Sel: ast.NewIdent("simQueueError")},
					Args: []ast.Expr{send.Value},
				}})
				modified = true
				return false
			}
		}
		return true
	}, nil)
	return modified
}

// applyThreadSimExtensions injects simFib/simPet/simMulticastFib + Thread
// sim methods into fw/fw/thread.go and adds the "sync" import.
func applyThreadSimExtensions(file *ast.File, fset *token.FileSet) bool {
	injectDecls(file, fset, threadSimSnippet)
	// thread.go has sync/atomic but not sync itself.
	astutil.AddImport(fset, file, "sync")
	return true
}

func applyBierSimExtensions(file *ast.File, fset *token.FileSet) bool {
	injectDecls(file, fset, bierSimSnippet)
	modified := applyBierCfgIndexHooks(file)
	return true || modified
}

func applyBierCfgIndexHooks(file *ast.File) bool {
	for _, decl := range file.Decls {
		fn, ok := decl.(*ast.FuncDecl)
		if !ok || fn.Name == nil || fn.Name.Name != "CfgBierIndex" {
			continue
		}
		fn.Body = &ast.BlockStmt{List: []ast.Stmt{
			&ast.IfStmt{
				Init: &ast.AssignStmt{
					Lhs: []ast.Expr{ast.NewIdent("h")},
					Tok: token.DEFINE,
					Rhs: []ast.Expr{&ast.CallExpr{Fun: &ast.SelectorExpr{X: ast.NewIdent("_ndndsim"), Sel: ast.NewIdent("GetHooks")}}},
				},
				Cond: &ast.SelectorExpr{X: ast.NewIdent("h"), Sel: ast.NewIdent("BierIndexSet")},
				Body: &ast.BlockStmt{List: []ast.Stmt{&ast.ReturnStmt{Results: []ast.Expr{&ast.SelectorExpr{X: ast.NewIdent("h"), Sel: ast.NewIdent("BierIndex")}}}}},
			},
			&ast.ReturnStmt{Results: []ast.Expr{&ast.SelectorExpr{
				X: &ast.SelectorExpr{X: &ast.SelectorExpr{X: ast.NewIdent("core"), Sel: ast.NewIdent("C")}, Sel: ast.NewIdent("Fw")},
				Sel: ast.NewIdent("BierIndex"),
			}}},
		}}
		return true
	}
	return false
}

func applyBierSimBiftInTableAlgo(file *ast.File) bool {
	modified := false
	astutil.Apply(file, func(c *astutil.Cursor) bool {
		sel, ok := c.Node().(*ast.SelectorExpr)
		if !ok || sel.Sel.Name != "Bift" {
			return true
		}
		pkg, ok := sel.X.(*ast.Ident)
		if !ok || pkg.Name != "bier" {
			return true
		}
		c.Replace(&ast.CallExpr{Fun: &ast.SelectorExpr{X: ast.NewIdent("bier"), Sel: ast.NewIdent("SimBift")}})
		modified = true
		return true
	}, nil)
	return modified
}

func applyBierSimExtensionsOld(file *ast.File, fset *token.FileSet) bool {
	injectDecls(file, fset, bierSimSnippet)
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
	addNamedImport(file, "bier", "github.com/named-data/ndnd/fw/bier")
	// enc and log are needed by Init, LinkMulticastPrefixes, PrefixAnnounceCmd, PrefixWithdrawCmd.
	addNamedImport(file, "enc", "github.com/named-data/ndnd/std/encoding")
	addNamedImport(file, "log", "github.com/named-data/ndnd/std/log")
	addNamedImport(file, "sync", "sync")
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

	// Add self to the RIB and generate the initial advertisement.
	dv.rib.Set(dv.config.RouterName(), dv.config.RouterName(), 0)
	dv.advert.generate()

	// Initialise prefix egress state (table only; SVS not yet started).
	dv.pfx.Reset()

	// nfdc.Start() is NOT launched in simulation: all management commands
	// are executed synchronously via simExec (ruleNfdcChannelSend transform).
	// Starting the goroutine would violate the zero-goroutine constraint.

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
	// nfdc.Stop() is intentionally omitted: nfdc.Start() was never called in
	// sim mode, so there is no goroutine to signal (and sending to the stop
	// channel would deadlock).
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

// NumPendingFetchInterests returns the total number of in-flight prefix SVS
// data fetch Interests for this router. This is the sum of (Pending - Known)
// across all publishers. A non-zero value indicates there are Interests that
// have been sent but not yet satisfied.
func (dv *Router) NumPendingFetchInterests() uint64 {
	if dv.pfxSvs == nil {
		return 0
	}
	return dv.pfxSvs.NumPendingFetchInterests()
}

// LinkMulticastPrefixes returns the prefixes that must be explicitly forwarded
// to all link faces for DV sync to reach neighbors.
// At main@51774b8 there is no multicastFib/BROADCAST_STRATEGY; the caller
// must install explicit RIB routes to every link face for these prefixes.
func (dv *Router) LinkMulticastPrefixes() []enc.Name {
	return []enc.Name{
		dv.config.AdvertisementSyncPrefix(),
		dv.pfxSvs.SyncPrefix(),
	}
}

// MgmtPrefix returns the management prefix for this DV router instance.
// This is phase-specific (/localhost/dv in twophase, /localhost/nlsr in onephase)
// and must be used when constructing management Interest names.
func (dv *Router) MgmtPrefix() enc.Name {
	return dv.config.MgmtPrefix()
}

// PrefixAnnounceCmd returns the command path components appended after MgmtPrefix
// to form a prefix-announce management Interest.
// Onephase: ["rib", "register"] → /localhost/nlsr/rib/register/<params>
func (dv *Router) PrefixAnnounceCmd() enc.Name {
	return enc.Name{
		enc.NewGenericComponent("rib"),
		enc.NewGenericComponent("register"),
	}
}

// PrefixWithdrawCmd returns the command path components appended after MgmtPrefix
// to form a prefix-withdraw management Interest.
// Onephase: ["rib", "unregister"] → /localhost/nlsr/rib/unregister/<params>
func (dv *Router) PrefixWithdrawCmd() enc.Name {
	return enc.Name{
		enc.NewGenericComponent("rib"),
		enc.NewGenericComponent("unregister"),
	}
}

// _pfxStarted tracks which Router instances have had pfx sync started.
// Used by startPfxOnce to guarantee exactly-once startup of the prefix daemon
// even though notify.go (shared between phases) calls startPfxOnce on every
// convergence hook invocation.
var _pfxStarted sync.Map

// startPfxOnce starts the prefix SVS sync exactly once per Router instance.
// The reachableCount argument is accepted for signature compatibility with
// notify.go but is unused in the onephase path.
func (dv *Router) startPfxOnce(_ int) {
	if _, loaded := _pfxStarted.LoadOrStore(dv, struct{}{}); !loaded {
		dv.pfxSvs.Start()
	}
}

type SyntheticRoute struct {
	Dest    enc.Name
	NextHop enc.Name
	Cost    uint64
	FaceId  uint64
}

var _syntheticRoutingStable sync.Map

func (dv *Router) syntheticRoutingIsStable() bool {
	_, ok := _syntheticRoutingStable.Load(dv)
	return ok
}

func (dv *Router) SeedSyntheticRoutes(routes []SyntheticRoute) {
	dv.mutex.Lock()
	for _, route := range routes {
		dv.neighbors.SimSetNeighborFace(route.NextHop, route.FaceId)
		dv.rib.Set(route.Dest, route.NextHop, route.Cost)
	}
	dv.mutex.Unlock()

	dv.updateFib()
	dv.advert.generate()

	_syntheticRoutingStable.Store(dv, struct{}{})
	log.Info(dv, "seedSyntheticRoutes", "numRoutes", len(routes))
}

func (dv *Router) StartPfxIfSyntheticStable() {
	if _, ok := _syntheticRoutingStable.LoadAndDelete(dv); !ok {
		return
	}
	dv.startPfxSyntheticReady(0)
}
`

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
	// enc is needed by LinkMulticastPrefixes.
	addNamedImport(file, "enc", "github.com/named-data/ndnd/std/encoding")
	addNamedImport(file, "sync", "sync")
	return true
}
