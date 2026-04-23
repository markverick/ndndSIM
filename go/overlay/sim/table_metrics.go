package sim

import (
	dv "github.com/named-data/ndnd/dv/dv"
)

func (fwd *SimForwarder) SimTableMetrics() []dv.SimTableMetric {
	metrics := []dv.SimTableMetric{
		{
			Category:   dv.SimTableCategoryCommon,
			Table:      "forwarder_rib",
			EntryCount: len(fwd.rib.GetAllEntries()),
		},
		{
			Category:   dv.SimTableCategoryCommon,
			Table:      "forwarder_fib",
			EntryCount: fwd.fib.GetNumFIBEntries(),
		},
	}
	return append(metrics, fwd.simPhaseTableMetrics()...)
}