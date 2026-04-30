package sim

import (
	"encoding/json"
	"fmt"
	"os"
	"unsafe"

	dv "github.com/named-data/ndnd/dv/dv"
)

/*
#include "ndndsim_sim.h"
#include <stdlib.h>
*/
import "C"

// snapshotFile is the on-disk format written by NdndSimExportSnapshot and read
// by NdndSimImportSnapshot.  Each key is the decimal node ID.
type snapshotFile struct {
	Nodes map[string]json.RawMessage `json:"nodes"`
}

// NdndSimExportSnapshot exports the DV routing state of every node to a JSON
// file at the given path.  Returns 0 on success, -1 on error.
//
//export NdndSimExportSnapshot
func NdndSimExportSnapshot(path *C.char) C.int {
	if globalRuntime == nil {
		return -1
	}
	filePath := C.GoString(path)

	nodes := make(map[string]json.RawMessage)
	var exportErr error

	globalRuntime.IterNodes(func(id uint32, node *Node) {
		if exportErr != nil {
			return
		}
		sdv := node.DvRouter()
		if sdv == nil {
			return
		}
		snap := sdv.Router().ExportSnapshot()
		b, err := json.Marshal(snap)
		if err != nil {
			exportErr = fmt.Errorf("node %d: %w", id, err)
			return
		}
		nodes[fmt.Sprintf("%d", id)] = json.RawMessage(b)
	})

	if exportErr != nil {
		fmt.Fprintf(os.Stderr, "NdndSimExportSnapshot: %v\n", exportErr)
		return -1
	}

	sf := snapshotFile{Nodes: nodes}
	b, err := json.MarshalIndent(sf, "", "  ")
	if err != nil {
		fmt.Fprintf(os.Stderr, "NdndSimExportSnapshot: marshal: %v\n", err)
		return -1
	}
	if err := os.WriteFile(filePath, b, 0o644); err != nil {
		fmt.Fprintf(os.Stderr, "NdndSimExportSnapshot: write %s: %v\n", filePath, err)
		return -1
	}
	return 0
}

// NdndSimImportSnapshot restores DV routing state for every node from a JSON
// snapshot file at the given path.  Must be called after NdndSimStartDv for
// all nodes but before the simulator advances time.  Returns 0 on success,
// -1 on error.
//
//export NdndSimImportSnapshot
func NdndSimImportSnapshot(path *C.char) C.int {
	if globalRuntime == nil {
		return -1
	}
	filePath := C.GoString(path)

	data, err := os.ReadFile(filePath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "NdndSimImportSnapshot: read %s: %v\n", filePath, err)
		return -1
	}

	var sf snapshotFile
	if err := json.Unmarshal(data, &sf); err != nil {
		fmt.Fprintf(os.Stderr, "NdndSimImportSnapshot: parse: %v\n", err)
		return -1
	}

	var importErr error
	for key, raw := range sf.Nodes {
		if importErr != nil {
			break
		}
		var id uint32
		if _, err := fmt.Sscanf(key, "%d", &id); err != nil {
			importErr = fmt.Errorf("invalid node key %q: %w", key, err)
			break
		}

		node := globalRuntime.GetNode(id)
		if node == nil {
			// Node not created in this run — skip silently (e.g. partial import)
			continue
		}
		sdv := node.DvRouter()
		if sdv == nil {
			continue
		}

		var snap dv.RouterSnapshot
		if err := json.Unmarshal(raw, &snap); err != nil {
			importErr = fmt.Errorf("node %d: unmarshal snapshot: %w", id, err)
			break
		}
		if err := sdv.ImportSnapshot(snap); err != nil {
			importErr = fmt.Errorf("node %d: import snapshot: %w", id, err)
			break
		}
	}

	if importErr != nil {
		fmt.Fprintf(os.Stderr, "NdndSimImportSnapshot: %v\n", importErr)
		return -1
	}
	return 0
}

// Ensure the unsafe import is used (required by CGo).
var _ = unsafe.Pointer(nil)
