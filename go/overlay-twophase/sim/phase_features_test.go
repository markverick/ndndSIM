package sim

import (
"testing"
"time"

"github.com/named-data/ndnd/fw/table"
mgmt "github.com/named-data/ndnd/std/ndn/mgmt_2022"
)

// TestRegisterRouteUsesPET verifies that RegisterRoute installs a PET nexthop
// on the app face and does NOT install a direct FIB entry.
// This is the twophase-specific test; onephase has its own version.
func TestRegisterRouteUsesPET(t *testing.T) {
clock := NewDeterministicClock(time.Unix(0, 0))
node := NewNode(0, clock)
if err := node.Start(); err != nil {
t.Fatalf("Start: %v", err)
}
defer node.Stop()

prefix := mustName(t, "/test/register-route")
eng := node.Engine()

if err := eng.RegisterRoute(prefix); err != nil {
t.Fatalf("RegisterRoute: %v", err)
}

// Must not touch the FIB.
if hops := node.Forwarder.Thread().Fib().FindNextHopsEnc(prefix); len(hops) != 0 {
t.Fatalf("RegisterRoute must not install a direct FIB entry, got %d nexthops", len(hops))
}

// Must install a PET nexthop on the app face.
pet := node.Forwarder.pet.(*table.PrefixEgressTable)
resp, err := eng.ExecMgmtCmd("pet", "list", &mgmt.ControlArgs{})
if err != nil {
t.Fatalf("pet/list: %v", err)
}
_ = pet // PET type confirmed by compile-time assertion above.
found := false
if status, ok := resp.(*mgmt.PetStatus); ok {
for _, entry := range status.Entries {
if entry.Name.String() != prefix.String() {
continue
}
for _, nh := range entry.NextHopRecords {
if nh.FaceId == node.AppFaceID() {
found = true
}
}
}
}
if !found {
t.Fatalf("RegisterRoute did not install a PET nexthop for %s on app face %d", prefix, node.AppFaceID())
}

if err := eng.UnregisterRoute(prefix); err != nil {
t.Fatalf("UnregisterRoute: %v", err)
}

// PET nexthop must be gone.
resp, err = eng.ExecMgmtCmd("pet", "list", &mgmt.ControlArgs{})
if err != nil {
t.Fatalf("pet/list after unregister: %v", err)
}
if status, ok := resp.(*mgmt.PetStatus); ok {
for _, entry := range status.Entries {
if entry.Name.String() != prefix.String() {
continue
}
for _, nh := range entry.NextHopRecords {
if nh.FaceId == node.AppFaceID() {
t.Fatalf("PET nexthop still present after UnregisterRoute for %s on app face %d", prefix, node.AppFaceID())
}
}
}
}

// FIB must still be clean.
if hops := node.Forwarder.Thread().Fib().FindNextHopsEnc(prefix); len(hops) != 0 {
t.Fatalf("FIB unexpectedly has entries after UnregisterRoute: %#v", hops)
}
}
