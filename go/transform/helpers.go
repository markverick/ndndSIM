package main

import (
	"go/ast"
	"go/parser"
	"go/token"
	"strings"

	"golang.org/x/tools/go/ast/astutil"
)

// injectDecls parses src as a Go source snippet (prefixed with "package x\n\n")
// using the provided FileSet, and appends all top-level declarations from the
// snippet to file.Decls.  Use this instead of overlay files to inject new
// functions, methods, or var declarations into an upstream file.
func injectDecls(file *ast.File, fset *token.FileSet, src string) {
	snippet, err := parser.ParseFile(fset, "<injected>", "package x\n\n"+src, 0)
	if err != nil {
		panic("BUG: injection snippet parse error: " + err.Error())
	}
	file.Decls = append(file.Decls, snippet.Decls...)
}

// addNamedImport adds a named import (e.g. `_ndndsim "path"`) to the file's
// import block if not already present.
func addNamedImport(file *ast.File, localName, pkgPath string) {
	for _, imp := range file.Imports {
		if strings.Trim(imp.Path.Value, `"`) == pkgPath {
			return
		}
	}

	spec := &ast.ImportSpec{
		Name: ast.NewIdent(localName),
		Path: &ast.BasicLit{
			Kind:  token.STRING,
			Value: `"` + pkgPath + `"`,
		},
	}

	// Append to an existing import block if one exists.
	for _, decl := range file.Decls {
		gd, ok := decl.(*ast.GenDecl)
		if !ok || gd.Tok != token.IMPORT {
			continue
		}
		gd.Specs = append(gd.Specs, spec)
		file.Imports = append(file.Imports, spec)
		return
	}

	// No import block — prepend a new one.
	gd := &ast.GenDecl{
		Tok:    token.IMPORT,
		Lparen: 1, // force parenthesised form
		Specs:  []ast.Spec{spec},
	}
	file.Decls = append([]ast.Decl{gd}, file.Decls...)
	file.Imports = append(file.Imports, spec)
}

// pruneUnusedPackageImports removes imports whose package qualifier no longer
// appears anywhere in f after rewriting.  It handles normal, named, and
// dot-imports but skips blank (_) imports which are kept for side-effects.
func pruneUnusedPackageImports(fset *token.FileSet, f *ast.File) {
	// Collect every package name still referenced as a selector qualifier.
	used := make(map[string]bool)
	ast.Inspect(f, func(n ast.Node) bool {
		if sel, ok := n.(*ast.SelectorExpr); ok {
			if id, ok := sel.X.(*ast.Ident); ok {
				used[id.Name] = true
			}
		}
		return true
	})

	var toDelete []string
	for _, imp := range f.Imports {
		if imp == nil {
			continue
		}
		var localName string
		if imp.Name != nil {
			localName = imp.Name.Name
		} else {
			path := strings.Trim(imp.Path.Value, `"`)
			if idx := strings.LastIndex(path, "/"); idx >= 0 {
				localName = path[idx+1:]
			} else {
				localName = path
			}
		}
		if localName == "_" || localName == "." {
			continue
		}
		if !used[localName] {
			toDelete = append(toDelete, strings.Trim(imp.Path.Value, `"`))
		}
	}
	for _, path := range toDelete {
		astutil.DeleteImport(fset, f, path)
	}
}
