package dv

func (dv *Router) registerPhaseBierRouters() {}

func (dv *Router) simSeedPsdPrefix() {}

func (dv *Router) resetPfxForSim() {
	dv.pfx.Reset()
}

func (dv *Router) startPfxFromScratchReady() {}

func (dv *Router) startPfxSyntheticReady(_ int) {
	if _, loaded := _pfxStarted.LoadOrStore(dv, struct{}{}); !loaded {
		_ = dv.pfxSvs.SimStartQuiet()
	}
}
