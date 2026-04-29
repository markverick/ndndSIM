package table

import (
	"github.com/named-data/ndnd/dv/config"
	enc "github.com/named-data/ndnd/std/encoding"
)

// RibSnapshotRow is one row of exported RIB state.
// Each row captures a single (destination, nextHop, cost) triple.
type RibSnapshotRow struct {
	Dest    enc.Name
	NextHop enc.Name
	Cost    uint64
}

// Snapshot returns all reachable RIB entries as a flat slice of rows.
// Entries with cost >= CostInfinity are excluded.
// The slice can be passed to RestoreSnapshot() on a fresh Rib to replay the
// same routing state without re-running DV convergence.
func (r *Rib) Snapshot() []RibSnapshotRow {
	var rows []RibSnapshotRow
	for _, entry := range r.entries {
		for hopHash, cost := range entry.costs {
			if cost >= config.CostInfinity {
				continue
			}
			hopName := r.neighbors[hopHash]
			if hopName == nil {
				continue
			}
			rows = append(rows, RibSnapshotRow{
				Dest:    entry.name.Clone(),
				NextHop: hopName.Clone(),
				Cost:    cost,
			})
		}
	}
	return rows
}

// RestoreSnapshot restores the RIB from a previously exported snapshot.
// It calls Set() for each row, replicating the exact same routing state.
// Must be called before any heartbeat/deadcheck so the RIB is populated
// before DV processes its first advertisement.
func (r *Rib) RestoreSnapshot(rows []RibSnapshotRow) {
	for _, row := range rows {
		r.Set(row.Dest, row.NextHop, row.Cost)
	}
}

// FibSnapshotRow is one row of exported FIB state.
type FibSnapshotRow struct {
	Prefix enc.Name
	FaceId uint64
	Cost   uint64
}

// Snapshot returns all FIB entries as a flat slice of rows.
func (fib *Fib) Snapshot() []FibSnapshotRow {
	var rows []FibSnapshotRow
	for h, entries := range fib.prefixes {
		name := fib.names[h]
		if name == nil {
			continue
		}
		for _, e := range entries {
			if e.Cost >= config.CostPfxInfinity {
				continue
			}
			rows = append(rows, FibSnapshotRow{
				Prefix: name.Clone(),
				FaceId: e.FaceId,
				Cost:   e.Cost,
			})
		}
	}
	return rows
}

// RestoreSnapshot re-installs FIB entries from a previously exported snapshot.
// It calls Update() for each prefix, registering each face-route via nfdc.
func (fib *Fib) RestoreSnapshot(rows []FibSnapshotRow) {
	// Group rows by prefix name.
	byPrefix := make(map[string][]FibSnapshotRow)
	for _, row := range rows {
		key := row.Prefix.TlvStr()
		byPrefix[key] = append(byPrefix[key], row)
	}
	for _, group := range byPrefix {
		name := group[0].Prefix
		entries := make([]FibEntry, 0, len(group))
		for _, row := range group {
			entries = append(entries, FibEntry{
				FaceId: row.FaceId,
				Cost:   row.Cost,
			})
		}
		fib.Update(name, entries)
	}
}
