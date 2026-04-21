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

// Code-injection rules (eliminate former *_sim.go overlay files).
ruleInjectFibStrategyTreeExtensions // fw/table/fib-strategy-tree.go
ruleInjectFibHashTableExtensions    // fw/table/fib-strategy-hashtable.go
ruleInjectPetConstructor            // fw/table/pet.go
ruleInjectRibSimFunctions           // fw/table/rib.go  (needs _ndndsim import)
ruleInjectSvsSimExtensions          // std/sync/svs.go
ruleInjectThreadSimExtensions       // fw/fw/thread.go
ruleInjectNfdcSimExtensions         // dv/nfdc/nfdc.go  (needs _ndndsim import)
ruleInjectPrefixSimExtensions       // dv/dv/prefix.go
ruleInjectRouterSimExtensions       // dv/dv/router.go
)

// targetRewrites returns the full set of package-level rewrite descriptors.
func targetRewrites(outDir, simModule string) []packageRewrite {
pkg := func(rel string, fileRules map[string][]fileRule) packageRewrite {
return packageRewrite{
pkgDir:    filepath.Join(outDir, filepath.FromSlash(rel)),
simModule: simModule,
fileRules: fileRules,
}
}
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
// dv/dv: inject Router and PrefixModule sim methods (eliminates router_sim.go
// and prefix_sim.go overlays).
pkg("dv/dv", map[string][]fileRule{
"router.go": {ruleKeychainNewKeyChain, ruleInjectRouterSimExtensions},
"prefix.go": {ruleFaceEventsGuard, ruleInjectPrefixSimExtensions},
}),
// dv/nfdc: inject simExec (eliminates nfdc_sim.go overlay).
pkg("dv/nfdc", map[string][]fileRule{
"nfdc.go": {ruleNfdcChannelSend, ruleInjectNfdcSimExtensions},
}),
// std/sync: inject simTicker and SuppressStats (eliminates ticker_sim.go
// and suppress_sim.go overlays).
pkg("std/sync", map[string][]fileRule{
"svs.go": {ruleSimTicker, ruleInjectSvsSimExtensions},
}),
}
}

// rewritePackage applies all relevant rewrites to every non-test Go file in r.pkgDir.
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


