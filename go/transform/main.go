// ndnd-transform patches a clean copy of ndnd source so that the
// ndndSIM simulation layer can inject per-node scheduler/clock/table
// overrides without forking ndnd.
//
// Usage:
//
//	ndnd-transform \
//	    --src  /path/to/upstream/ndnd \
//	    --out  /path/to/.transformed-ndnd \
//	    --overlay /path/to/ndndSIM/go/overlay \
//	    --sim-module github.com/named-data/ndndsim
//
// The tool:
//  1. Copies --src to --out.
//  2. Applies AST rewrites to the target packages inside --out.
//  3. Copies every file from --overlay/{pkg-path}/ into --out/{pkg-path}/.
//  4. Adds a `require` + `replace` for the ndndsim module in --out/go.mod.
package main

import (
	"flag"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
)

func main() {
	src := flag.String("src", "", "path to clean upstream ndnd source (required)")
	out := flag.String("out", "", "output directory for the transformed ndnd (required)")
	overlayDir := flag.String("overlay", "", "overlay directory (required)")
	simModule := flag.String("sim-module", "github.com/named-data/ndndsim", "module path of the ndndsim package")
	simModuleDir := flag.String("sim-module-dir", "", "local path to the ndndsim module (for replace directive)")
	phase := flag.String("phase", "twophase", "build phase: twophase or onephase")
	flag.Parse()

	if *src == "" || *out == "" || *overlayDir == "" {
		fmt.Fprintln(os.Stderr, "usage: ndnd-transform --src <ndnd> --out <dir> --overlay <dir> [--sim-module <path>] [--sim-module-dir <dir>] [--phase twophase|onephase]")
		os.Exit(1)
	}

	// 1. Copy source tree to output.
	if err := copyDir(*src, *out); err != nil {
		fatalf("copy: %v", err)
	}

	// 2. Apply AST rewrites to target packages.
	rewrites := targetRewrites(*out, *simModule, *phase)
	for _, r := range rewrites {
		if err := rewritePackage(r); err != nil {
			fatalf("rewrite %s: %v", r.pkgDir, err)
		}
	}

	// 3. Copy overlay files.
	if err := applyOverlay(*overlayDir, *out); err != nil {
		fatalf("overlay: %v", err)
	}

	// 4. Patch go.mod to require ndndsim.
	if err := patchGoMod(*out, *simModule, *simModuleDir); err != nil {
		fatalf("go.mod: %v", err)
	}

	fmt.Printf("transformed ndnd written to %s\n", *out)
}

// ---------------------------------------------------------------------------
// File-system helpers
// ---------------------------------------------------------------------------

func copyDir(src, dst string) error {
	// Remove destination if it exists so we start fresh.
	if err := os.RemoveAll(dst); err != nil {
		return err
	}
	return filepath.WalkDir(src, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(src, path)
		target := filepath.Join(dst, rel)
		if d.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		return copyFile(path, target)
	})
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()

	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer out.Close()
	_, err = io.Copy(out, in)
	return err
}

// applyOverlay copies every file from overlayDir/{pkg}/ into outDir/{pkg}/.
// The overlay directory mirrors the package structure of ndnd.
func applyOverlay(overlayDir, outDir string) error {
	return filepath.WalkDir(overlayDir, func(path string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		rel, _ := filepath.Rel(overlayDir, path)
		dst := filepath.Join(outDir, rel)
		fmt.Printf("  overlay: %s\n", rel)
		return copyFile(path, dst)
	})
}

// patchGoMod appends a require + (optional) replace for ndndsim to go.mod.
func patchGoMod(outDir, simModule, simModuleDir string) error {
	gomod := filepath.Join(outDir, "go.mod")
	f, err := os.OpenFile(gomod, os.O_APPEND|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	defer f.Close()

	// Use a pseudo-version that go.work overrides; any version string works.
	fmt.Fprintf(f, "\nrequire %s v0.0.0-00010101000000-000000000000\n", simModule)
	if simModuleDir != "" {
		fmt.Fprintf(f, "\nreplace %s => %s\n", simModule, simModuleDir)
	}
	return nil
}

func fatalf(format string, args ...interface{}) {
	fmt.Fprintf(os.Stderr, "ndnd-transform: "+format+"\n", args...)
	os.Exit(1)
}
