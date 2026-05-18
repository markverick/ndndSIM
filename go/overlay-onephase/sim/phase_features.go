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

// addSimPetEgress is a no-op in onephase: PET does not exist.
func addSimPetEgress(any, enc.Name, enc.Name, bool) {}

func registerMgmtLocalhost(fwd *SimForwarder, faceID uint64) {
	fwd.fib.InsertNextHopEnc(defn.LOCAL_PREFIX, faceID, 0)
}

func execSimPetMgmtCmd(*SimForwarder, string, *mgmt.ControlArgs, uint64) (any, error) {
	return nil, fmt.Errorf("SimEngine: unsupported mgmt cmd pet")
}

// decodeEgressRouter is a no-op in the onephase build: the onephase upstream
// (ndnd@main) does not have EgressRouter in defn.FwLpPacket or defn.Pkt.
func decodeEgressRouter(*SimForwarder, *defn.Pkt, *defn.FwLpPacket) {}

// encodeEgressRouter is a no-op in the onephase build.
func encodeEgressRouter(*defn.FwLpPacket, *defn.Pkt) {}

// registerSimRoute registers a producer prefix via the RIB with RouteOriginApp,
// exactly matching what ndnd@main Engine.RegisterRoute does:
//   RegisterRoute → ExecMgmtCmd("rib","register") → Rib.AddEncRoute(Origin:RouteOriginApp=0, Flags:ChildInherit)
//   → updateNexthopsEnc → FibStrategyTable.InsertNextHopEnc
func registerSimRoute(fwd *SimForwarder, prefix enc.Name, appFaceID uint64) error {
	fwd.AddRouteWithFlags(prefix, appFaceID, 0,
		uint64(mgmt.RouteOriginApp),
		uint64(mgmt.RouteFlagChildInherit))
	return nil
}

// unregisterSimRoute removes the RIB entry installed by registerSimRoute.
func unregisterSimRoute(fwd *SimForwarder, prefix enc.Name, appFaceID uint64) error {
	fwd.RemoveRouteWithOrigin(prefix, appFaceID, uint64(mgmt.RouteOriginApp))
	return nil
}