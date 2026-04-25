package sim

import (
	"fmt"

	"github.com/named-data/ndnd/fw/defn"
	"github.com/named-data/ndnd/fw/fw"
	"github.com/named-data/ndnd/fw/table"
	enc "github.com/named-data/ndnd/std/encoding"
	mgmt "github.com/named-data/ndnd/std/ndn/mgmt_2022"
)

func newSimPet() any {
	return nil
}

func newSimMulticastFib() table.FibStrategy {
	return nil
}

func attachSimPetThread(*fw.Thread, any) {}

func cleanUpSimPetFace(any, uint64) {}

func addSimPetNextHop(any, enc.Name, uint64, uint64) {}

func removeSimPetNextHop(any, enc.Name, uint64) {}

func execSimPetMgmtCmd(*SimForwarder, string, *mgmt.ControlArgs, uint64) (any, error) {
	return nil, fmt.Errorf("SimEngine: unsupported mgmt cmd pet")
}

// decodeEgressRouter is a no-op in the onephase build: the onephase upstream
// (ndnd@main) does not have EgressRouter in defn.FwLpPacket or defn.Pkt.
func decodeEgressRouter(*SimForwarder, *defn.Pkt, *defn.FwLpPacket) {}

// encodeEgressRouter is a no-op in the onephase build.
func encodeEgressRouter(*defn.FwLpPacket, *defn.Pkt) {}