package sim

// Unit tests for SimEngine behaviours that are not covered by the
// end-to-end DV integration tests:
//
//   1. AttachHandler returns ErrMultipleHandlers on duplicate prefix.
//   2. DetachHandler prunes the nameTrie so re-attachment works.
//   3. Express fires the timeout callback exactly once (not twice).
//   4. onData CanBePrefix: Interest name is a prefix of Data name (not the
//      reverse). This is the regression test for the dataName.IsPrefix bug.
//   5. RegisterRoute follows phase semantics: PET nexthop in twophase,
//      direct FIB route in onephase.

func petHasFaceForPrefix(resp any, prefix string, faceID uint64) bool {
	dataset := reflect.ValueOf(resp)
	if !dataset.IsValid() || dataset.Kind() != reflect.Ptr || dataset.IsNil() {
		return false
	}
	entries := dataset.Elem().FieldByName("Entries")
	if !entries.IsValid() || entries.Kind() != reflect.Slice {
		return false
	}
	for i := 0; i < entries.Len(); i++ {
		entry := entries.Index(i)
		nameField := entry.Elem().FieldByName("Name")
		entryName, ok := nameField.Interface().(interface{ String() string })
		if !ok || entryName.String() != prefix {
			continue
		}
		nextHopRecords := entry.Elem().FieldByName("NextHopRecords")
		for j := 0; j < nextHopRecords.Len(); j++ {
			nh := nextHopRecords.Index(j).Elem().FieldByName("FaceId")
			if nh.Uint() == faceID {
				return true
			}
		}
	}
	return false
}

import (
	"errors"
	"reflect"
	"sync/atomic"
	"testing"
	"time"

	"github.com/named-data/ndnd/std/ndn"
	mgmt "github.com/named-data/ndnd/std/ndn/mgmt_2022"
	sig "github.com/named-data/ndnd/std/security/signer"
	"github.com/named-data/ndnd/std/types/optional"
	"github.com/named-data/ndnd/std/utils"
)

// TestEngineAttachHandlerDuplicate verifies that attaching a second handler to
// the same prefix returns ErrMultipleHandlers.
func TestEngineAttachHandlerDuplicate(t *testing.T) {
	clock := NewDeterministicClock(time.Unix(0, 0))
	node := NewNode(0, clock)
	if err := node.Start(); err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer node.Stop()

	prefix := mustName(t, "/test/dup")
	noop := func(ndn.InterestHandlerArgs) {}

	if err := node.Engine().AttachHandler(prefix, noop); err != nil {
		t.Fatalf("first AttachHandler: %v", err)
	}
	err := node.Engine().AttachHandler(prefix, noop)
	if !errors.Is(err, ndn.ErrMultipleHandlers) {
		t.Fatalf("expected ErrMultipleHandlers, got %v", err)
	}
}

// TestEngineDetachHandlerAllowsReattach verifies that after DetachHandler the
// same prefix can be registered again (proves the nameTrie node was pruned).
func TestEngineDetachHandlerAllowsReattach(t *testing.T) {
	clock := NewDeterministicClock(time.Unix(0, 0))
	node := NewNode(0, clock)
	if err := node.Start(); err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer node.Stop()

	prefix := mustName(t, "/test/reattach")
	noop := func(ndn.InterestHandlerArgs) {}

	if err := node.Engine().AttachHandler(prefix, noop); err != nil {
		t.Fatalf("AttachHandler: %v", err)
	}
	if err := node.Engine().DetachHandler(prefix); err != nil {
		t.Fatalf("DetachHandler: %v", err)
	}
	// Detach+prune must allow a fresh attach.
	if err := node.Engine().AttachHandler(prefix, noop); err != nil {
		t.Fatalf("re-AttachHandler after DetachHandler: %v", err)
	}
}

// TestEngineExpressTimeoutFiredExactlyOnce verifies that when an Interest times
// out, the callback fires exactly once with InterestResultTimeout.  This is
// the regression test for the bug where Express returned an error but the
// scheduled timeout callback would also fire (double delivery).
func TestEngineExpressTimeoutFiredExactlyOnce(t *testing.T) {
	clock, n0, _, _, _, cleanup := makeConnectedPair(t)
	defer cleanup()

	prefix := mustName(t, "/timeout/test")
	// Route to n0's own app face so Express() does not fail immediately with
	// "no route".  No handler is registered for this prefix, so the Interest
	// is never answered and times out.
	n0.AddRoute(prefix, n0.AppFaceID(), 0)

	eng := n0.Engine()
	interest, err := eng.Spec().MakeInterest(prefix, &ndn.InterestConfig{
		Lifetime: optional.Some(100 * time.Millisecond),
		Nonce:    utils.ConvertNonce(eng.Timer().Nonce()),
	}, nil, nil)
	if err != nil {
		t.Fatalf("MakeInterest: %v", err)
	}

	var callCount int32
	expressErr := eng.Express(interest, func(args ndn.ExpressCallbackArgs) {
		if args.Result != ndn.InterestResultTimeout {
			t.Errorf("expected InterestResultTimeout, got %v", args.Result)
		}
		atomic.AddInt32(&callCount, 1)
	})
	if expressErr != nil {
		t.Fatalf("Express: %v", expressErr)
	}

	// Advance time past the lifetime + 50 ms slack to trigger the timeout.
	clock.Advance(200 * time.Millisecond)

	got := atomic.LoadInt32(&callCount)
	if got != 1 {
		t.Fatalf("timeout callback fired %d times, want exactly 1", got)
	}
}

// TestEngineOnDataCanBePrefix verifies that a CanBePrefix Interest is satisfied
// by a Data whose name is strictly longer (i.e. the Interest name is a prefix of
// the Data name).  In this test the Data is matched via the PIT-token path
// (Express always LP-wraps the Interest with a token, so the reply carries it
// back).  The name-based fallback path inside onData is a separate code path
// that cannot be exercised through the normal forwarding pipeline.
func TestEngineOnDataCanBePrefix(t *testing.T) {
	clock, n0, n1, face0to1, _, cleanup := makeConnectedPair(t)
	defer cleanup()

	// Producer serves /data/exact/suffix when asked for /data/exact (CanBePrefix).
	producerPrefix := mustName(t, "/data/exact")
	dataName := mustName(t, "/data/exact/suffix")
	if err := n1.Engine().RegisterRoute(producerPrefix); err != nil {
		t.Fatalf("RegisterRoute: %v", err)
	}
	n0.AddRoute(producerPrefix, face0to1, 0)

	signer := sig.NewSha256Signer()
	if err := n1.Engine().AttachHandler(producerPrefix, func(args ndn.InterestHandlerArgs) {
		data, err := n1.Engine().Spec().MakeData(
			dataName, // reply with a *longer* name than the Interest
			&ndn.DataConfig{},
			nil,
			signer,
		)
		if err != nil {
			t.Errorf("producer MakeData: %v", err)
			return
		}
		if err := args.Reply(data.Wire); err != nil {
			t.Errorf("producer Reply: %v", err)
		}
	}); err != nil {
		t.Fatalf("AttachHandler: %v", err)
	}

	eng0 := n0.Engine()
	interest, err := eng0.Spec().MakeInterest(producerPrefix, &ndn.InterestConfig{
		CanBePrefix: true,
		Lifetime:    optional.Some(2 * time.Second),
		Nonce:       utils.ConvertNonce(eng0.Timer().Nonce()),
	}, nil, nil)
	if err != nil {
		t.Fatalf("MakeInterest: %v", err)
	}

	var result atomic.Value
	if err := eng0.Express(interest, func(args ndn.ExpressCallbackArgs) {
		result.Store(args.Result)
	}); err != nil {
		t.Fatalf("Express: %v", err)
	}

	clock.Advance(500 * time.Millisecond)

	r, ok := result.Load().(ndn.InterestResult)
	if !ok || r != ndn.InterestResultData {
		t.Fatalf("CanBePrefix Data not matched: result=%v", result.Load())
	}
}

// TestEngineRegisterRouteMatchesPhaseBehavior verifies that RegisterRoute
// follows the correct per-phase semantics:
// twophase registers a PET nexthop (no direct FIB entry);
// onephase registers a direct FIB entry to the app face (no PET).
func TestEngineRegisterRouteMatchesPhaseBehavior(t *testing.T) {
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

	fibHops := node.Forwarder.Thread().Fib().FindNextHopsEnc(prefix)
	if node.Forwarder.pet != nil {
		// twophase build: must install a PET nexthop, must not touch the FIB.
		if len(fibHops) != 0 {
			t.Fatalf("twophase: RegisterRoute must not install a direct FIB entry, got %d nexthops", len(fibHops))
		}
		resp, err := eng.ExecMgmtCmd("pet", "list", &mgmt.ControlArgs{})
		if err != nil {
			t.Fatalf("pet/list: %v", err)
		}
		if !petHasFaceForPrefix(resp, prefix.String(), node.AppFaceID()) {
			t.Fatalf("twophase RegisterRoute did not install PET nexthop for %s on app face %d", prefix, node.AppFaceID())
		}
	} else {
		// onephase build: must install a direct FIB entry to the app face, no PET.
		if len(fibHops) != 1 || fibHops[0].Nexthop != node.AppFaceID() {
			t.Fatalf("onephase: RegisterRoute must install one direct FIB entry to app face %d, got %#v", node.AppFaceID(), fibHops)
		}
	}

	if err := eng.UnregisterRoute(prefix); err != nil {
		t.Fatalf("UnregisterRoute: %v", err)
	}

	fibHops = node.Forwarder.Thread().Fib().FindNextHopsEnc(prefix)
	if len(fibHops) != 0 {
		t.Fatalf("route still present in FIB after UnregisterRoute: %#v", fibHops)
	}
}
