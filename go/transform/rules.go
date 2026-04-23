package main

import (
	"go/ast"
	"go/token"

	"golang.org/x/tools/go/ast/astutil"
)

// applyGlobalRewrites applies AST rewrites that run across every Go file in
// every target package:
//
//   - go stmt                       → _ndndsim.Go(func() { … })
//   - time.Now() / time.AfterFunc() → _ndndsim.Now() / _ndndsim.AfterFunc()
//   - &table.Pet                    → simPet()
//   - table.FibStrategyTable        → simFib()
//   - table.MulticastStrategyTable  → simMulticastFib()
//   - table.Pet                     → simPet()
//
// It returns whether any change was made and whether _ndndsim call sites
// were introduced (so the caller can insert the named import).
func applyGlobalRewrites(file *ast.File) (modified, ndndsimUsed bool) {
	astutil.Apply(file, func(c *astutil.Cursor) bool {
		switch n := c.Node().(type) {

		// go stmt → _ndndsim.Go(func() { … })
		case *ast.GoStmt:
			c.Replace(makeGoCall(n.Call))
			modified = true
			ndndsimUsed = true

		// &table.Pet → simPet()
		// Must be matched before the SelectorExpr case so the whole UnaryExpr
		// is replaced rather than leaving a dangling & operator.
		case *ast.UnaryExpr:
			if n.Op != token.AND {
				break
			}
			sel, ok := n.X.(*ast.SelectorExpr)
			if !ok {
				break
			}
			pkg, ok := sel.X.(*ast.Ident)
			if !ok || pkg.Name != "table" || sel.Sel.Name != "Pet" {
				break
			}
			c.Replace(&ast.CallExpr{Fun: ast.NewIdent("simPet")})
			modified = true

		// time.Now()       → _ndndsim.Now()
		// time.AfterFunc()  → _ndndsim.AfterFunc()
		// time.Sleep(d)    → _ndndsim.Sleep(d)
		// time.Since(t)    → _ndndsim.Now().Sub(t)
		//   time.Since(t) is sugar for time.Now().Sub(t); replacing it here
		//   ensures both the Now() call and the subtraction use the sim clock.
		case *ast.CallExpr:
			sel, ok := n.Fun.(*ast.SelectorExpr)
			if !ok {
				break
			}
			pkg, ok := sel.X.(*ast.Ident)
			if !ok || pkg.Name != "time" {
				break
			}
			switch sel.Sel.Name {
			case "Now", "AfterFunc", "Sleep":
				sel.X = ast.NewIdent("_ndndsim")
				modified = true
				ndndsimUsed = true
			case "Since":
				// Replace time.Since(t) with _ndndsim.Now().Sub(t)
				if len(n.Args) == 1 {
					c.Replace(&ast.CallExpr{
						Fun: &ast.SelectorExpr{
							X: &ast.CallExpr{
								Fun: &ast.SelectorExpr{
									X:   ast.NewIdent("_ndndsim"),
									Sel: ast.NewIdent("Now"),
								},
							},
							Sel: ast.NewIdent("Sub"),
						},
						Args: n.Args,
					})
					modified = true
					ndndsimUsed = true
				}
			}

		// table.FibStrategyTable      → simFib()
		// table.MulticastStrategyTable → simMulticastFib()
		// table.Pet                   → simPet()
		// (simFib/simPet are package-local; no _ndndsim import needed)
		case *ast.SelectorExpr:
			pkg, ok := n.X.(*ast.Ident)
			if !ok || pkg.Name != "table" {
				break
			}
			var name string
			switch n.Sel.Name {
			case "FibStrategyTable":
				name = "simFib"
			case "MulticastStrategyTable":
				name = "simMulticastFib"
			case "Pet":
				name = "simPet"
			}
			if name != "" {
				c.Replace(&ast.CallExpr{Fun: ast.NewIdent(name)})
				modified = true
			}
		}
		return true
	}, nil)
	return
}

// ---------------------------------------------------------------------------
// Rule: GoStmt → _ndndsim.Go(func() { … })
// ---------------------------------------------------------------------------

// isLongLivedCall returns true when call is a known long-lived blocking-loop
// that cannot be routed through GoFunc=clock.Schedule.
// Note: "main" (SvSync) and "run" (SvsALO) are handled separately in
// makeGoCall — they generate if/else that skips the loop entirely in sim mode.
func isLongLivedCall(call *ast.CallExpr) bool {
	return false // all cases now handled in makeGoCall or inject.go
}

// bodyContainsInfiniteForLoop returns true if the function literal's body
// contains at least one *ast.ForStmt with a nil Cond (i.e., "for { … }").
// Such goroutines run forever and cannot be scheduled as clock events in DES.
func bodyContainsInfiniteForLoop(fl *ast.FuncLit) bool {
	found := false
	ast.Inspect(fl.Body, func(n ast.Node) bool {
		if found {
			return false
		}
		if fs, ok := n.(*ast.ForStmt); ok && fs.Cond == nil {
			found = true
		}
		return true
	})
	return found
}

// makeGoCall returns the statement to replace a GoStmt.
//
//   - For short-lived goroutines: _ndndsim.Go(func() { … })
//     Go() routes through GoFunc=clock.Schedule in sim, so no real goroutine.
//
//   - For s.main() (SvSync blocking loop):
//     if _ndndsim.IsSynchronous() { s.simStart() } else { go s.main() }
//     simStart() sets up timer/state without running a goroutine.
//
//   - For s.run() (SvsALO blocking loop):
//     if !_ndndsim.IsSynchronous() { go s.run() }
//     In sim, delivery is handled by simQueuePub/simQueueError/simQueuePubl.
//
//   - For IIFEs whose body contains an infinite for loop (for { … }):
//     if !_ndndsim.IsSynchronous() { go func(…) { for { … } }(…) }
//     Scheduling a blocking loop as a clock event would deadlock the DES clock.
//     Such goroutines are simply skipped in sim mode (e.g. startPrefixPrune).
func makeGoCall(call *ast.CallExpr) ast.Node {
	var funcLit *ast.FuncLit

	// Unwrap IIFE with no parameters: go func() { body }() → _ndndsim.Go(func() { body })
	if fl, ok := call.Fun.(*ast.FuncLit); ok && len(call.Args) == 0 &&
		(fl.Type.Params == nil || len(fl.Type.Params.List) == 0) {
		funcLit = fl
	} else {
		funcLit = &ast.FuncLit{
			Type: &ast.FuncType{Params: &ast.FieldList{}},
			Body: &ast.BlockStmt{List: []ast.Stmt{&ast.ExprStmt{X: call}}},
		}
	}

	// Determine the "inner" call — either the original call (non-IIFE) or
	// the single statement inside the unwrapped function literal (IIFE).
	innerCall := call
	if funcLit != nil && len(funcLit.Body.List) == 1 {
		if es, ok := funcLit.Body.List[0].(*ast.ExprStmt); ok {
			if ic, ok := es.X.(*ast.CallExpr); ok {
				innerCall = ic
			}
		}
	}

	// Detect long-lived blocking loops by method name.
	if sel, ok := innerCall.Fun.(*ast.SelectorExpr); ok {
		switch sel.Sel.Name {
		case "main":
			// SvSync.main() — in sim, call simStart() instead of running the loop.
			// if _ndndsim.IsSynchronous() { recv.simStart() } else { go recv.main() }
			return &ast.IfStmt{
				Cond: isSynchronousCallExpr(),
				Body: &ast.BlockStmt{List: []ast.Stmt{
					&ast.ExprStmt{X: &ast.CallExpr{
						Fun: &ast.SelectorExpr{X: sel.X, Sel: ast.NewIdent("simStart")},
					}},
				}},
				Else: &ast.BlockStmt{List: []ast.Stmt{
					&ast.GoStmt{Call: innerCall},
				}},
			}
		case "run":
			// SvsALO.run() — in sim, delivery is done via simQueue* direct calls.
			// if !_ndndsim.IsSynchronous() { go recv.run() }
			return &ast.IfStmt{
				Cond: &ast.UnaryExpr{Op: token.NOT, X: isSynchronousCallExpr()},
				Body: &ast.BlockStmt{List: []ast.Stmt{
					&ast.GoStmt{Call: innerCall},
				}},
			}
		}
	}

	// Detect IIFE goroutines (with or without parameters) whose body contains
	// an infinite for loop (for { … }).  Scheduling such a blocking loop via
	// GoFunc=clock.Schedule would run it synchronously inside Advance() and
	// deadlock the DES clock.  Skip them entirely in sim mode instead.
	if fl, ok := call.Fun.(*ast.FuncLit); ok && bodyContainsInfiniteForLoop(fl) {
		return &ast.IfStmt{
			Cond: &ast.UnaryExpr{Op: token.NOT, X: isSynchronousCallExpr()},
			Body: &ast.BlockStmt{List: []ast.Stmt{
				&ast.GoStmt{Call: call},
			}},
		}
	}

	// Short-lived goroutine: route through GoFunc (clock.Schedule in sim, go f() in prod).
	return &ast.ExprStmt{
		X: &ast.CallExpr{
			Fun: &ast.SelectorExpr{
				X:   ast.NewIdent("_ndndsim"),
				Sel: ast.NewIdent("Go"),
			},
			Args: []ast.Expr{funcLit},
		},
	}
}

// isSynchronousCallExpr returns the AST for _ndndsim.IsSynchronous().
func isSynchronousCallExpr() *ast.CallExpr {
	return &ast.CallExpr{
		Fun: &ast.SelectorExpr{
			X:   ast.NewIdent("_ndndsim"),
			Sel: ast.NewIdent("IsSynchronous"),
		},
	}
}

// ---------------------------------------------------------------------------
// Rule: m.channel <- cmd  →  m.simExec(cmd)   (nfdc.go only)
// ---------------------------------------------------------------------------

func applyNfdcChannelSend(file *ast.File) bool {
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
		recv, ok := sel.X.(*ast.Ident)
		if !ok || recv.Name != "m" || sel.Sel.Name != "channel" {
			return true
		}
		c.Replace(&ast.ExprStmt{
			X: &ast.CallExpr{
				Fun: &ast.SelectorExpr{
					X:   ast.NewIdent("m"),
					Sel: ast.NewIdent("simExec"),
				},
				Args: []ast.Expr{send.Value},
			},
		})
		modified = true
		return true
	}, nil)
	return modified
}

// ---------------------------------------------------------------------------
// Rule: keychain.NewKeyChain(…) → simNewKeyChain(…)   (router.go only)
// ---------------------------------------------------------------------------

func applyKeychainReplace(file *ast.File) bool {
	modified := false
	astutil.Apply(file, func(c *astutil.Cursor) bool {
		call, ok := c.Node().(*ast.CallExpr)
		if !ok {
			return true
		}
		sel, ok := call.Fun.(*ast.SelectorExpr)
		if !ok {
			return true
		}
		pkg, ok := sel.X.(*ast.Ident)
		if !ok || pkg.Name != "keychain" || sel.Sel.Name != "NewKeyChain" {
			return true
		}
		call.Fun = ast.NewIdent("simNewKeyChain")
		modified = true
		return true
	}, nil)
	return modified
}

// ---------------------------------------------------------------------------
// Rule: inject EnableFaceEvents guard at the top of startFaceEvents()
// ---------------------------------------------------------------------------

// applyFaceEventsGuard prepends an early-return guard to startFaceEvents so
// that face-event registration is skipped when the sim disables it.
// simModule is accepted for forward-compatibility but is not currently used.
func applyFaceEventsGuard(file *ast.File, simModule string) bool {
	for _, decl := range file.Decls {
		fd, ok := decl.(*ast.FuncDecl)
		if !ok || fd.Name.Name != "startFaceEvents" || fd.Body == nil {
			continue
		}
		guard := &ast.IfStmt{
			Cond: &ast.UnaryExpr{
				Op: token.NOT,
				X: &ast.CallExpr{
					Fun: &ast.SelectorExpr{
						X:   ast.NewIdent("_ndndsim"),
						Sel: ast.NewIdent("EnableFaceEvents"),
					},
				},
			},
			Body: &ast.BlockStmt{List: []ast.Stmt{&ast.ReturnStmt{}}},
		}
		fd.Body.List = append([]ast.Stmt{guard}, fd.Body.List...)
		return true
	}
	return false
}

// ---------------------------------------------------------------------------
// Rule: var Pet = PrefixEgressTable{…} → var Pet = NewPrefixEgressTable()
// ---------------------------------------------------------------------------

// applyPetGlobalPointer transforms the package-level Pet variable from a
// value to a pointer so that sim/forwarder.go can swap it per-node.
func applyPetGlobalPointer(file *ast.File) bool {
	for _, decl := range file.Decls {
		gd, ok := decl.(*ast.GenDecl)
		if !ok || gd.Tok != token.VAR {
			continue
		}
		for _, spec := range gd.Specs {
			vs, ok := spec.(*ast.ValueSpec)
			if !ok || len(vs.Names) == 0 || vs.Names[0].Name != "Pet" || len(vs.Values) == 0 {
				continue
			}
			vs.Values[0] = &ast.CallExpr{Fun: ast.NewIdent("NewPrefixEgressTable")}
			return true
		}
	}
	return false
}

// ---------------------------------------------------------------------------
// Rule: *time.Ticker field → *simTicker; time.NewTicker(x) → newSimTicker(x)
// ---------------------------------------------------------------------------

// applySimTicker rewrites the SvSync struct's ticker field from *time.Ticker
// to *simTicker, and replaces time.NewTicker calls with newSimTicker.
func applySimTicker(file *ast.File) bool {
	modified := false

	// 1. Rewrite struct field type: ticker *time.Ticker → ticker *simTicker
	for _, decl := range file.Decls {
		gd, ok := decl.(*ast.GenDecl)
		if !ok || gd.Tok != token.TYPE {
			continue
		}
		for _, spec := range gd.Specs {
			ts, ok := spec.(*ast.TypeSpec)
			if !ok {
				continue
			}
			st, ok := ts.Type.(*ast.StructType)
			if !ok {
				continue
			}
			for _, field := range st.Fields.List {
				for _, name := range field.Names {
					if name.Name != "ticker" {
						continue
					}
					star, ok := field.Type.(*ast.StarExpr)
					if !ok {
						continue
					}
					sel, ok := star.X.(*ast.SelectorExpr)
					if !ok {
						continue
					}
					pkg, ok := sel.X.(*ast.Ident)
					if !ok || pkg.Name != "time" || sel.Sel.Name != "Ticker" {
						continue
					}
					field.Type = &ast.StarExpr{X: ast.NewIdent("simTicker")}
					modified = true
				}
			}
		}
	}

	// 2. Replace time.NewTicker(x) → newSimTicker(x)
	astutil.Apply(file, func(c *astutil.Cursor) bool {
		call, ok := c.Node().(*ast.CallExpr)
		if !ok {
			return true
		}
		sel, ok := call.Fun.(*ast.SelectorExpr)
		if !ok {
			return true
		}
		pkg, ok := sel.X.(*ast.Ident)
		if !ok || pkg.Name != "time" || sel.Sel.Name != "NewTicker" {
			return true
		}
		call.Fun = ast.NewIdent("newSimTicker")
		modified = true
		return true
	}, nil)

	return modified
}

// ---------------------------------------------------------------------------
// Rule: SvSync uses a per-instance deterministic rand.Rand
// ---------------------------------------------------------------------------

// applySvsDeterministicRNG moves SVS timeout jitter off the process-global RNG
// and onto a per-instance generator seeded from GroupPrefix. This preserves
// deterministic simulation behavior without replacing the upstream file.
func applySvsDeterministicRNG(file *ast.File) bool {
	modified := false

	if addSvSyncRngField(file) {
		modified = true
	}
	if addSvSyncRngInitializer(file) {
		modified = true
	}

	astutil.Apply(file, func(c *astutil.Cursor) bool {
		call, ok := c.Node().(*ast.CallExpr)
		if !ok {
			return true
		}
		sel, ok := call.Fun.(*ast.SelectorExpr)
		if !ok {
			return true
		}
		pkg, ok := sel.X.(*ast.Ident)
		if !ok || pkg.Name != "rand" || sel.Sel.Name != "Int64N" {
			return true
		}
		c.Replace(&ast.CallExpr{
			Fun: &ast.SelectorExpr{
				X: &ast.SelectorExpr{
					X:   ast.NewIdent("s"),
					Sel: ast.NewIdent("rng"),
				},
				Sel: ast.NewIdent("Int64N"),
			},
			Args: call.Args,
		})
		modified = true
		return true
	}, nil)

	return modified
}

func addSvSyncRngField(file *ast.File) bool {
	for _, decl := range file.Decls {
		gd, ok := decl.(*ast.GenDecl)
		if !ok || gd.Tok != token.TYPE {
			continue
		}
		for _, spec := range gd.Specs {
			ts, ok := spec.(*ast.TypeSpec)
			if !ok || ts.Name.Name != "SvSync" {
				continue
			}
			st, ok := ts.Type.(*ast.StructType)
			if !ok {
				continue
			}
			if structHasNamedField(st, "rng") {
				return false
			}
			st.Fields.List = append(st.Fields.List, &ast.Field{
				Names: []*ast.Ident{ast.NewIdent("rng")},
				Type: &ast.StarExpr{X: &ast.SelectorExpr{
					X:   ast.NewIdent("rand"),
					Sel: ast.NewIdent("Rand"),
				}},
			})
			return true
		}
	}
	return false
}

func addSvSyncRngInitializer(file *ast.File) bool {
	lit := findSvSyncConstructorLiteral(file)
	if lit == nil || compositeLiteralHasKey(lit, "rng") {
		return false
	}
	lit.Elts = append(lit.Elts, &ast.KeyValueExpr{
		Key: ast.NewIdent("rng"),
		Value: &ast.CallExpr{
			Fun: &ast.SelectorExpr{
				X:   ast.NewIdent("rand"),
				Sel: ast.NewIdent("New"),
			},
			Args: []ast.Expr{&ast.CallExpr{
				Fun: &ast.SelectorExpr{
					X:   ast.NewIdent("rand"),
					Sel: ast.NewIdent("NewPCG"),
				},
				Args: []ast.Expr{
					&ast.CallExpr{Fun: &ast.SelectorExpr{
						X: &ast.SelectorExpr{
							X:   ast.NewIdent("opts"),
							Sel: ast.NewIdent("GroupPrefix"),
						},
						Sel: ast.NewIdent("Hash"),
					}},
					&ast.BasicLit{Kind: token.INT, Value: "0"},
				},
			}},
		},
	})
	return true
}

func structHasNamedField(st *ast.StructType, name string) bool {
	for _, field := range st.Fields.List {
		for _, fieldName := range field.Names {
			if fieldName.Name == name {
				return true
			}
		}
	}
	return false
}

func findSvSyncConstructorLiteral(file *ast.File) *ast.CompositeLit {
	for _, decl := range file.Decls {
		fd, ok := decl.(*ast.FuncDecl)
		if !ok || fd.Name.Name != "NewSvSync" || fd.Body == nil {
			continue
		}
		var lit *ast.CompositeLit
		ast.Inspect(fd.Body, func(n ast.Node) bool {
			ret, ok := n.(*ast.ReturnStmt)
			if !ok || len(ret.Results) != 1 {
				return true
			}
			lit = svSyncCompositeLiteral(ret.Results[0])
			return lit == nil
		})
		if lit != nil {
			return lit
		}
	}
	return nil
}

func svSyncCompositeLiteral(expr ast.Expr) *ast.CompositeLit {
	switch node := expr.(type) {
	case *ast.UnaryExpr:
		if node.Op == token.AND {
			return svSyncCompositeLiteral(node.X)
		}
	case *ast.CompositeLit:
		if ident, ok := node.Type.(*ast.Ident); ok && ident.Name == "SvSync" {
			return node
		}
	}
	return nil
}

func compositeLiteralHasKey(lit *ast.CompositeLit, name string) bool {
	for _, elt := range lit.Elts {
		kv, ok := elt.(*ast.KeyValueExpr)
		if !ok {
			continue
		}
		ident, ok := kv.Key.(*ast.Ident)
		if ok && ident.Name == name {
			return true
		}
	}
	return false
}

// ---------------------------------------------------------------------------
// Rule: append dv.runConvergenceHook() to postUpdateRib()
// ---------------------------------------------------------------------------

func applyPostUpdateRibConvergenceHook(file *ast.File) bool {
	for _, decl := range file.Decls {
		fd, ok := decl.(*ast.FuncDecl)
		if !ok || fd.Name.Name != "postUpdateRib" || fd.Body == nil {
			continue
		}
		if blockHasSelectorCall(fd.Body, "dv", "runConvergenceHook") {
			return false
		}
		fd.Body.List = append(fd.Body.List, &ast.ExprStmt{X: &ast.CallExpr{
			Fun: &ast.SelectorExpr{X: ast.NewIdent("dv"), Sel: ast.NewIdent("runConvergenceHook")},
		}})
		return true
	}
	return false
}

func blockHasSelectorCall(block *ast.BlockStmt, recvName, methodName string) bool {
	for _, stmt := range block.List {
		es, ok := stmt.(*ast.ExprStmt)
		if !ok {
			continue
		}
		call, ok := es.X.(*ast.CallExpr)
		if !ok {
			continue
		}
		sel, ok := call.Fun.(*ast.SelectorExpr)
		if !ok || sel.Sel.Name != methodName {
			continue
		}
		recv, ok := sel.X.(*ast.Ident)
		if ok && recv.Name == recvName {
			return true
		}
	}
	return false
}

// ---------------------------------------------------------------------------
// Rule: storage.NewMemoryStore() → simNewStore()   (router.go only)
// ---------------------------------------------------------------------------

func applyStorageNewMemoryStore(file *ast.File) bool {
	modified := false
	astutil.Apply(file, func(c *astutil.Cursor) bool {
		call, ok := c.Node().(*ast.CallExpr)
		if !ok {
			return true
		}
		sel, ok := call.Fun.(*ast.SelectorExpr)
		if !ok {
			return true
		}
		pkg, ok := sel.X.(*ast.Ident)
		if !ok || pkg.Name != "storage" || sel.Sel.Name != "NewMemoryStore" {
			return true
		}
		call.Fun = ast.NewIdent("simNewStore")
		modified = true
		return true
	}, nil)
	return modified
}

// ---------------------------------------------------------------------------
// Rule: FibStrategyTable.Foo → simFib().Foo  (fw/table/rib.go only)
// ---------------------------------------------------------------------------

// applyFibGlobalPointerInternal replaces bare FibStrategyTable method-call
// receivers with simFib() inside the fw/table package (no "table." qualifier).
func applyFibGlobalPointerInternal(file *ast.File) bool {
	modified := false
	astutil.Apply(file, func(c *astutil.Cursor) bool {
		n, ok := c.Node().(*ast.SelectorExpr)
		if !ok {
			return true
		}
		x, ok := n.X.(*ast.Ident)
		if !ok || x.Name != "FibStrategyTable" {
			return true
		}
		n.X = &ast.CallExpr{Fun: ast.NewIdent("simFib")}
		modified = true
		return true
	}, nil)
	return modified
}

// ---------------------------------------------------------------------------
// Rule: add sync.RWMutex to DeadNonceList (fw/table/dead-nonce-list.go)
// ---------------------------------------------------------------------------

// applyDeadNonceListMutex adds a `mu sync.RWMutex` field to DeadNonceList and
// wraps Find (RLock), Insert, and RemoveExpiredEntries (Lock) for safe
// concurrent access from DV goroutines and the simulation maintenance callback.
func applyDeadNonceListMutex(file *ast.File, fset *token.FileSet) bool {
	modified := false
	astutil.Apply(file, func(c *astutil.Cursor) bool {
		switch n := c.Node().(type) {

		case *ast.GenDecl:
			if n.Tok != token.TYPE {
				break
			}
			for _, spec := range n.Specs {
				ts, ok := spec.(*ast.TypeSpec)
				if !ok || ts.Name.Name != "DeadNonceList" {
					continue
				}
				st, ok := ts.Type.(*ast.StructType)
				if !ok {
					continue
				}
				muField := &ast.Field{
					Names: []*ast.Ident{ast.NewIdent("mu")},
					Type: &ast.SelectorExpr{
						X:   ast.NewIdent("sync"),
						Sel: ast.NewIdent("RWMutex"),
					},
				}
				st.Fields.List = append([]*ast.Field{muField}, st.Fields.List...)
				modified = true
			}

		case *ast.FuncDecl:
			if n.Recv == nil || len(n.Recv.List) == 0 || n.Body == nil {
				break
			}
			star, ok := n.Recv.List[0].Type.(*ast.StarExpr)
			if !ok {
				break
			}
			ident, ok := star.X.(*ast.Ident)
			if !ok || ident.Name != "DeadNonceList" {
				break
			}
			recv := "d"
			if len(n.Recv.List[0].Names) > 0 {
				recv = n.Recv.List[0].Names[0].Name
			}
			var lockMethod, unlockMethod string
			switch n.Name.Name {
			case "Find":
				lockMethod, unlockMethod = "RLock", "RUnlock"
			case "Insert", "RemoveExpiredEntries":
				lockMethod, unlockMethod = "Lock", "Unlock"
			}
			if lockMethod == "" {
				break
			}
			lockStmt := &ast.ExprStmt{X: &ast.CallExpr{
				Fun: &ast.SelectorExpr{
					X:   &ast.SelectorExpr{X: ast.NewIdent(recv), Sel: ast.NewIdent("mu")},
					Sel: ast.NewIdent(lockMethod),
				},
			}}
			deferStmt := &ast.DeferStmt{Call: &ast.CallExpr{
				Fun: &ast.SelectorExpr{
					X:   &ast.SelectorExpr{X: ast.NewIdent(recv), Sel: ast.NewIdent("mu")},
					Sel: ast.NewIdent(unlockMethod),
				},
			}}
			n.Body.List = append([]ast.Stmt{lockStmt, deferStmt}, n.Body.List...)
			modified = true
		}
		return true
	}, nil)

	if modified {
		astutil.AddImport(fset, file, "sync")
	}
	return modified
}
