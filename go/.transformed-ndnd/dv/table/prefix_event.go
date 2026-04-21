// PrefixEvent — simulation observer for prefix-table changes.
// Overlay file — injected by ndndSIM build system only.
package table

import (
	"sync"
	"time"

	enc "github.com/named-data/ndnd/std/encoding"
)

// PrefixEventKind identifies the type of prefix event.
type PrefixEventKind int

const (
	// PrefixEventGlobalAnnounce fires when the local node announces a new prefix.
	PrefixEventGlobalAnnounce PrefixEventKind = iota
	// PrefixEventAddRemotePrefix fires when a remote prefix is added to the table.
	PrefixEventAddRemotePrefix
)

// PrefixEvent is delivered to the registered observer on prefix-table changes.
type PrefixEvent struct {
	Kind   PrefixEventKind
	At     time.Time
	Name   enc.Name
	Router enc.Name
}

var (
	prefixObsMu sync.RWMutex
	prefixObs   func(PrefixEvent)
)

// SetPrefixEventObserver registers a callback invoked on every prefix event.
// Pass nil to deregister.
func SetPrefixEventObserver(fn func(PrefixEvent)) {
	prefixObsMu.Lock()
	prefixObs = fn
	prefixObsMu.Unlock()
}

// NotifyPrefixEvent fires the registered observer.  No-op if none registered.
func NotifyPrefixEvent(ev PrefixEvent) {
	prefixObsMu.RLock()
	fn := prefixObs
	prefixObsMu.RUnlock()
	if fn != nil {
		fn(ev)
	}
}
