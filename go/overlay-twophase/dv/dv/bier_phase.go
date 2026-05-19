package dv

import (
	"github.com/named-data/ndnd/dv/nfdc"
	mgmt "github.com/named-data/ndnd/std/ndn/mgmt_2022"
	ndn_sync "github.com/named-data/ndnd/std/sync"
	_ndndsim "github.com/named-data/ndndsim"
)

func (dv *Router) registerPhaseBierRouters() {
	dv.RegisterSimBierRouters()
}

func (dv *Router) simSeedPsdPrefix() {
	dv.updatePsdPrefix()
}

func (dv *Router) resetPfxForSim() {}

func (dv *Router) startPfxFromScratchReady() {
	if _, ok := _pfxStarted.Load(dv); ok {
		return
	}
	if _, loaded := _pfxStarted.LoadOrStore(dv, struct{}{}); loaded {
		return
	}
	if prev, ok := _pfxCancel.Load(dv); ok {
		prev.(func())()
	}

	// In from-scratch experiments, keep PSD quiet at T=0 but ready to publish
	// the first real prefix event. This avoids PSD bootstrap traffic in p0.
	_ = dv.pfx.pfxSvs.SimStartQuiet()
}

func (dv *Router) startPfxSyntheticReady(_ int) {
	if _, ok := _pfxStarted.Load(dv); ok {
		return
	}
	if _, loaded := _pfxStarted.LoadOrStore(dv, struct{}{}); loaded {
		return
	}
	if prev, ok := _pfxCancel.Load(dv); ok {
		prev.(func())()
	}

	// Synthetic routing has already populated the RIB/PET inputs. Install the
	// PSD sync multicast entry and per-router PSD data entries without starting
	// the normal prefix daemon bootstrap path.
	dv.updatePsdPrefix()

	selfName := dv.config.RouterName()
	routers := dv.rib.SnapshotReachableNames()
	for _, router := range routers {
		if router.Equal(selfName) {
			continue
		}
		routerHash := router.Hash()
		if _, ok := dv.pfx.pfxSubs[routerHash]; ok {
			continue
		}
		dv.pfx.pfxSeen[routerHash] = router.Clone()
		dv.pfx.pfxSubs[routerHash] = router.Clone()
		if dv.pfx.replicatePsd && dv.pfx.nfdc != nil {
			route := dv.pfx.pfxGroup.Append(router...)
			dv.pfx.nfdc.Exec(nfdc.NfdMgmtCmd{
				Module:  "pet",
				Cmd:     "add-egress",
				Args:    &mgmt.ControlArgs{Name: route, Egress: &mgmt.EgressRecord{Name: router.Clone()}},
				Retries: -1,
			})
		}
		dv.pfx.pfxSvs.SubscribePublisher(router, func(sp ndn_sync.SvsPub) {
			_ndndsim.NdndsimRecordPfxSvsDelivery()
			dv.pfx.mu.Lock()
			_, petOps := dv.pfx.processUpdate(sp.Content)
			dv.pfx.mu.Unlock()
			dv.pfx.applyPetOps(petOps)
		})
	}

	_ = dv.pfx.pfxSvs.SimStartQuiet()
}
