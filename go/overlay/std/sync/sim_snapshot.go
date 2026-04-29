package sync

import enc "github.com/named-data/ndnd/std/encoding"

// ExportInstanceState returns the current SVS-ALO instance state as a TLV
// wire. The wire can be persisted and later passed to ImportInstanceState to
// restore the state in a new simulation run (Stage N+1 starting from Stage N's
// final state without re-running convergence).
func (s *SvsALO) ExportInstanceState() enc.Wire {
	return s.instanceState()
}

// ImportInstanceState restores the SVS-ALO state from a previously exported
// wire. Must be called before Start() so that the SVS instance is initialised
// with the correct sequence numbers and boot time.
func (s *SvsALO) ImportInstanceState(wire enc.Wire) error {
	return s.parseInstanceState(wire)
}
