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
rulePostUpdateRibConvergenceHook              // dv/dv/table_algo.go: append dv.runConvergenceHook() + dv.updatePsdPrefix() to postUpdateRib
rulePrefixEventHooks                          // dv/table prefix announce/add hooks for convergence metrics
// Code-injection rules (eliminate former *_sim.go overlay files).
ruleInjectFibStrategyTreeExtensions   // fw/table/fib-strategy-tree.go (twophase)
ruleInjectFibHashTableExtensions      // fw/table/fib-strategy-hashtable.go
ruleInjectPetConstructor              // fw/table/pet.go
ruleInjectRibSimFunctions             // fw/table/rib.go  (needs _ndndsim import)
ruleInjectSvsSimExtensions            // std/sync/svs.go
ruleSvsDeterministicRng               // std/sync/svs.go: per-instance seeded RNG
ruleInjectSvsAloSimExtensions         // std/sync/svs_alo.go
ruleSvsAloInitialStatePatch            // std/sync/svs_alo_initial_state.go: skip seqNo=0
ruleSvsAloOnSvsUpdatePatch            // std/sync/svs_alo.go: skip fetch for newly discovered publishers
ruleSvsAloConsumeCheckPatch           // std/sync/svs_alo_data.go: guard against Known=0/Latest=1
ruleSvsChannels                       // std/sync/svs.go: recvSv/stop sends → sim methods
ruleSvsAloChannels                    // std/sync/svs_alo.go: stop/publpipe sends → sim methods
ruleInjectSvsAloDataChannels          // std/sync/svs_alo_data.go: outpipe/errpipe sends
ruleSvsAloSnapContent                // std/sync/svs_alo.go: store/restore content in snapshots
ruleSvsAloSnapContentData            // std/sync/svs_alo_data.go: store content in produceObject
ruleSvsAloSnapContentState          // std/sync/svs_alo_initial_state.go: encode/decode content in snapshots
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
		"router.go":      {ruleKeychainNewKeyChain, ruleInjectRouterSimExtensionsOp, ruleSvsALOMaxPipelineSim},
		"advert_data.go": {ruleDvAdvReceiptCallback},
	}),
pkg("dv/nfdc", map[string][]fileRule{
"nfdc.go": {ruleNfdcChannelSend, ruleInjectNfdcSimExtensions},
}),
pkg("std/sync", map[string][]fileRule{
"svs.go":          {ruleSimTicker, ruleSvsDeterministicRng, ruleInjectSvsSimExtensions, ruleSvsChannels},
"svs_alo.go":      {ruleSvsAloChannels, ruleInjectSvsAloSimExtensions, ruleSvsAloSnapContent},
"svs_alo_initial_state.go": {ruleSvsAloInitialStatePatch, ruleSvsAloSnapContentState},
"svs_alo_data.go":         {ruleInjectSvsAloDataChannels, ruleSvsAloSnapContentData},
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
		"svs_alo.go":      {ruleSvsAloChannels, ruleInjectSvsAloSimExtensions, ruleSvsAloOnSvsUpdatePatch, ruleSvsAloSnapContent},
		"svs_alo_initial_state.go": {ruleSvsAloInitialStatePatch, ruleSvsAloSnapContentState},
			"svs_alo_data.go":         {ruleInjectSvsAloDataChannels, ruleSvsAloConsumeCheckPatch, ruleSvsAloSnapContentData},
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
			modified = applySvsAloInitialStatePatch(file, fset) || modified
			modified = applySvsAloSimExtensions(file, fset) || modified

		case ruleSvsChannels:
			modified = applySvsChannels(file) || modified

		case ruleSvsAloChannels:
			modified = applySvsAloChannels(file) || modified

		case ruleSvsAloInitialStatePatch:
			modified = applySvsAloInitialStatePatch(file, fset) || modified

		case ruleSvsAloOnSvsUpdatePatch:
			modified = applySvsAloOnSvsUpdatePatch(file) || modified

		case ruleSvsAloConsumeCheckPatch:
			// Disabled: guard breaks normal operation. Content storage should be the real fix.
			// modified = applySvsAloConsumeCheckPatch(file) || modified

		case ruleInjectSvsAloDataChannels:
			modified = applySvsAloDataChannels(file) || modified

		case ruleSvsAloSnapContentData:
			modified = applySvsAloStoreContentData(file, fset) || modified

		case ruleSvsAloSnapContentState:
			modified = applySvsAloSnapContentState(file, fset) || modified

		case ruleSvsAloSnapContent:
			modified = applySvsAloSnapContentStruct(file, fset) || modified
			modified = applySvsAloSnapContentHelpers(file, fset) || modified
			modified = applySvsAloSnapContentInit(file, fset) || modified



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
