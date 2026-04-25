package sim

import (
	"fmt"

	"github.com/named-data/ndnd/fw/defn"
	"github.com/named-data/ndnd/fw/fw"
	"github.com/named-data/ndnd/fw/table"
	enc "github.com/named-data/ndnd/std/encoding"
	mgmt "github.com/named-data/ndnd/std/ndn/mgmt_2022"
	"github.com/named-data/ndnd/std/types/optional"
)

func newSimPet() any {
	return table.NewPrefixEgressTable()
}

func newSimMulticastFib() table.FibStrategy {
	return table.NewMulticastStrategyTree()
}

func attachSimPetThread(thread *fw.Thread, pet any) {
	thread.SetPet(pet.(*table.PrefixEgressTable))
}

func cleanUpSimPetFace(pet any, faceID uint64) {
	pet.(*table.PrefixEgressTable).CleanUpFace(faceID)
}

func addSimPetNextHop(pet any, name enc.Name, faceID uint64, cost uint64) {
	pet.(*table.PrefixEgressTable).AddNextHopEnc(name, faceID, cost)
}

func removeSimPetNextHop(pet any, name enc.Name, faceID uint64) {
	pet.(*table.PrefixEgressTable).RemoveNextHopEnc(name, faceID)
}

// decodeEgressRouter copies the EgressRouter from an incoming LP header into
// the packet, then strips it if it names this node so the forwarding pipeline
// takes fwUnicastIngress (→ PET lookup → local delivery) rather than
// fwUnicastTransit (FIB-only, drops without a matching PIT entry).
// In the simulation CfgRouterName() always returns false because
// core.C.Fw.RouterName is never set; we use hooks.RouterName instead.
func decodeEgressRouter(fwd *SimForwarder, pkt *defn.Pkt, lp *defn.FwLpPacket) {
	if lp.EgressRouter != nil {
		pkt.EgressRouter = lp.EgressRouter.Name
	}
	if rn, ok := fwd.hooks.RouterName.(enc.Name); ok && len(rn) > 0 && len(pkt.EgressRouter) > 0 && pkt.EgressRouter.Equal(rn) {
		pkt.EgressRouter = nil
	}
}

// encodeEgressRouter writes the EgressRouter into an outgoing LP header for
// Interest packets that have one set (i.e., those routed via a PET egress
// entry). Transit nodes use this to forward via FIB when they lack a PET entry.
func encodeEgressRouter(lpFrag *defn.FwLpPacket, pkt *defn.Pkt) {
	if pkt.L3.Interest != nil && len(pkt.EgressRouter) > 0 {
		lpFrag.EgressRouter = &defn.FwEgressRouter{Name: pkt.EgressRouter}
	}
}

func execSimPetMgmtCmd(fwd *SimForwarder, cmd string, ca *mgmt.ControlArgs, appFaceID uint64) (any, error) {
	pet, ok := fwd.pet.(*table.PrefixEgressTable)
	if !ok || pet == nil {
		return nil, fmt.Errorf("SimEngine: unsupported mgmt cmd pet")
	}

	switch cmd {
	case "add-egress":
		if ca.Name == nil {
			return nil, fmt.Errorf("pet/add-egress: missing name")
		}
		if ca.Egress == nil || len(ca.Egress.Name) == 0 {
			return nil, fmt.Errorf("pet/add-egress: missing egress")
		}
		pet.AddEgressEnc(ca.Name, ca.Egress.Name, ca.Multicast)
		return &mgmt.ControlResponse{Val: &mgmt.ControlResponseVal{
			StatusCode: 200,
			Params:     &mgmt.ControlArgs{Name: ca.Name, Egress: ca.Egress},
		}}, nil
	case "remove-egress":
		if ca.Name == nil {
			return nil, fmt.Errorf("pet/remove-egress: missing name")
		}
		if ca.Egress == nil || len(ca.Egress.Name) == 0 {
			return nil, fmt.Errorf("pet/remove-egress: missing egress")
		}
		pet.RemoveEgressEnc(ca.Name, ca.Egress.Name)
		return &mgmt.ControlResponse{Val: &mgmt.ControlResponseVal{
			StatusCode: 200,
			Params:     &mgmt.ControlArgs{Name: ca.Name, Egress: ca.Egress},
		}}, nil
	case "add-nexthop":
		if ca.Name == nil {
			return nil, fmt.Errorf("pet/add-nexthop: missing name")
		}
		faceID := ca.FaceId.GetOr(0)
		if faceID == 0 {
			faceID = appFaceID
		}
		if fwd.GetFace(faceID) == nil {
			return nil, fmt.Errorf("pet/add-nexthop: face %d does not exist", faceID)
		}
		cost := ca.Cost.GetOr(0)
		pet.AddNextHopEnc(ca.Name, faceID, cost)
		return &mgmt.ControlResponse{Val: &mgmt.ControlResponseVal{
			StatusCode: 200,
			Params:     &mgmt.ControlArgs{Name: ca.Name, FaceId: optional.Some(faceID), Cost: optional.Some(cost)},
		}}, nil
	case "remove-nexthop":
		if ca.Name == nil {
			return nil, fmt.Errorf("pet/remove-nexthop: missing name")
		}
		faceID := ca.FaceId.GetOr(0)
		if faceID == 0 {
			faceID = appFaceID
		}
		pet.RemoveNextHopEnc(ca.Name, faceID)
		return &mgmt.ControlResponse{Val: &mgmt.ControlResponseVal{
			StatusCode: 200,
			Params:     &mgmt.ControlArgs{Name: ca.Name, FaceId: optional.Some(faceID)},
		}}, nil
	case "list":
		entries := pet.GetAllEntries()
		dataset := &mgmt.PetStatus{}
		for _, entry := range entries {
			petEntry := &mgmt.PetEntry{
				Name:           entry.Name,
				EgressRecords:  make([]*mgmt.EgressRecord, 0, len(entry.EgressRouters)),
				NextHopRecords: make([]*mgmt.NextHopRecord, 0, len(entry.NextHops)),
			}
			for _, egress := range entry.EgressRouters {
				petEntry.EgressRecords = append(petEntry.EgressRecords, &mgmt.EgressRecord{Name: egress})
			}
			for _, nextHop := range entry.NextHops {
				petEntry.NextHopRecords = append(petEntry.NextHopRecords, &mgmt.NextHopRecord{FaceId: nextHop.FaceID, Cost: nextHop.Cost})
			}
			dataset.Entries = append(dataset.Entries, petEntry)
		}
		return dataset, nil
	default:
		return nil, fmt.Errorf("SimEngine: unsupported mgmt cmd pet/%s", cmd)
	}
}