package sim

import (
	"testing"
	"time"
)

// TestRegisterRouteUsesFIB verifies that RegisterRoute installs a direct FIB
// entry on the app face. This is the onephase-specific test; the onephase build
// has no PET — the FIB is the only forwarding table for local delivery.
func TestRegisterRouteUsesFIB(t *testing.T) {
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

	// Must install exactly one FIB entry pointing at the app face.
	hops := node.Forwarder.Thread().Fib().FindNextHopsEnc(prefix)
	if len(hops) != 1 || hops[0].Nexthop != node.AppFaceID() {
		t.Fatalf("RegisterRoute must install one FIB entry to app face %d, got %#v", node.AppFaceID(), hops)
	}

	if err := eng.UnregisterRoute(prefix); err != nil {
		t.Fatalf("UnregisterRoute: %v", err)
	}

	// FIB entry must be gone.
	if hops := node.Forwarder.Thread().Fib().FindNextHopsEnc(prefix); len(hops) != 0 {
		t.Fatalf("FIB entry still present after UnregisterRoute: %#v", hops)
	}
}
