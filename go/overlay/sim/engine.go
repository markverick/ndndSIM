package sim

import (
	"encoding/binary"
	"fmt"
	"sync"
	"sync/atomic"
	"time"

	"github.com/named-data/ndnd/fw/defn"
	enc "github.com/named-data/ndnd/std/encoding"
	"github.com/named-data/ndnd/std/ndn"
	"github.com/named-data/ndnd/std/log"
	mgmt "github.com/named-data/ndnd/std/ndn/mgmt_2022"
	spec "github.com/named-data/ndnd/std/ndn/spec_2022"
	"github.com/named-data/ndnd/std/types/optional"
)

// SimEngine implements ndn.Engine for discrete-event simulation.
//
// Unlike BasicEngine, it processes all packets synchronously on the caller's
// thread. This is essential because ns-3 is single-threaded: all API calls
// (including Simulator::Schedule, NetDevice::Send) must happen on the main
// simulation thread.
//
// When the forwarder delivers a packet to the application face, SimFace.Receive()
// calls SimEngine.onPacket() directly, which parses, dispatches to the handler,
// and the handler's Reply callback sends Data back -- all synchronously, all on
// the ns-3 main thread.
type SimEngine struct {
	face    ndn.Face
	timer   ndn.Timer
	running atomic.Bool

	// Node ID for callbacks to C++
	nodeID uint32

	// Reference to the node's forwarder for ExecMgmtCmd
	forwarder *SimForwarder

	// App face ID for default route registration
	appFaceID uint64

	// Called when Data is received at this engine (consumer side)
	onDataReceived func(nodeID uint32, dataSize uint32, dataName string)

		// Interest handler FIB (prefix -> handler)
	fib     *nameTrie
	fibLock sync.Mutex

		// Pending Interest Table for Express() -- maps PIT token to callback
	pit     map[uint64]*pendingInterest
	pitLock sync.Mutex
	pitSeq  uint64
}

// pendingInterest tracks an outstanding Interest expression.
type pendingInterest struct {
	callback      ndn.ExpressCallbackFunc
	timeoutCancel func() error
	name          enc.Name
	canBePrefix   bool
}

var _ ndn.Engine = (*SimEngine)(nil)

// NewSimEngine creates a new simulation engine attached to the given face and timer.
func NewSimEngine(face ndn.Face, timer ndn.Timer, nodeID uint32, onDataReceived func(uint32, uint32, string)) *SimEngine {
	return &SimEngine{
		face:           face,
		timer:          timer,
		nodeID:         nodeID,
		onDataReceived: onDataReceived,
			fib:            newNameTrie(),
		pit:            make(map[uint64]*pendingInterest),
	}
}

func (e *SimEngine) String() string {
	return "SimEngine"
}

func (e *SimEngine) EngineTrait() ndn.Engine {
	return e
}

func (e *SimEngine) Spec() ndn.Spec {
	return spec.Spec{}
}

func (e *SimEngine) Timer() ndn.Timer {
	return e.timer
}

func (e *SimEngine) Face() ndn.Face {
	return e.face
}

func (e *SimEngine) IsRunning() bool {
	return e.running.Load()
}

func (e *SimEngine) Start() error {
	if e.face.IsRunning() {
		return fmt.Errorf("face is already running")
	}

	// Register synchronous packet handler -- no goroutine, no channel.
	e.face.OnPacket(func(frame []byte) {
		e.onPacket(frame)
	})

	if err := e.face.Open(); err != nil {
		return err
	}
	e.running.Store(true)
	return nil
}

func (e *SimEngine) Stop() error {
	if !e.running.Load() {
		return fmt.Errorf("engine is not running")
	}
	e.running.Store(false)
	return e.face.Close()
}

func (e *SimEngine) AttachHandler(prefix enc.Name, handler ndn.InterestHandler) error {
	e.fibLock.Lock()
	defer e.fibLock.Unlock()
	n := e.fib.matchAlways(prefix)
	if n.val != nil {
		return fmt.Errorf("%w: %s", ndn.ErrMultipleHandlers, prefix)
	}
	n.val = handler
	return nil
}

func (e *SimEngine) DetachHandler(prefix enc.Name) error {
	e.fibLock.Lock()
	defer e.fibLock.Unlock()
	n := e.fib.exactMatch(prefix)
	if n == nil {
		return fmt.Errorf("no handler for prefix %s", prefix)
	}
	n.val = nil
	n.prune()
	return nil
}

// DispatchInterest delivers an encoded Interest directly to the local handler
// registered for its prefix, bypassing the forwarder. This avoids the
// same-face loop prevention that would drop the Interest when both the
// source and destination are on the same application face.
func (e *SimEngine) DispatchInterest(interest *ndn.EncodedInterest) {
	name := interest.FinalName

	handler := func() ndn.InterestHandler {
		e.fibLock.Lock()
		defer e.fibLock.Unlock()
		n := e.fib.prefixMatch(name)
		for n != nil && n.val == nil {
			n = n.par
		}
		if n != nil {
			return n.val
		}
		return nil
	}()
	if handler == nil {
		return
	}

	// Parse the Interest from wire for the handler
	var pkt *spec.Packet
	var err error
	raw := interest.Wire
	if len(raw) == 1 {
		pkt, _, err = spec.ReadPacket(enc.NewBufferView(raw[0]))
	} else {
		pkt, _, err = spec.ReadPacket(enc.NewWireView(raw))
	}
	if err != nil || pkt.Interest == nil {
		return
	}

	handler(ndn.InterestHandlerArgs{
		Interest: pkt.Interest,
		Reply:    func(wire enc.Wire) error { return nil }, // discard reply Data
		Deadline: e.timer.Now().Add(pkt.Interest.Lifetime().GetOr(4 * time.Second)),
	})
}

// Express sends an Interest and optionally tracks a callback for the reply.
// If callback is nil, the Interest is fire-and-forget (e.g., Sync Interests).
func (e *SimEngine) Express(interest *ndn.EncodedInterest, callback ndn.ExpressCallbackFunc) error {
	if !e.running.Load() || !e.face.IsRunning() {
		return ndn.ErrFaceDown
	}

	wire := interest.Wire

	if callback != nil {
		// Generate a PIT token to match the returning Data
		e.pitLock.Lock()
		e.pitSeq++
		token := e.pitSeq
		tokenBytes := make([]byte, 8)
		binary.BigEndian.PutUint64(tokenBytes, token)

		lifetime := interest.Config.Lifetime.GetOr(4 * time.Second)
		pi := &pendingInterest{
			callback:    callback,
			name:        interest.FinalName,
			canBePrefix: interest.Config.CanBePrefix,
		}

		// Schedule the timeout before inserting pi into the live PIT so that
		// pi.timeoutCancel is never nil while pi is visible to onData.
		pi.timeoutCancel = e.timer.Schedule(lifetime+10*time.Millisecond, func() {
			e.pitLock.Lock()
			_, ok := e.pit[token]
			if ok {
				delete(e.pit, token)
			}
			e.pitLock.Unlock()
			if ok {
				callback(ndn.ExpressCallbackArgs{
					Result: ndn.InterestResultTimeout,
				})
			}
		})
		e.pit[token] = pi
		e.pitLock.Unlock()

		// LP-wrap with PIT token (and NextHopFaceId if set)
		lpHdr := &spec.LpPacket{
			PitToken: tokenBytes,
			Fragment: wire,
		}
		if hop, ok := interest.Config.NextHopId.Get(); ok {
			lpHdr.NextHopFaceId.Set(hop)
		}
		lpPkt := &spec.Packet{LpPacket: lpHdr}
		encoder := spec.PacketEncoder{}
		encoder.Init(lpPkt)
		wire = encoder.Encode(lpPkt)
		if wire == nil {
			// Cancel the timeout and remove the PIT entry so the callback is
			// never called after this error return.  Without this cleanup, the
			// scheduled timeout would fire and invoke callback(InterestResultTimeout)
			// even though Express already returned an error — violating the
			// contract that exactly one of {error, callback} is delivered.
			pi.timeoutCancel()
			e.pitLock.Lock()
			delete(e.pit, token)
			e.pitLock.Unlock()
			return fmt.Errorf("failed to encode LP packet")
		}
	}

	return e.face.Send(wire)
}

// ExecMgmtCmd implements management commands by directly manipulating the SimForwarder.
func (e *SimEngine) ExecMgmtCmd(module string, cmd string, args any) (any, error) {
	if e.forwarder == nil {
		return nil, fmt.Errorf("SimEngine: no forwarder attached")
	}

	ca, ok := args.(*mgmt.ControlArgs)
	if !ok || ca == nil {
		return nil, fmt.Errorf("SimEngine: ExecMgmtCmd expects *mgmt.ControlArgs")
	}

	switch module {
	case "fib":
		switch cmd {
		case "add-nexthop":
			if ca.Name == nil {
				return nil, fmt.Errorf("fib/add-nexthop: missing name")
			}
			faceID := ca.FaceId.GetOr(0)
			if faceID == 0 {
				faceID = e.appFaceID
			}
			if e.forwarder.GetFace(faceID) == nil {
				return nil, fmt.Errorf("fib/add-nexthop: face %d does not exist", faceID)
			}
			cost := ca.Cost.GetOr(0)
			e.forwarder.Thread().Fib().InsertNextHopEnc(ca.Name, faceID, cost)
			return &mgmt.ControlResponse{Val: &mgmt.ControlResponseVal{
				StatusCode: 200,
				Params:     &mgmt.ControlArgs{Name: ca.Name, FaceId: optional.Some(faceID), Cost: optional.Some(cost)},
			}}, nil
		case "remove-nexthop":
			if ca.Name == nil {
				return nil, fmt.Errorf("fib/remove-nexthop: missing name")
			}
			faceID := ca.FaceId.GetOr(0)
			if faceID == 0 {
				faceID = e.appFaceID
			}
			e.forwarder.Thread().Fib().RemoveNextHopEnc(ca.Name, faceID)
			return &mgmt.ControlResponse{Val: &mgmt.ControlResponseVal{
				StatusCode: 200,
				Params:     &mgmt.ControlArgs{Name: ca.Name, FaceId: optional.Some(faceID)},
			}}, nil
		case "list":
			entries := e.forwarder.Thread().Fib().GetAllFIBEntries()
			dataset := &mgmt.FibStatus{}
			for _, entry := range entries {
				nextHops := entry.GetNextHops()
				fibEntry := &mgmt.FibEntry{
					Name:           entry.Name(),
					NextHopRecords: make([]*mgmt.NextHopRecord, len(nextHops)),
				}
				for i, nextHop := range nextHops {
					fibEntry.NextHopRecords[i] = &mgmt.NextHopRecord{FaceId: nextHop.Nexthop, Cost: nextHop.Cost}
				}
				dataset.Entries = append(dataset.Entries, fibEntry)
			}
			return dataset, nil
		}
	case "pet":
		return execSimPetMgmtCmd(e.forwarder, cmd, ca, e.appFaceID)
	case "rib":
		switch cmd {
		case "register":
			if ca.Name == nil {
				return nil, fmt.Errorf("rib/register: missing name")
			}
			faceID := ca.FaceId.GetOr(0)
			cost := ca.Cost.GetOr(0)
			origin := ca.Origin.GetOr(0)
			flags := ca.Flags.GetOr(uint64(mgmt.RouteFlagChildInherit))
			if faceID == 0 {
					// Default to app face -- the DV registers prefixes for itself
				faceID = e.appFaceID
			}
			if e.forwarder.GetFace(faceID) == nil {
				return nil, fmt.Errorf("rib/register: face %d does not exist", faceID)
			}
			e.forwarder.AddRouteWithFlags(ca.Name, faceID, cost, origin, flags)
			return &mgmt.ControlResponse{Val: &mgmt.ControlResponseVal{
				StatusCode: 200,
				Params:     &mgmt.ControlArgs{Name: ca.Name, FaceId: optional.Some(faceID)},
			}}, nil
		case "unregister":
			if ca.Name == nil {
				return nil, fmt.Errorf("rib/unregister: missing name")
			}
			faceID := ca.FaceId.GetOr(0)
			origin := ca.Origin.GetOr(0)
			// Mirror rib/register: default to app face when no face ID is specified
			// so that a round-trip register+unregister with faceID=0 is not a no-op.
			if faceID == 0 {
				faceID = e.appFaceID
			}
			if faceID > 0 {
				e.forwarder.RemoveRouteWithOrigin(ca.Name, faceID, origin)
			}
			return &mgmt.ControlResponse{Val: &mgmt.ControlResponseVal{StatusCode: 200}}, nil
		}
	case "faces":
		switch cmd {
		case "update":
			// No-op in simulation (local fields, etc.)
			return &mgmt.ControlResponse{Val: &mgmt.ControlResponseVal{StatusCode: 200}}, nil
		case "create":
			// In simulation, faces are pre-created by ns-3. Return the existing app face.
			return &mgmt.ControlResponse{Val: &mgmt.ControlResponseVal{
				StatusCode: 409, // already exists
				Params:     &mgmt.ControlArgs{FaceId: optional.Some(e.appFaceID)},
			}}, nil
		case "destroy":
				// No-op -- ns-3 manages face lifecycle
			return &mgmt.ControlResponse{Val: &mgmt.ControlResponseVal{StatusCode: 200}}, nil
		}
	case "strategy-choice":
		if cmd == "set" {
			if ca.Strategy == nil || ca.Name == nil {
				return nil, fmt.Errorf("strategy-choice/set: missing name or strategy")
			}
			strategyName := ca.Strategy.Name
			// Resolve versioned strategy name if version is missing
			if len(strategyName) > len(defn.STRATEGY_PREFIX) &&
				!strategyName[len(strategyName)-1].IsVersion() {
				// Look up the strategy and add default version 1
				strategyName = strategyName.Append(enc.NewVersionComponent(1))
			}
			e.forwarder.SetStrategy(ca.Name, strategyName)
			return &mgmt.ControlResponse{Val: &mgmt.ControlResponseVal{StatusCode: 200}}, nil
		}
	case "multicast-strategy-choice":
		if cmd == "set" {
			if ca.Strategy == nil || ca.Name == nil {
				return nil, fmt.Errorf("multicast-strategy-choice/set: missing name or strategy")
			}
			strategyName := ca.Strategy.Name
			if len(strategyName) > len(defn.STRATEGY_PREFIX) &&
				!strategyName[len(strategyName)-1].IsVersion() {
				strategyName = strategyName.Append(enc.NewVersionComponent(1))
			}
			// Use the phase-aware per-node multicast state rather than any
			// process-wide forwarding table.
			e.forwarder.SetMulticastStrategy(ca.Name, strategyName)
			return &mgmt.ControlResponse{Val: &mgmt.ControlResponseVal{StatusCode: 200}}, nil
		}
	}

	return nil, fmt.Errorf("SimEngine: unsupported mgmt cmd %s/%s", module, cmd)
}

// SetCmdSec is a no-op in simulation.
func (e *SimEngine) SetCmdSec(signer ndn.Signer, validator func(enc.Name, enc.Wire, ndn.Signature) bool) {
}

// RegisterRoute installs the producer app-face entry for the given prefix.
// twophase: registers via the PET (prefix egress table).
// onephase: registers via a direct FIB entry to the app face.
func (e *SimEngine) RegisterRoute(prefix enc.Name) error {
	return registerSimRoute(e.forwarder, prefix, e.appFaceID)
}

// UnregisterRoute removes the producer app-face entry installed by RegisterRoute.
// twophase: removes the PET nexthop.
// onephase: removes the direct FIB entry.
func (e *SimEngine) UnregisterRoute(prefix enc.Name) error {
	return unregisterSimRoute(e.forwarder, prefix, e.appFaceID)
}

// Post executes the task synchronously.
func (e *SimEngine) Post(task func()) {
	task()
}

// --- Packet processing (synchronous) ---

func (e *SimEngine) onPacket(frame []byte) {
	reader := enc.NewBufferView(frame)

	var pitToken []byte
	var incomingFaceId optional.Optional[uint64]
	var raw enc.Wire

	pkt, ctx, err := spec.ReadPacket(reader)
	if err != nil {
		return
	}

	if pkt.LpPacket != nil {
		lp := pkt.LpPacket
		if lp.FragIndex.IsSet() || lp.FragCount.IsSet() {
			return // fragmentation not supported
		}

		raw = lp.Fragment
		pitToken = lp.PitToken
		incomingFaceId = lp.IncomingFaceId

		// Parse inner packet
		if len(raw) == 1 {
			pkt, ctx, err = spec.ReadPacket(enc.NewBufferView(raw[0]))
		} else {
			pkt, ctx, err = spec.ReadPacket(enc.NewWireView(raw))
		}
		if err != nil || (pkt.Data == nil) == (pkt.Interest == nil) {
			return
		}
	} else {
		raw = reader.Range(0, reader.Length())
	}

	if pkt.Interest != nil {
		e.onInterest(ndn.InterestHandlerArgs{
			Interest:       pkt.Interest,
			RawInterest:    raw,
			SigCovered:     ctx.Interest_context.SigCovered(),
			PitToken:       pitToken,
			IncomingFaceId: incomingFaceId,
		})
	}
	if pkt.Data != nil {
		// Try to match against pending Interests (Express callbacks)
		e.onData(pkt.Data, raw, ctx.Data_context.SigCovered(), pitToken)

		if e.onDataReceived != nil {
			e.onDataReceived(e.nodeID, uint32(raw.Length()), pkt.Data.Name().String())
		}
	}
}

func (e *SimEngine) onInterest(args ndn.InterestHandlerArgs) {
	name := args.Interest.Name()
	args.Deadline = e.timer.Now().Add(
		args.Interest.Lifetime().GetOr(4 * time.Second))

	handler := func() ndn.InterestHandler {
		e.fibLock.Lock()
		defer e.fibLock.Unlock()
		n := e.fib.prefixMatch(name)
		for n != nil && n.val == nil {
			n = n.par
		}
		if n != nil {
			return n.val
		}
		return nil
	}()

	if handler == nil {
		return
	}

	args.Reply = e.makeReplyFunc(args.PitToken)
	handler(args)
}

func (e *SimEngine) makeReplyFunc(pitToken []byte) ndn.WireReplyFunc {
	return func(dataWire enc.Wire) error {
		if dataWire == nil || !e.running.Load() || !e.face.IsRunning() {
			return ndn.ErrFaceDown
		}

		var outWire enc.Wire = dataWire
		if pitToken != nil {
			lpPkt := &spec.Packet{
				LpPacket: &spec.LpPacket{
					PitToken: pitToken,
					Fragment: dataWire,
				},
			}
			encoder := spec.PacketEncoder{}
			encoder.Init(lpPkt)
			wire := encoder.Encode(lpPkt)
			if wire != nil {
				outWire = wire
			}
		}

		return e.face.Send(outWire)
	}
}

// onData matches incoming Data against pending Interests from Express().
func (e *SimEngine) onData(data ndn.Data, raw enc.Wire, sigCovered enc.Wire, pitToken []byte) {
	// Match by PIT token (preferred -- reliable)
	if len(pitToken) == 8 {
		token := binary.BigEndian.Uint64(pitToken)
		e.pitLock.Lock()
		pi, ok := e.pit[token]
		if ok {
			delete(e.pit, token)
		}
		e.pitLock.Unlock()
		if ok && pi.callback != nil {
			if pi.timeoutCancel != nil {
				pi.timeoutCancel()
			}
			pi.callback(ndn.ExpressCallbackArgs{
				Result:     ndn.InterestResultData,
				Data:       data,
				RawData:    raw,
				SigCovered: sigCovered,
			})
			return
		}
	}

	// Fall back to name-based matching (scan all pending Interests).
	// This path should rarely trigger -- log a warning so we can confirm.
	log.Warn(nil, "Data matched by name fallback (no PIT token match)",
		"name", data.Name().String())
	dataName := data.Name()
	e.pitLock.Lock()
	var matchToken uint64
	var matchPI *pendingInterest
	for tok, pi := range e.pit {
		if pi.canBePrefix {
			// NDN CanBePrefix: Interest name is a prefix of the Data name.
			// (NOT the reverse: dataName.IsPrefix(pi.name) would check
			// whether the Data name is a prefix of the Interest name,
			// which is semantically wrong and could match spuriously.)
			if pi.name.IsPrefix(dataName) {
				matchToken = tok
				matchPI = pi
				break
			}
		} else {
			if pi.name.Equal(dataName) {
				matchToken = tok
				matchPI = pi
				break
			}
		}
	}
	if matchPI != nil {
		delete(e.pit, matchToken)
	}
	e.pitLock.Unlock()

	if matchPI != nil && matchPI.callback != nil {
		if matchPI.timeoutCancel != nil {
			matchPI.timeoutCancel()
		}
		matchPI.callback(ndn.ExpressCallbackArgs{
			Result:     ndn.InterestResultData,
			Data:       data,
			RawData:    raw,
			SigCovered: sigCovered,
		})
	}
}

// --- Minimal name trie for Interest dispatch ---

type nameTrie struct {
	val ndn.InterestHandler
	par *nameTrie
	dep int
	key string
	chd map[string]*nameTrie
}

func newNameTrie() *nameTrie {
	return &nameTrie{chd: map[string]*nameTrie{}}
}

func (n *nameTrie) exactMatch(name enc.Name) *nameTrie {
	if len(name) <= n.dep {
		return n
	}
	c := name[n.dep].TlvStr()
	if ch, ok := n.chd[c]; ok {
		return ch.exactMatch(name)
	}
	return nil
}

func (n *nameTrie) prefixMatch(name enc.Name) *nameTrie {
	if len(name) <= n.dep {
		return n
	}
	c := name[n.dep].TlvStr()
	if ch, ok := n.chd[c]; ok {
		return ch.prefixMatch(name)
	}
	return n
}

func (n *nameTrie) matchAlways(name enc.Name) *nameTrie {
	if len(name) <= n.dep {
		return n
	}
	c := name[n.dep].TlvStr()
	ch, ok := n.chd[c]
	if !ok {
		ch = &nameTrie{
			par: n,
			dep: n.dep + 1,
			key: c,
			chd: map[string]*nameTrie{},
		}
		n.chd[c] = ch
	}
	return ch.matchAlways(name)
}

// prune removes this leaf node and any childless nil-valued ancestors
// walking up to (but not including) the root.
func (n *nameTrie) prune() {
	for n.par != nil && n.val == nil && len(n.chd) == 0 {
		delete(n.par.chd, n.key)
		n = n.par
	}
}
