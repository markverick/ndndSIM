// RouterReachableEvent — simulation observer for router reachability.
// Overlay file — injected by ndndSIM build system only.
package table

import (
	"sync"
	"time"

	enc "github.com/named-data/ndnd/std/encoding"
)

// RouterReachableEvent is fired when a DV node first learns a route to
// another router (the router becomes reachable in the RIB).
type RouterReachableEvent struct {
	// At is the timestamp when the router became reachable.
	At time.Time
	// NodeRouter is the router name of the observing node.
	NodeRouter enc.Name
	// ReachableRouter is the router name that just became reachable.
	ReachableRouter enc.Name
}

var (
	routerObsMu sync.RWMutex
	routerObs   func(RouterReachableEvent)
)

// SetRouterReachableObserver registers a callback invoked each time a DV
// node discovers that another router has become reachable.
func SetRouterReachableObserver(fn func(RouterReachableEvent)) {
	routerObsMu.Lock()
	routerObs = fn
	routerObsMu.Unlock()
}

// NotifyRouterReachable fires the registered observer.
// Safe to call from any goroutine.  No-op if none registered.
func NotifyRouterReachable(ev RouterReachableEvent) {
	routerObsMu.RLock()
	fn := routerObs
	routerObsMu.RUnlock()
	if fn != nil {
		fn(ev)
	}
}
