package dv

func (dv *Router) simPhaseTableMetrics() []SimTableMetric {
	if dv.pfx == nil {
		return nil
	}

	return []SimTableMetric{
		{
			Category:   SimTableCategoryTwoPhase,
			Table:      "dv_prefix_egress_state",
			EntryCount: dv.pfx.EntryCount(),
		},
	}
}