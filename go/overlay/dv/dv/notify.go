package dv

import (
	"github.com/named-data/ndnd/dv/table"
	_ndndsim "github.com/named-data/ndndsim"
)

// runConvergenceHook fires RouterReachableEvent for every router currently
// reachable in the RIB.  Called from postUpdateRib after each FIB sync.
//
// cgo_export.go deduplicates events per (nodeRouter, reachableRouter) pair, so
// firing on every update is safe -- only the first event per pair is counted.
func (dv *Router) runConvergenceHook() {
	dv.mutex.Lock()
	now := _ndndsim.Now()
	nodeRouter := dv.config.RouterName()
	var evts []table.RouterReachableEvent
	for _, router := range dv.rib.Entries() {
		dest := router.Name()
		if !dest.Equal(nodeRouter) {
			evts = append(evts, table.RouterReachableEvent{
				At:              now,
				NodeRouter:      nodeRouter,
				ReachableRouter: dest,
			})
		}
	}
	dv.mutex.Unlock()

	for _, ev := range evts {
		table.NotifyRouterReachable(ev)
	}
}
