package dv

func (dv *Router) simPhaseTableMetrics() []SimTableMetric {
	if dv.pfx == nil {
		return nil
	}

	return []SimTableMetric{
		{
			Category:   SimTableCategoryOnePhase,
			Table:      "dv_prefix_table",
			EntryCount: dv.pfx.EntryCount(),
		},
	}
}