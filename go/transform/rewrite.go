// rewrite.go: orchestration for ndnd AST transformation.
//
// This file owns the rule taxonomy (fileRule constants), the target-package
// table (targetRewrites), and the file-level dispatch loop (rewriteFile).
// The actual rule implementations live in:
//   - rules.go   – pure AST rewrite rules (no code injection)
//   - inject.go  – code-injection rules (injectDecls + snippet constants)
//   - helpers.go – shared utilities (injectDecls, addNamedImport, pruneUnusedPackageImports)
package main

import (
	"go/format"
	"go/parser"
	"go/token"
	"os"
	"path/filepath"
	"strings"
)

// packageRewrite describes the rewrite rules for a single package directory.
type packageRewrite struct {
pkgDir    string // absolute path to the package inside the output tree
simModule string // import path for the ndndsim shim package

// Per-file rules keyed on base filename (e.g. "nfdc.go").
fileRules map[string][]fileRule
}

// fileRule is an additional rule applied only to a specific file.
type fileRule int

const (
// Simple AST-substitution rules.
ruleNfdcChannelSend          fileRule = iota // m.channel <- cmd  →  m.simExec(cmd)
ruleKeychainNewKeyChain                       // keychain.NewKeyChain(…) → simNewKeyChain(…)
ruleFaceEventsGuard                           // prepend guard to startFaceEvents()
rulePetGlobalPointer                          // var Pet = PrefixEgressTable{…} → var Pet = NewPrefixEgressTable()
ruleSimTicker                                 // *time.Ticker field → *simTicker; time.NewTicker → newSimTicker
ruleStorageNewMemoryStore                     // storage.NewMemoryStore() → simNewStore()
ruleFibGlobalPointerInternal                  // FibStrategyTable.Foo → simFib().Foo  (within fw/table pkg)
ruleDeadNonceListMutex                        // add sync.RWMutex to DeadNonceList
rulePostUpdateRibConvergenceHook              // dv/dv/table_algo.go: append dv.runConvergenceHook() to postUpdateRib
rulePrefixEventHooks                          // dv/table prefix announce/add hooks for convergence metrics
ruleSnapGraceGuard                            // dv/dv/table_algo.go: guard UnsubscribePublisher with _snapGraceActive check
	ruleSnapGraceFibGuard                         // dv/dv/table_algo.go: guard dv.fib.RemoveUnmarked() with _snapGraceActive check
	ruleSnapGraceRibPruneGuard                    // dv/dv/table_algo.go: guard dv.rib.DirtyResetNextHop() in updateRib() with _snapGraceActive check
// Code-injection rules (eliminate former *_sim.go overlay files).
ruleInjectFibStrategyTreeExtensions   // fw/table/fib-strategy-tree.go (twophase)
ruleInjectFibHashTableExtensions      // fw/table/fib-strategy-hashtable.go
ruleInjectPetConstructor              // fw/table/pet.go
ruleInjectRibSimFunctions             // fw/table/rib.go  (needs _ndndsim import)
ruleInjectSvsSimExtensions            // std/sync/svs.go
ruleSvsDeterministicRng               // std/sync/svs.go: per-instance seeded RNG
ruleInjectSvsAloSimExtensions         // std/sync/svs_alo.go
ruleSvsChannels                       // std/sync/svs.go: recvSv/stop sends → sim methods
ruleSvsAloChannels                    // std/sync/svs_alo.go: stop/publpipe sends → sim methods
ruleInjectSvsAloDataChannels          // std/sync/svs_alo_data.go: outpipe/errpipe sends
ruleInjectThreadSimExtensions         // fw/fw/thread.go (twophase: includes simPet)
ruleInjectNfdcSimExtensions           // dv/nfdc/nfdc.go  (needs _ndndsim import)
ruleInjectPrefixSimExtensions         // dv/dv/prefix.go
ruleInjectRouterSimExtensions         // dv/dv/router.go (twophase: pfx.Start)
ruleSvsALOMaxPipelineSim              // dv/dv/prefix.go (twophase) + dv/dv/router.go (onephase)
ruleDvAdvReceiptCallback              // dv/dv/advert_data.go: stamp in-flight DV adv receipts
rulePfxSvsDeliveryCallback           // dv/dv/prefix.go: stamp in-flight prefix SVS deliveries
	rulePfxSvsDeliveryCallbackOp        // dv/dv/table_algo.go (onephase): stamp in-flight prefix SVS deliveries
	rulePfxSvsDeliveryCallbackInApplyOp // dv/table/prefix_table.go (onephase): stamp in-flight prefix SVS deliveries in Apply

// Onephase variants (named-data/ndnd@main@51774b8: no PET, no prefix.go,
// no MulticastStrategyTable, different router.go pfxSvs API).
ruleInjectFibStrategyTreeExtensionsOp // fw/table/fib-strategy-tree.go (onephase)
ruleInjectThreadSimExtensionsOp       // fw/fw/thread.go (onephase: no simPet/simMulticastFib)
ruleInjectRouterSimExtensionsOp       // dv/dv/router.go (onephase: pfxSvs.Start)
	ruleDisablePfxSvsSnapshot             // dv/dv/router.go (onephase): SnapshotNodeLatest → SnapshotNull
	ruleSnapshotDisableInSim              // std/sync/svs_alo_data.go: skip onUpdate in sim
	ruleSnapshotEvictionDisableInSim      // std/sync/snapshot_node_latest.go: skip RemoveFlatRange eviction in sim
)

// targetRewrites returns the full set of package-level rewrite descriptors.
// phase must be "twophase" (named-data/ndnd@dv2) or "onephase" (named-data/ndnd@main@51774b8).
func targetRewrites(outDir, simModule, phase string) []packageRewrite {
pkg := func(rel string, fileRules map[string][]fileRule) packageRewrite {
return packageRewrite{
pkgDir:    filepath.Join(outDir, filepath.FromSlash(rel)),
simModule: simModule,
fileRules: fileRules,
}
}

if phase == "onephase" {
// Onephase (named-data/ndnd@main@51774b8):
//   • No fw/table/pet.go                  → skip pet rules
//   • No dv/dv/prefix.go                  → skip prefix rules
//   • No table.MulticastStrategyTable     → simpler thread snippet
//   • fw/table/fib-strategy-tree.go uses direct struct construction
//   • dv/dv/router.go uses pfxSvs.Start() instead of pfx.Start()
return []packageRewrite{
pkg("fw/fw", map[string][]fileRule{
"thread.go": {ruleInjectThreadSimExtensionsOp},
}),
pkg("fw/table", map[string][]fileRule{
"fib-strategy-tree.go":      {ruleInjectFibStrategyTreeExtensionsOp},
"fib-strategy-hashtable.go": {ruleInjectFibHashTableExtensions},
"rib.go":                    {ruleFibGlobalPointerInternal, ruleInjectRibSimFunctions},
"dead-nonce-list.go":        {ruleDeadNonceListMutex},
}),
// dv/table: apply global rewrites (time.Now→_ndndsim.Now,
// time.Since→_ndndsim.Now().Sub, go→_ndndsim.Go) to all files.
// Fixes lastSeen tracking and IsDead() in neighbor_table.go.
pkg("dv/table", map[string][]fileRule{}),
pkg("dv/table", map[string][]fileRule{
"prefix_table.go": {rulePrefixEventHooks, rulePfxSvsDeliveryCallbackInApplyOp},
}),
pkg("dv/dv", map[string][]fileRule{
		"table_algo.go":  {rulePostUpdateRibConvergenceHook, rulePfxSvsDeliveryCallbackOp},
		"router.go":      {ruleKeychainNewKeyChain, ruleInjectRouterSimExtensionsOp, ruleDisablePfxSvsSnapshot, ruleSvsALOMaxPipelineSim},
		"advert_data.go": {ruleDvAdvReceiptCallback},
	}),
pkg("dv/nfdc", map[string][]fileRule{
"nfdc.go": {ruleNfdcChannelSend, ruleInjectNfdcSimExtensions},
}),
pkg("std/sync", map[string][]fileRule{
"svs.go":          {ruleSimTicker, ruleSvsDeterministicRng, ruleInjectSvsSimExtensions, ruleSvsChannels},
"svs_alo.go":      {ruleSvsAloChannels, ruleInjectSvsAloSimExtensions},
"svs_alo_data.go":         {ruleInjectSvsAloDataChannels},
		"snapshot_node_latest.go": {ruleSnapshotEvictionDisableInSim},
}),
}
}

// Twophase (named-data/ndnd@dv2@76aeb89c) — default.
return []packageRewrite{
// fw/fw: inject Thread sim helpers (eliminates thread_sim.go overlay).
pkg("fw/fw", map[string][]fileRule{
"thread.go": {ruleInjectThreadSimExtensions},
}),
// fw/table: inject sim constructors and safety fixes (eliminates fib_sim.go,
// pet_sim.go, and rib_sim.go overlays).
pkg("fw/table", map[string][]fileRule{
"fib-strategy-tree.go":      {ruleInjectFibStrategyTreeExtensions},
"fib-strategy-hashtable.go": {ruleInjectFibHashTableExtensions},
"pet.go":                    {rulePetGlobalPointer, ruleInjectPetConstructor},
"rib.go":                    {ruleFibGlobalPointerInternal, ruleInjectRibSimFunctions},
"dead-nonce-list.go":        {ruleDeadNonceListMutex},
}),
// dv/table: apply global rewrites (time.Now→_ndndsim.Now,
// time.Since→_ndndsim.Now().Sub, go→_ndndsim.Go) to all files.
// Fixes lastSeen tracking and IsDead() in neighbor_table.go.
pkg("dv/table", map[string][]fileRule{}),
pkg("dv/table", map[string][]fileRule{
"prefix_state.go": {rulePrefixEventHooks},
}),
// dv/dv: inject Router and PrefixModule sim methods (eliminates router_sim.go
// and prefix_sim.go overlays).
pkg("dv/dv", map[string][]fileRule{
		"table_algo.go": {rulePostUpdateRibConvergenceHook},
"router.go": {ruleKeychainNewKeyChain, ruleInjectRouterSimExtensions},
"prefix.go": {ruleFaceEventsGuard, ruleInjectPrefixSimExtensions, ruleSvsALOMaxPipelineSim, rulePfxSvsDeliveryCallback},
"advert_data.go": {ruleDvAdvReceiptCallback},
}),
// dv/nfdc: inject simExec (eliminates nfdc_sim.go overlay).
pkg("dv/nfdc", map[string][]fileRule{
"nfdc.go": {ruleNfdcChannelSend, ruleInjectNfdcSimExtensions},
}),
// std/sync: eliminate goroutines via direct-delivery sim helpers.
	pkg("std/sync", map[string][]fileRule{
		"svs.go":          {ruleSimTicker, ruleSvsDeterministicRng, ruleInjectSvsSimExtensions, ruleSvsChannels},
		"svs_alo.go":      {ruleSvsAloChannels, ruleInjectSvsAloSimExtensions},
			"svs_alo_data.go":         {ruleInjectSvsAloDataChannels, ruleSnapshotDisableInSim},
			"snapshot_node_latest.go": {ruleSnapshotEvictionDisableInSim},
}),
}
}

func rewritePackage(r packageRewrite) error {
entries, err := os.ReadDir(r.pkgDir)
if err != nil {
return err
}
for _, e := range entries {
if e.IsDir() || !strings.HasSuffix(e.Name(), ".go") || strings.HasSuffix(e.Name(), "_test.go") {
continue
}
fpath := filepath.Join(r.pkgDir, e.Name())
if err := rewriteFile(fpath, r.simModule, r.fileRules[e.Name()]); err != nil {
return err
}
}
return nil
}

// rewriteFile parses, rewrites, and writes back a single Go source file.
func rewriteFile(fpath, simModule string, extraRules []fileRule) error {
fset := token.NewFileSet()
file, err := parser.ParseFile(fset, fpath, nil, parser.ParseComments)
if err != nil {
return err
}

// Apply global rewrites (go→_ndndsim.Go, time.Now→_ndndsim.Now, table.Pet→simPet, …).
modified, ndndsimUsed := applyGlobalRewrites(file)

// Apply per-file rules.
for _, rule := range extraRules {
switch rule {

// --- Simple AST-substitution rules ---

case ruleNfdcChannelSend:
// simExec is package-local; no _ndndsim import needed.
modified = applyNfdcChannelSend(file) || modified

case ruleKeychainNewKeyChain:
// simNewKeyChain is package-local; no _ndndsim import needed.
modified = applyKeychainReplace(file) || modified

case ruleFaceEventsGuard:
// Injects _ndndsim.EnableFaceEvents() call.
if applyFaceEventsGuard(file, simModule) {
modified = true
ndndsimUsed = true
}

case rulePetGlobalPointer:
modified = applyPetGlobalPointer(file) || modified

case ruleSimTicker:
// newSimTicker is package-local; no _ndndsim import needed here.
modified = applySimTicker(file) || modified

case ruleStorageNewMemoryStore:
// simNewStore is package-local; no _ndndsim import needed.
modified = applyStorageNewMemoryStore(file) || modified

case ruleFibGlobalPointerInternal:
// simFib is package-local; no _ndndsim import needed.
modified = applyFibGlobalPointerInternal(file) || modified

case ruleDeadNonceListMutex:
modified = applyDeadNonceListMutex(file, fset) || modified

		case rulePostUpdateRibConvergenceHook:
			modified = applyPostUpdateRibConvergenceHook(file) || modified

		case rulePrefixEventHooks:
			if applyPrefixEventHooks(file) {
				modified = true
				ndndsimUsed = true
			}

// --- Code-injection rules ---

case ruleInjectFibStrategyTreeExtensions:
modified = applyFibStrategyTreeExtensions(file, fset) || modified

case ruleInjectFibHashTableExtensions:
modified = applyFibHashTableExtensions(file, fset) || modified

case ruleInjectPetConstructor:
modified = applyPetConstructor(file, fset) || modified

case ruleInjectRibSimFunctions:
// Injected simFib() calls _ndndsim.GetHooks().
if applyRibSimExtensions(file, fset) {
modified = true
ndndsimUsed = true
}

case ruleInjectSvsSimExtensions:
// _ndndsim, time, sync/atomic already imported in svs.go.
modified = applySvsSimExtensions(file, fset) || modified

		case ruleSvsDeterministicRng:
			modified = applySvsDeterministicRNG(file) || modified
		case ruleInjectSvsAloSimExtensions:
			modified = applySvsAloSimExtensions(file, fset) || modified

		case ruleSvsChannels:
			modified = applySvsChannels(file) || modified

		case ruleSvsAloChannels:
			modified = applySvsAloChannels(file) || modified

		case ruleInjectSvsAloDataChannels:
			modified = applySvsAloDataChannels(file) || modified

		case ruleSnapshotDisableInSim:
			// Wrap s.opts.Snapshot.onUpdate(...) with if !_ndndsim.IsSynchronous()
			// to disable the snapshot strategy in sim mode.  Without this,
			// SnapshotNodeLatest.onUpdate sets SnapBlock=1 on every consumeCheck
			// call when Known==0, blocking normal sequential fetching forever.
			if applySnapshotDisableInSim(file) {
				modified = true
				ndndsimUsed = true
			}

		case ruleSnapshotEvictionDisableInSim:
			// Wrap the RemoveFlatRange eviction block in takeSnap with
			// if !_ndndsim.IsSynchronous() so it is skipped in sim mode.
			// MemoryStore has unlimited capacity; eviction is not needed and
			// breaks the 3-stage scenario where all announcements happen at t=0.
			if applySnapshotEvictionDisableInSim(file) {
				modified = true
				ndndsimUsed = true
			}

		case ruleInjectThreadSimExtensions:
// _ndndsim, defn, table already in thread.go; sync added inside.
modified = applyThreadSimExtensions(file, fset) || modified

case ruleInjectNfdcSimExtensions:
// Injected simExec() calls _ndndsim.IsSynchronous().
if applyNfdcSimExtensions(file, fset) {
modified = true
ndndsimUsed = true
}

case ruleInjectPrefixSimExtensions:
// ndn_sync already imported in prefix.go.
modified = applyPrefixSimExtensions(file, fset) || modified

case ruleInjectRouterSimExtensions:
// _ndndsim already in router.go from global go→_ndndsim.Go rule.
// ndn_sync is added inside applyRouterSimExtensions.
modified = applyRouterSimExtensions(file, fset) || modified

case ruleDvAdvReceiptCallback:
// Inject ndndsim.NdndsimRecordDvAdvReceipt() in advert_data.go dataHandler.
if applyDvAdvReceiptCallback(file) {
modified = true
ndndsimUsed = true
}

case rulePfxSvsDeliveryCallback:
// Inject ndndsim.NdndsimRecordPfxSvsDelivery() in prefix.go SubscribePublisher callback.
if applyPfxSvsDeliveryCallback(file) {
modified = true
ndndsimUsed = true
}

case rulePfxSvsDeliveryCallbackOp:
// Inject ndndsim.NdndsimRecordPfxSvsDelivery() in onephase table_algo.go SubscribePublisher callback.
if applyPfxSvsDeliveryCallbackOp(file) {
modified = true
ndndsimUsed = true
}

case rulePfxSvsDeliveryCallbackInApplyOp:
// Inject ndndsim.NdndsimRecordPfxSvsDelivery() in onephase prefix_table.go Apply function.
if applyPfxSvsDeliveryCallbackInApplyOp(file) {
modified = true
ndndsimUsed = true
}

// --- Onephase code-injection rules ---

case ruleInjectFibStrategyTreeExtensionsOp:
// Onephase: direct struct construction (no newStrategyTableTree helper).
modified = applyFibStrategyTreeExtensionsOp(file, fset) || modified

case ruleInjectThreadSimExtensionsOp:
// Onephase: no simPet / simMulticastFib (types don't exist at 51774b8).
modified = applyThreadSimExtensionsOp(file, fset) || modified

case ruleInjectRouterSimExtensionsOp:
// Onephase: uses pfxSvs.Start/Stop instead of pfx.Start/Stop.
modified = applyRouterSimExtensionsOp(file, fset) || modified

		case ruleDisablePfxSvsSnapshot:
			// Onephase: replace SnapshotNodeLatest with SnapshotNull in
			// createPrefixTable so the SVS-internal snapshot is fully disabled.
			// The sim uses its own stage1→snap→stage2 mechanism instead.
			modified = applyDisablePfxSvsSnapshot(file) || modified

		case ruleSvsALOMaxPipelineSim:
			// Both phases: inject MaxPipelineSize: _ndndsim.SvsMaxPipelineSize()
			// into the SvsAloOpts used for prefix sync so all objects are fetched
			// in a single RTT batch during simulation.
			// _ndndsim is already imported in both prefix.go (twophase) and
			// router.go (onephase) by the global go→_ndndsim.Go rewrite.
			if applySetSvsALOMaxPipeline(file) {
				modified = true
				ndndsimUsed = true
			}
		}
	}

	if !modified {
		return nil
	}
	if ndndsimUsed {
		addNamedImport(file, "_ndndsim", simModule)
	}
	pruneUnusedPackageImports(fset, file)

	out, err := os.Create(fpath)
	if err != nil {
		return err
	}
	defer out.Close()
	return format.Node(out, fset, file)
}
