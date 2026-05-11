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
// In fresh mode, this is the ONLY place where PES subscriptions are set up:
// SubscribePublisher is called for all known neighbors before pfxSvs.Start().
// (In snap-import mode, subscriptions are also set up in ImportSnapshot.)
//
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

	// Subscribe to PES routers before starting pfxSvs.
	// In snap-import mode this was already done in ImportSnapshot.
	// In fresh mode we need to subscribe to all known neighbors.
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
		if dv.pfx.replicatePes && dv.pfx.nfdc != nil {
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
// faces for DV sync to reach neighbors.
// In twophase, the multicastFib BROADCAST_STRATEGY handles this automatically;
// return nil so the caller skips explicit link-face route installation.
func (dv *Router) LinkMulticastPrefixes() []enc.Name {
	return nil
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

// RouterSnapshotRow is a JSON-serialisable RIB row.
type RouterSnapshotRibRow struct {
	Dest    string ` + "`" + `json:"dest"` + "`" + `
	NextHop string ` + "`" + `json:"next_hop"` + "`" + `
	Cost    uint64 ` + "`" + `json:"cost"` + "`" + `
}

// RouterSnapshotFibRow is a JSON-serialisable FIB row.
type RouterSnapshotFibRow struct {
	Prefix string ` + "`" + `json:"prefix"` + "`" + `
	FaceId uint64 ` + "`" + `json:"face_id"` + "`" + `
	Cost   uint64 ` + "`" + `json:"cost"` + "`" + `
}

// RouterSnapshotPfxEntry is a JSON-serialisable prefix entry.
type RouterSnapshotPfxEntry struct {
	Router    string ` + "`" + `json:"router"` + "`" + `
	Name      string ` + "`" + `json:"name"` + "`" + `
	Multicast bool   ` + "`" + `json:"multicast"` + "`" + `
	NextHops  []RouterSnapshotPfxNextHop ` + "`" + `json:"next_hops"` + "`" + `
}

// RouterSnapshotPfxNextHop is a JSON-serialisable prefix next-hop.
type RouterSnapshotPfxNextHop struct {
	Face uint64 ` + "`" + `json:"face"` + "`" + `
	Cost uint64 ` + "`" + `json:"cost"` + "`" + `
}

// RouterSnapshotNeighborEntry is one entry in an exported neighbor advertisement.
type RouterSnapshotNeighborEntry struct {
	Dest      string ` + "`" + `json:"dest"` + "`" + `
	NextHop   string ` + "`" + `json:"next_hop"` + "`" + `
	Cost      uint64 ` + "`" + `json:"cost"` + "`" + `
	OtherCost uint64 ` + "`" + `json:"other_cost"` + "`" + `
}

// RouterSnapshotNeighbor is a JSON-serialisable exported neighbor advert.
type RouterSnapshotNeighbor struct {
	Name       string                        ` + "`" + `json:"name"` + "`" + `
	Entries    []RouterSnapshotNeighborEntry ` + "`" + `json:"entries"` + "`" + `
	AdvertBoot uint64                        ` + "`" + `json:"advert_boot,omitempty"` + "`" + `
	AdvertSeq  uint64                        ` + "`" + `json:"advert_seq,omitempty"` + "`" + `
}

// RouterSnapshot is the full serialisable state of a Router instance.
type RouterSnapshot struct {
	Rib        []RouterSnapshotRibRow    ` + "`" + `json:"rib"` + "`" + `
	Fib        []RouterSnapshotFibRow    ` + "`" + `json:"fib"` + "`" + `
	PfxEntries []RouterSnapshotPfxEntry  ` + "`" + `json:"pfx_entries"` + "`" + `
	Neighbors  []RouterSnapshotNeighbor  ` + "`" + `json:"neighbors"` + "`" + `
	PfxSvsState []byte                   ` + "`" + `json:"pfx_svs_state,omitempty"` + "`" + `
}

// ExportSnapshot captures the current DV state into a RouterSnapshot.
// Must be called after DV and prefix-SVS have converged.
func (dv *Router) ExportSnapshot() RouterSnapshot {
	dv.mutex.Lock()
	ribRows := dv.rib.Snapshot()
	fibRows := dv.fib.Snapshot()
	pfxEntries := dv.pfx.SnapshotEntries()
	neighbors := dv.neighbors.GetAll()
	dv.mutex.Unlock()

	snap := RouterSnapshot{}
	for _, r := range ribRows {
		snap.Rib = append(snap.Rib, RouterSnapshotRibRow{
			Dest:    r.Dest.TlvStr(),
			NextHop: r.NextHop.TlvStr(),
			Cost:    r.Cost,
		})
	}
	for _, f := range fibRows {
		snap.Fib = append(snap.Fib, RouterSnapshotFibRow{
			Prefix: f.Prefix.TlvStr(),
			FaceId: f.FaceId,
			Cost:   f.Cost,
		})
	}
	for _, e := range pfxEntries {
		entry := RouterSnapshotPfxEntry{
			Router:    e.Router.TlvStr(),
			Name:      e.Name.TlvStr(),
			Multicast: e.Multicast,
		}
		for _, nh := range e.NextHops {
			entry.NextHops = append(entry.NextHops, RouterSnapshotPfxNextHop{Face: nh.Face, Cost: nh.Cost})
		}
		snap.PfxEntries = append(snap.PfxEntries, entry)
	}
	for _, ns := range neighbors {
		if ns.Advert == nil {
			continue
		}
		sn := RouterSnapshotNeighbor{Name: ns.Name.TlvStr(), AdvertBoot: ns.AdvertBoot, AdvertSeq: ns.AdvertSeq}
		for _, e := range ns.Advert.Entries {
			if e.Destination == nil || e.NextHop == nil {
				continue
			}
			sn.Entries = append(sn.Entries, RouterSnapshotNeighborEntry{
				Dest:      e.Destination.Name.TlvStr(),
				NextHop:   e.NextHop.Name.TlvStr(),
				Cost:      e.Cost,
				OtherCost: e.OtherCost,
			})
		}
		snap.Neighbors = append(snap.Neighbors, sn)
	}
	if wire := dv.pfx.pfxSvs.ExportInstanceState(); len(wire) > 0 {
		snap.PfxSvsState = wire.Join()
	}
	return snap
}

// ImportSnapshot restores DV state from a RouterSnapshot.
// Must be called after Init() and before the first heartbeat tick.
func (dv *Router) ImportSnapshot(snap RouterSnapshot) error {
	dv.mutex.Lock()
	// Restore RIB rows as fallback for neighbors without exported adverts.
	for _, r := range snap.Rib {
		dest, err := enc.NameFromTlvStr(r.Dest)
		if err != nil {
			dv.mutex.Unlock()
			return fmt.Errorf("ImportSnapshot: bad dest name %q: %w", r.Dest, err)
		}
		nh, err := enc.NameFromTlvStr(r.NextHop)
		if err != nil {
			dv.mutex.Unlock()
			return fmt.Errorf("ImportSnapshot: bad next-hop name %q: %w", r.NextHop, err)
		}
		dv.rib.Set(dest, nh, r.Cost)
	}
	// Restore FIB.
	fibRowMap := make(map[string][]table.FibEntry)
	for _, f := range snap.Fib {
		fibRowMap[f.Prefix] = append(fibRowMap[f.Prefix], table.FibEntry{FaceId: f.FaceId, Cost: f.Cost})
	}
	for prefixStr, entries := range fibRowMap {
		name, err := enc.NameFromTlvStr(prefixStr)
		if err != nil {
			dv.mutex.Unlock()
			return fmt.Errorf("ImportSnapshot: bad fib prefix %q: %w", prefixStr, err)
		}
		dv.fib.Update(name, entries)
	}
	// Restore prefix egress state entries.
	pfxEntries := make([]table.PrefixSnapshotEntry, 0, len(snap.PfxEntries))
	for _, e := range snap.PfxEntries {
		router, err := enc.NameFromTlvStr(e.Router)
		if err != nil {
			dv.mutex.Unlock()
			return fmt.Errorf("ImportSnapshot: bad pfx router name %q: %w", e.Router, err)
		}
		name, err := enc.NameFromTlvStr(e.Name)
		if err != nil {
			dv.mutex.Unlock()
			return fmt.Errorf("ImportSnapshot: bad pfx name %q: %w", e.Name, err)
		}
		entry := table.PrefixSnapshotEntry{
			Router:    router,
			Name:      name,
			Multicast: e.Multicast,
		}
		for _, nh := range e.NextHops {
			entry.NextHops = append(entry.NextHops, table.PrefixNextHop{Face: nh.Face, Cost: nh.Cost})
		}
		pfxEntries = append(pfxEntries, entry)
	}
	dv.mutex.Unlock()
	dv.pfx.RestorePfxEntries(pfxEntries)
	if len(snap.PfxSvsState) > 0 {
		if err := dv.pfx.pfxSvs.ImportInstanceState(enc.Wire{snap.PfxSvsState}); err != nil {
			log.Warn(dv, "ImportSnapshot: pfxSvs state restore failed", "err", err)
		}
	}

	// Rebuild RIB from exported neighbor adverts via DV's own updateRib logic.
	for _, sn := range snap.Neighbors {
		name, err := enc.NameFromTlvStr(sn.Name)
		if err != nil {
			continue
		}
		advert := &tlv.Advertisement{}
		for _, e := range sn.Entries {
			dest, err2 := enc.NameFromTlvStr(e.Dest)
			if err2 != nil {
				continue
			}
			nh, err2 := enc.NameFromTlvStr(e.NextHop)
			if err2 != nil {
				continue
			}
			advert.Entries = append(advert.Entries, &tlv.AdvEntry{
				Destination: &tlv.Destination{Name: dest},
				NextHop:     &tlv.Destination{Name: nh},
				Cost:        e.Cost,
				OtherCost:   e.OtherCost,
			})
		}
		dv.mutex.Lock()
		realNs := dv.neighbors.Add(name)
		realNs.Advert = advert
		realNs.AdvertBoot = sn.AdvertBoot
		realNs.AdvertSeq = sn.AdvertSeq
		dv.mutex.Unlock()
		dv.updateRib(realNs)
	}

	// Install all restored PES entries to PET.
	// After ImportInstanceState, the SVS considers restored publications as
	// already-seen (Latest seqnos are restored), so SubscribePublisher callbacks
	// will NOT fire for them. We must directly install to PET here.
	selfName := dv.config.RouterName().TlvStr()
	dv.mutex.Lock()
	for _, pfxEntry := range dv.pfx.SnapshotEntries() {
		routerName := pfxEntry.Router.TlvStr()
		if routerName == selfName {
			continue // skip self
		}
		routerHash := pfxEntry.Router.Hash()
		_, alreadySubscribed := dv.pfx.pfxSubs[routerHash]
		if alreadySubscribed {
			// Already subscribed (from stage-1 or from running startPfxOnce
			// before ImportSnapshot). Just install to PET if needed.
			if dv.pfx.replicatePes && dv.pfx.nfdc != nil {
				route := dv.pfx.pfxGroup.Append(pfxEntry.Router...)
				dv.pfx.nfdc.Exec(nfdc.NfdMgmtCmd{
					Module:  "pet",
					Cmd:     "add-egress",
					Args:    &mgmt.ControlArgs{Name: route, Egress: &mgmt.EgressRecord{Name: pfxEntry.Router.Clone()}},
					Retries: -1,
				})
			}
			continue
		}
		// New subscription: add to pfxSeen/pfxSubs and install to PET.
		dv.pfx.pfxSeen[routerHash] = pfxEntry.Router.Clone()
		dv.pfx.pfxSubs[routerHash] = pfxEntry.Router.Clone()
		if dv.pfx.replicatePes && dv.pfx.nfdc != nil {
			route := dv.pfx.pfxGroup.Append(pfxEntry.Router...)
			dv.pfx.nfdc.Exec(nfdc.NfdMgmtCmd{
				Module:  "pet",
				Cmd:     "add-egress",
				Args:    &mgmt.ControlArgs{Name: route, Egress: &mgmt.EgressRecord{Name: pfxEntry.Router.Clone()}},
				Retries: -1,
			})
		}
		// Subscribe to receive future PES updates from this router.
		err := dv.pfx.pfxSvs.SubscribePublisher(pfxEntry.Router, func(sp ndn_sync.SvsPub) {
			_ndndsim.NdndsimRecordPfxSvsDelivery()
			dv.pfx.mu.Lock()
			_, petOps := dv.pfx.processUpdate(sp.Content)
			dv.pfx.mu.Unlock()
			dv.pfx.applyPetOps(petOps)
		})
		if err != nil {
			delete(dv.pfx.pfxSubs, routerHash)
			log.Warn(dv.pfx, "ImportSnapshot: failed to subscribe to PES router", "router", pfxEntry.Router, "err", err)
		}
	}
	dv.mutex.Unlock()

	// Start pfxSvs immediately since the snapshot represents already-converged
	// routing state. Unlike fresh startup (where debouncing prevents broadcast
	// storms before nexthops exist), an imported snapshot already has RIB/FIB
	// entries installed, so there's no storm risk.
	// Cancel any pending startPfxOnce timer first to prevent double-start.
	if cancel, ok := _pfxCancel.Load(dv); ok {
		cancel.(func())()
		_pfxCancel.Delete(dv)
	}
	if _, loaded := _pfxStarted.LoadOrStore(dv, struct{}{}); !loaded {
		dv.pfx.pfxSvs.Start()
		// Note: do NOT call startFaceEvents() here. The face event history is
		// not available at ImportSnapshot time (before simulation starts), so
		// calling it would only fetch a partial/incomplete set of faces.
		// The face events will be processed naturally when DV convergence
		// triggers startPfxOnce later. Similarly, prefix prune runs on its own
		// timer and doesn't need to be started explicitly.
	}

	return nil
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
`

func applySvsAloSimExtensions(file *ast.File, fset *token.FileSet) bool {
	injectDecls(file, fset, svsAloSimSnippet)
	return true
}

// applySvsChannels transforms channel operations in std/sync/svs.go:
//   - s.recvSv <- sv  →  s.simRecvSv(sv)
//   - s.stop <- struct{}{}  →  s.simStop()
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

// applySnapshotEvictionDisableInSim wraps the eviction block in takeSnap
// (snapshot_node_latest.go) with:
//
//	if !_ndndsim.IsSynchronous() { ... }
//
// In synchronous sim mode, MemoryStore has unlimited capacity, so evicting old
// publications is unnecessary.  Without this guard, takeSnap() calls
// RemoveFlatRange to erase seqno=0..N-3*Threshold once seqNo >= 4*Threshold.
// In the 3-stage scenario all prefix announcements happen at t=0 before any
// consumer can fetch them, so the eviction races ahead and removes data objects
// that consumers need, causing all fetches to time out with "retries exhausted".
func applySnapshotEvictionDisableInSim(file *ast.File) bool {
	modified := false
	astutil.Apply(file, func(c *astutil.Cursor) bool {
		ifStmt, ok := c.Node().(*ast.IfStmt)
		if !ok || ifStmt.Init != nil {
			return true
		}
		// Check if the body contains a RemoveFlatRange call.
		containsEviction := false
		ast.Inspect(ifStmt.Body, func(n ast.Node) bool {
			call, ok := n.(*ast.CallExpr)
			if !ok {
				return true
			}
			if sel, ok := call.Fun.(*ast.SelectorExpr); ok && sel.Sel.Name == "RemoveFlatRange" {
				containsEviction = true
				return false
			}
			return true
		})
		if !containsEviction {
			return true
		}
		// Wrap with: if !_ndndsim.IsSynchronous() { <original if> }
		c.Replace(&ast.IfStmt{
			Cond: &ast.UnaryExpr{
				Op: token.NOT,
				X: &ast.CallExpr{
					Fun: &ast.SelectorExpr{
						X:   ast.NewIdent("_ndndsim"),
						Sel: ast.NewIdent("IsSynchronous"),
					},
				},
			},
			Body: &ast.BlockStmt{List: []ast.Stmt{ifStmt}},
		})
		modified = true
		return false
	}, nil)
	return modified
}

// applySnapshotDisableInSim wraps the call s.opts.Snapshot.onUpdate(s.state, node)
// inside consumeCheck with:
//
//	if !_ndndsim.IsSynchronous() { s.opts.Snapshot.onUpdate(s.state, node) }
//
// Without this guard, SnapshotNodeLatest.onUpdate sets SnapBlock=1 whenever
// Known==0, preventing normal sequential PFS data fetching in the sim.  The
// snapshot never arrives (no snapshot was produced), so SnapBlock stays 1
// forever and consumeCheck can never make progress.
func applySnapshotDisableInSim(file *ast.File) bool {
	modified := false
	astutil.Apply(file, func(c *astutil.Cursor) bool {
		stmt, ok := c.Node().(*ast.ExprStmt)
		if !ok {
			return true
		}
		call, ok := stmt.X.(*ast.CallExpr)
		if !ok {
			return true
		}
		// Match s.opts.Snapshot.onUpdate(...)
		outer, ok := call.Fun.(*ast.SelectorExpr)
		if !ok || outer.Sel.Name != "onUpdate" {
			return true
		}
		// Wrap with: if !_ndndsim.IsSynchronous() { <original> }
		c.Replace(&ast.IfStmt{
			Cond: &ast.UnaryExpr{
				Op: token.NOT,
				X: &ast.CallExpr{
					Fun: &ast.SelectorExpr{
						X:   ast.NewIdent("_ndndsim"),
						Sel: ast.NewIdent("IsSynchronous"),
					},
				},
			},
			Body: &ast.BlockStmt{List: []ast.Stmt{stmt}},
		})
		modified = true
		return false
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
	// fmt, enc, table and tlv are needed for ExportSnapshot/ImportSnapshot.
	astutil.AddImport(fset, file, "fmt")
	addNamedImport(file, "enc", "github.com/named-data/ndnd/std/encoding")
	addNamedImport(file, "table", "github.com/named-data/ndnd/dv/table")
	addNamedImport(file, "tlv", "github.com/named-data/ndnd/dv/tlv")
	// log is needed for ImportSnapshot warning.
	addNamedImport(file, "log", "github.com/named-data/ndnd/std/log")
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
	dv.startPfxOnce(0)

	// Add self to the RIB and generate the initial advertisement.
	dv.rib.Set(dv.config.RouterName(), dv.config.RouterName(), 0)
	dv.advert.generate()

	// Reset prefix table.
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
// In the onephase build Init() already calls pfxSvs.Start(); subsequent calls
// from runConvergenceHook are deduplicated by _pfxStarted and are no-ops.
// The reachableCount argument is accepted for signature compatibility with
// notify.go but is unused in the onephase (immediate-start) path.
func (dv *Router) startPfxOnce(_ int) {
	if _, loaded := _pfxStarted.LoadOrStore(dv, struct{}{}); !loaded {
		dv.pfxSvs.Start()
	}
}

// RouterSnapshotRibRow is a JSON-serialisable RIB row.
type RouterSnapshotRibRow struct {
	Dest    string ` + "`" + `json:"dest"` + "`" + `
	NextHop string ` + "`" + `json:"next_hop"` + "`" + `
	Cost    uint64 ` + "`" + `json:"cost"` + "`" + `
}

// RouterSnapshotFibRow is a JSON-serialisable FIB row.
type RouterSnapshotFibRow struct {
	Prefix string ` + "`" + `json:"prefix"` + "`" + `
	FaceId uint64 ` + "`" + `json:"face_id"` + "`" + `
	Cost   uint64 ` + "`" + `json:"cost"` + "`" + `
}

// RouterSnapshotNeighborEntry is one entry in an exported neighbor advertisement.
type RouterSnapshotNeighborEntry struct {
	Dest      string ` + "`" + `json:"dest"` + "`" + `
	NextHop   string ` + "`" + `json:"next_hop"` + "`" + `
	Cost      uint64 ` + "`" + `json:"cost"` + "`" + `
	OtherCost uint64 ` + "`" + `json:"other_cost"` + "`" + `
}

// RouterSnapshotNeighbor is a JSON-serialisable exported neighbor advert.
type RouterSnapshotNeighbor struct {
	Name       string                        ` + "`" + `json:"name"` + "`" + `
	Entries    []RouterSnapshotNeighborEntry ` + "`" + `json:"entries"` + "`" + `
	AdvertBoot uint64                        ` + "`" + `json:"advert_boot,omitempty"` + "`" + `
	AdvertSeq  uint64                        ` + "`" + `json:"advert_seq,omitempty"` + "`" + `
}

// RouterSnapshotPfxEntry is a JSON-serialisable remote prefix entry.
type RouterSnapshotPfxEntry struct {
	Router string ` + "`" + `json:"router"` + "`" + `
	Name   string ` + "`" + `json:"name"` + "`" + `
	Cost   uint64 ` + "`" + `json:"cost"` + "`" + `
}

// RouterSnapshot is the full serialisable state of a Router instance.
type RouterSnapshot struct {
	Rib         []RouterSnapshotRibRow    ` + "`" + `json:"rib"` + "`" + `
	Fib         []RouterSnapshotFibRow    ` + "`" + `json:"fib"` + "`" + `
	Neighbors   []RouterSnapshotNeighbor  ` + "`" + `json:"neighbors"` + "`" + `
	PfxSvsState []byte                    ` + "`" + `json:"pfx_svs_state,omitempty"` + "`" + `
	PfxTable    []RouterSnapshotPfxEntry  ` + "`" + `json:"pfx_table,omitempty"` + "`" + `
}

// ExportSnapshot captures the current DV state into a RouterSnapshot.
// Must be called after DV and prefix-SVS have converged.
func (dv *Router) ExportSnapshot() RouterSnapshot {
	dv.mutex.Lock()
	ribRows := dv.rib.Snapshot()
	fibRows := dv.fib.Snapshot()
	neighbors := dv.neighbors.GetAll()
	dv.mutex.Unlock()

	snap := RouterSnapshot{}
	for _, r := range ribRows {
		snap.Rib = append(snap.Rib, RouterSnapshotRibRow{
			Dest:    r.Dest.TlvStr(),
			NextHop: r.NextHop.TlvStr(),
			Cost:    r.Cost,
		})
	}
	for _, f := range fibRows {
		snap.Fib = append(snap.Fib, RouterSnapshotFibRow{
			Prefix: f.Prefix.TlvStr(),
			FaceId: f.FaceId,
			Cost:   f.Cost,
		})
	}
	for _, ns := range neighbors {
		if ns.Advert == nil {
			continue
		}
		sn := RouterSnapshotNeighbor{Name: ns.Name.TlvStr(), AdvertBoot: ns.AdvertBoot, AdvertSeq: ns.AdvertSeq}
		for _, e := range ns.Advert.Entries {
			if e.Destination == nil || e.NextHop == nil {
				continue
			}
			sn.Entries = append(sn.Entries, RouterSnapshotNeighborEntry{
				Dest:      e.Destination.Name.TlvStr(),
				NextHop:   e.NextHop.Name.TlvStr(),
				Cost:      e.Cost,
				OtherCost: e.OtherCost,
			})
		}
		snap.Neighbors = append(snap.Neighbors, sn)
	}

	// Export pfxSvs Known seqNos so Stage N+1 skips re-fetching already-seen data.
	if wire := dv.pfxSvs.ExportInstanceState(); len(wire) > 0 {
		snap.PfxSvsState = wire.Join()
	}

	// Export the prefix table for all remote routers visible in the RIB so
	// Stage N+1 has an immediately-populated table without waiting for SVS sync.
	selfName := dv.config.RouterName().TlvStr()
	seen := make(map[string]bool)
	for _, r := range snap.Rib {
		if seen[r.Dest] || r.Dest == selfName {
			continue
		}
		seen[r.Dest] = true
		routerName, err := enc.NameFromTlvStr(r.Dest)
		if err != nil {
			continue
		}
		router := dv.pfx.GetRouter(routerName)
		for _, entry := range router.Prefixes {
			snap.PfxTable = append(snap.PfxTable, RouterSnapshotPfxEntry{
				Router: r.Dest,
				Name:   entry.Name.TlvStr(),
				Cost:   entry.Cost,
			})
		}
	}

	return snap
}

// ImportSnapshot restores DV state from a RouterSnapshot.
// Must be called after Init() and before the first heartbeat tick.
func (dv *Router) ImportSnapshot(snap RouterSnapshot) error {
	dv.mutex.Lock()
	// Restore RIB rows as fallback for neighbors without exported adverts.
	for _, r := range snap.Rib {
		dest, err := enc.NameFromTlvStr(r.Dest)
		if err != nil {
			dv.mutex.Unlock()
			return fmt.Errorf("ImportSnapshot: bad dest name %q: %w", r.Dest, err)
		}
		nh, err := enc.NameFromTlvStr(r.NextHop)
		if err != nil {
			dv.mutex.Unlock()
			return fmt.Errorf("ImportSnapshot: bad next-hop name %q: %w", r.NextHop, err)
		}
		dv.rib.Set(dest, nh, r.Cost)
	}
	fibRowMap := make(map[string][]table.FibEntry)
	for _, f := range snap.Fib {
		fibRowMap[f.Prefix] = append(fibRowMap[f.Prefix], table.FibEntry{FaceId: f.FaceId, Cost: f.Cost})
	}
	for prefixStr, entries := range fibRowMap {
		name, err := enc.NameFromTlvStr(prefixStr)
		if err != nil {
			dv.mutex.Unlock()
			return fmt.Errorf("ImportSnapshot: bad fib prefix %q: %w", prefixStr, err)
		}
		dv.fib.Update(name, entries)
	}

	// Restore remote prefix table entries directly so Stage N+1 has a
	// fully-populated prefix table without waiting for SVS re-sync.
	for _, pe := range snap.PfxTable {
		routerName, err := enc.NameFromTlvStr(pe.Router)
		if err != nil {
			continue
		}
		prefixName, err := enc.NameFromTlvStr(pe.Name)
		if err != nil {
			continue
		}
		router := dv.pfx.GetRouter(routerName)
		router.Prefixes[prefixName.TlvStr()] = &table.PrefixEntry{
			Name: prefixName,
			Cost: pe.Cost,
		}
	}

	dv.mutex.Unlock()

	// Restore pfxSvs Known state before SubscribePublisher triggers consumeCheck,
	// so that it sees Pending == Known == Latest and skips re-fetching stage-1 data.
	if len(snap.PfxSvsState) > 0 {
		if err := dv.pfxSvs.ImportInstanceState(enc.Wire{snap.PfxSvsState}); err != nil {
			log.Warn(dv, "ImportSnapshot: pfxSvs state restore failed", "err", err)
		}
	}

	// Rebuild RIB from exported neighbor adverts via DV's own updateRib logic.
	// This ensures that when the first real heartbeat arrives from each neighbor,
	// DirtyResetNextHop + re-add produces the same result as stage-1: no routes
	// are lost regardless of timing. Neighbors without exported adverts retain
	// their routes from the rib rows restored above.
	for _, sn := range snap.Neighbors {
		name, err := enc.NameFromTlvStr(sn.Name)
		if err != nil {
			continue
		}
		advert := &tlv.Advertisement{}
		for _, e := range sn.Entries {
			dest, err2 := enc.NameFromTlvStr(e.Dest)
			if err2 != nil {
				continue
			}
			nh, err2 := enc.NameFromTlvStr(e.NextHop)
			if err2 != nil {
				continue
			}
			advert.Entries = append(advert.Entries, &tlv.AdvEntry{
				Destination: &tlv.Destination{Name: dest},
				NextHop:     &tlv.Destination{Name: nh},
				Cost:        e.Cost,
				OtherCost:   e.OtherCost,
			})
		}
		dv.mutex.Lock()
		realNs := dv.neighbors.Add(name)
		realNs.Advert = advert
		realNs.AdvertBoot = sn.AdvertBoot
		realNs.AdvertSeq = sn.AdvertSeq
		dv.mutex.Unlock()
		dv.updateRib(realNs)
	}

	dv.updatePrefixSubs()
	return nil
}`

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
	// fmt, enc, table, tlv, and log needed for ExportSnapshot/ImportSnapshot.
	astutil.AddImport(fset, file, "fmt")
	addNamedImport(file, "enc", "github.com/named-data/ndnd/std/encoding")
	addNamedImport(file, "table", "github.com/named-data/ndnd/dv/table")
	addNamedImport(file, "tlv", "github.com/named-data/ndnd/dv/tlv")
	addNamedImport(file, "log", "github.com/named-data/ndnd/std/log")
	return true
}
