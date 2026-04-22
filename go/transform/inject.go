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
