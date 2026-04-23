package table

// EntryCount returns the total number of prefix entries across all known routers.
func (pt *PrefixTable) EntryCount() int {
	count := 0
	for _, router := range pt.routers {
		count += len(router.Prefixes)
	}
	return count
}