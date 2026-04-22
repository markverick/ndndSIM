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
// method that must run in a real goroutine rather than a clock-scheduled event.
// These methods never return until explicitly stopped (via a channel/stop
// signal) so they cannot be executed inside DeterministicClock.Advance().
func isLongLivedCall(call *ast.CallExpr) bool {
	sel, ok := call.Fun.(*ast.SelectorExpr)
	if !ok {
		return false
	}
	switch sel.Sel.Name {
	case "main", "run":
		// SvSync.main() and SvsALO.run() are blocking event-loop goroutines.
		return true
	}
	return false
}

func makeGoCall(call *ast.CallExpr) *ast.ExprStmt {
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

	// Long-lived blocking loops must use GoLong so they bypass the
	// GoFunc=clock.Schedule hook and always run in a real goroutine.
	// Check both the original call and (for IIFEs) the first statement
	// inside the unwrapped function literal body.
	goFuncName := "Go"
	if isLongLivedCall(call) {
		goFuncName = "GoLong"
	} else if funcLit != nil && len(funcLit.Body.List) == 1 {
		if es, ok := funcLit.Body.List[0].(*ast.ExprStmt); ok {
			if inner, ok := es.X.(*ast.CallExpr); ok && isLongLivedCall(inner) {
				goFuncName = "GoLong"
			}
		}
	}

	return &ast.ExprStmt{
		X: &ast.CallExpr{
			Fun: &ast.SelectorExpr{
				X:   ast.NewIdent("_ndndsim"),
				Sel: ast.NewIdent(goFuncName),
			},
			Args: []ast.Expr{funcLit},
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
