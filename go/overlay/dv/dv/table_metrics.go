package dv


const (
	SimTableCategoryCommon   = "common"
	SimTableCategoryOnePhase = "onephase"
	SimTableCategoryTwoPhase = "twophase"
)

type SimTableMetric struct {
	Category    string
	Table       string
	EntryCount  int
}

func (dv *Router) SimTableMetrics() []SimTableMetric {
	metrics := []SimTableMetric{
		{
			Category:   SimTableCategoryCommon,
			Table:      "dv_rib",
			EntryCount: dv.rib.Size(),
		},
		{
			Category:   SimTableCategoryCommon,
			Table:      "dv_neighbors",
			EntryCount: dv.neighbors.Size(),
		},
	}
	return append(metrics, dv.simPhaseTableMetrics()...)
}