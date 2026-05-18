package table

import enc "github.com/named-data/ndnd/std/encoding"

// SimSetNeighborFace seeds the face used to reach a synthetic next hop.
func (nt *NeighborTable) SimSetNeighborFace(name enc.Name, faceId uint64) {
	if faceId == 0 {
		return
	}
	ns := nt.Get(name)
	if ns == nil {
		ns = nt.Add(name)
	}
	ns.faceId = faceId
	ns.isFaceActive = true
}
