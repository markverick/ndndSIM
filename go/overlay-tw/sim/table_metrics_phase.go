package sim

import (
	dv "github.com/named-data/ndnd/dv/dv"
	"github.com/named-data/ndnd/fw/table"
)

func (fwd *SimForwarder) simPhaseTableMetrics() []dv.SimTableMetric {
	metrics := make([]dv.SimTableMetric, 0, 2)

	if pet, ok := fwd.pet.(*table.PrefixEgressTable); ok && pet != nil {
		metrics = append(metrics, dv.SimTableMetric{
			Category:   dv.SimTableCategoryTwoPhase,
			Table:      "forwarder_pet",
			EntryCount: len(pet.GetAllEntries()),
		})
	}

	if fwd.multicastFib != nil {
		metrics = append(metrics, dv.SimTableMetric{
			Category:   dv.SimTableCategoryTwoPhase,
			Table:      "forwarder_multicast_fib",
			EntryCount: fwd.multicastFib.GetNumFIBEntries(),
		})
	}

	return metrics
}