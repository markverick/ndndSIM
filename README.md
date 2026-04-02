# ndndSIM — NDN Simulation Module for ns-3

ndndSIM is an ns-3 contrib module that provides Named Data Networking (NDN)
simulation using the [NDNd](https://github.com/named-data/ndnd) Go forwarder.
It bridges NDNd's production forwarding engine with ns-3's discrete-event
simulation via CGo, giving you real NDN forwarding logic—FIB, PIT, CS,
distance-vector routing—inside a fully controllable simulation environment.

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│  ns-3 C++ simulation                                     │
│                                                          │
│   NdndConsumer ─┐         ┌─ NdndProducer                │
│   NdndConsumerZipf        │                              │
│                 ▼         ▼                               │
│              NdndApp (Application base)                   │
│                      │                                    │
│              NdndStack (per-node)                         │
│                 │    ▲                                    │
│   C bridge ─────┘    └──── ns-3 NetDevices               │
│ ─ ─ ─ ─ ─ CGo boundary ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ │
│                                                          │
│   Go: SimForwarder → fw.Thread (FIB, PIT, CS, DV)       │
│        SimFace / DispatchFace per interface               │
│        SimEngine – synchronous packet dispatch            │
│        Ns3Clock – simulation time via core.NowFunc        │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

Each simulated node gets a real NDNd `fw.Thread` with its own FIB, PIT, and
content store. Packets traverse ns-3 channels (point-to-point, CSMA, WiFi) as
Ethernet frames with EtherType `0x8624`. Simulation time replaces wall-clock
time so the Go forwarder runs deterministically under ns-3's scheduler.

## Prerequisites

| Dependency | Version | Notes |
|------------|---------|-------|
| ns-3       | 3.43+   | Tested with 3.47 |
| Go         | 1.22+   | CGo must be enabled (`CGO_ENABLED=1`) |
| C++ compiler | GCC 11+ or Clang 15+ | Must match the compiler ns-3 uses |
| CMake      | 3.16+   | |

## Building

ndndSIM lives in `contrib/ndndSIM/` inside your ns-3 tree. The NDNd Go
forwarder is included as a git submodule.

```bash
# Clone (if you haven't already)
cd ns-3-dev-git/contrib
git clone <ndndSIM-repo-url> ndndSIM
cd ndndSIM
git submodule update --init

# Configure (only needed once)
cd ../..
./ns3 configure --enable-examples --enable-tests

# Build
./ns3 build
```

The first `./ns3 configure` automatically patches `PointToPointNetDevice` to
add NDN EtherType support (`0x8624` ↔ PPP `0x0077`). This is required because
ns-3's P2P device only recognises IPv4/IPv6 by default and will assert on any
other protocol. The patch is idempotent — subsequent configures skip it if
already applied.

The build produces:
- `libndndsim_go.a` — Go forwarder compiled as a C archive
- `libns3.XX-ndndSIM-*.so` — ns-3 shared library module

## Quick Start

Minimal 3-node scenario: Consumer → Router → Producer

```cpp
#include "ns3/core-module.h"
#include "ns3/ndndSIM-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

using namespace ns3;

int main(int argc, char* argv[])
{
    // Create nodes
    NodeContainer nodes;
    nodes.Create(3);

    // Point-to-point links
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("10ms"));
    p2p.Install(nodes.Get(0), nodes.Get(1));
    p2p.Install(nodes.Get(1), nodes.Get(2));

    // Install NDNd stack + routing
    ndndsim::NdndStackHelper stackHelper;
    stackHelper.Install(nodes);
    stackHelper.AddRoutesToAll("/ndn/test", nodes);

    // Consumer on node 0
    ndndsim::NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue("/ndn/test"));
    consumerHelper.SetAttribute("Frequency", DoubleValue(10.0));
    consumerHelper.Install(nodes.Get(0)).Start(Seconds(1.0));

    // Producer on node 2
    ndndsim::NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue("/ndn/test"));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(1024));
    producerHelper.Install(nodes.Get(2)).Start(Seconds(0.5));

    Simulator::Stop(Seconds(10.0));
    Simulator::Run();
    Simulator::Destroy();
    stackHelper.DestroyBridge();
    return 0;
}
```

## API Reference

### Core Model

#### `NdndStack` — Per-Node NDN Protocol Stack

Aggregated onto each `ns3::Node`. Manages the Go-side forwarder, faces, and
routing table for that node.

```cpp
Ptr<NdndStack> stack = node->GetObject<NdndStack>();
stack->GetFaceId(ifIndex);                  // Get face ID for a NetDevice
stack->AddRoute("/ndn/prefix", faceId, 1);  // Add FIB entry
stack->RemoveRoute("/ndn/prefix", faceId);  // Remove FIB entry
```

#### `NdndApp` — Application Base Class

Base class for all NDN applications. Subclass it and override `OnStart()` /
`OnStop()` to create custom apps. Provides `GetStack()` to access the node's
NDN stack.

```cpp
class MyApp : public NdndApp
{
  protected:
    void OnStart() override;
    void OnStop() override;
};
```

See the [custom app example](#custom-app) for a complete walkthrough.

### Applications

#### `NdndConsumer` — Periodic Interest Sender

Sends Interests at a configurable rate with sequentially numbered names.

| Attribute   | Type     | Default       | Description |
|-------------|----------|---------------|-------------|
| `Prefix`    | string   | `"/ndn/test"` | NDN name prefix for Interests |
| `Frequency` | double   | `1.0`         | Interests per second |
| `LifeTime`  | Time     | `2s`          | Interest lifetime (expiry in PIT) |
| `Randomize` | string   | `"none"`      | Inter-Interest gap randomization: `"none"`, `"uniform"`, or `"exponential"` |

**Trace sources:**
- `InterestSent(uint32_t seqNo)` — fired on each Interest sent.
- `DataReceived(uint32_t dataSize)` — fired on each Data received.

#### `NdndConsumerZipf` — Zipf-Mandelbrot Consumer

Sends Interests with content names chosen from a Zipf-Mandelbrot distribution.
Low-numbered content items are requested more frequently than high-numbered ones,
modeling realistic content popularity.

| Attribute          | Type     | Default       | Description |
|--------------------|----------|---------------|-------------|
| `Prefix`           | string   | `"/ndn/zipf"` | NDN name prefix |
| `Frequency`        | double   | `1.0`         | Interests per second |
| `NumberOfContents` | uint32_t | `100`         | Total content items |
| `q`                | double   | `0.0`         | Zipf q-parameter |
| `s`                | double   | `0.7`         | Zipf s-parameter (skewness) |

The Zipf-Mandelbrot PMF is:

$$P(k) = \frac{1/(k+q)^s}{\sum_{i=1}^{N} 1/(i+q)^s}$$

**Trace sources:**
- `InterestSent(uint32_t seqNo)` — fired on each Interest sent.
- `DataReceived(uint32_t dataSize)` — fired on each Data received (inherited from `NdndConsumer`).

#### `NdndProducer` — Data Producer

Listens for Interests matching its prefix and replies with Data packets.

| Attribute     | Type     | Default       | Description |
|---------------|----------|---------------|-------------|
| `Prefix`      | string   | `"/ndn/test"` | Name prefix to serve |
| `PayloadSize` | uint32_t | `1024`        | Data payload size in bytes |
| `Freshness`   | Time     | `0ms`         | FreshnessPeriod set on produced Data |

**Trace source:** `DataSent(uint32_t payloadSize)` — fired on each Data sent.

### Helpers

#### `NdndStackHelper` — Stack Installation

Installs the NDN stack on nodes and configures routing.

```cpp
ndndsim::NdndStackHelper stackHelper;

// Initialize the Go bridge (called automatically on first Install)
stackHelper.InitializeBridge();

// Install on nodes
stackHelper.Install(nodes);                           // NodeContainer
Ptr<NdndStack> stack = stackHelper.Install(node);     // Single node

// Static routing
stackHelper.AddRoutesToAll("/prefix", nodes);          // All-to-all routes
stackHelper.AddRoute(node, "/prefix", ifIndex, cost);  // By interface index
stackHelper.AddRoute(node, "/prefix", faceId, cost);   // By face ID

// Dijkstra shortest-path routing (prefix → producer nodes → all nodes)
NodeContainer producers;
producers.Add(producerNode);
stackHelper.CalculateRoutes("/prefix", producers, allNodes);

// DV routing (automatic route discovery via NDNd distance-vector protocol)
stackHelper.EnableDvRouting("/ndn", allNodes);

// Dynamic prefix announce/withdraw (DV routing only)
stackHelper.AnnouncePrefixToDv(node, "/ndn/service/foo");
stackHelper.WithdrawPrefixFromDv(node, "/ndn/service/foo");

// Cleanup (call after Simulator::Destroy)
stackHelper.DestroyBridge();
```

**Routing strategies:**

| Strategy | Method | When to use |
|----------|--------|-------------|
| Manual | `AddRoute()`, `AddRoutesToAll()` | Full control over FIB entries |
| Dijkstra | `CalculateRoutes(prefix, producers, nodes)` | Metric-weighted shortest paths computed before simulation |
| DV | `EnableDvRouting(routerPrefix, nodes)` | Routes discovered at runtime via NDNd's distance-vector protocol with SVS sync |

When using DV routing, start the consumer after a convergence delay (typically
3–5 seconds for small topologies) to allow route advertisements to propagate.
Producers automatically announce their prefixes to the DV protocol.

**DV configuration options** (passed via `NdndStackHelper::EnableDvRouting()`):

| Parameter | JSON key | Description |
|-----------|----------|-------------|
| Advertisement interval | `advertise_interval` | Period (ms) for sending DV sync Interests. `0` = default. |
| Router dead interval | `router_dead_interval` | Timeout (ms) before declaring a neighbor dead. `0` = default. |
| One-step mode | `one_step` | If `true`, prefixes go directly into DV adverts — no PrefixSync. |
| PrefixSync delay | `prefix_sync_delay` | Delay (ms) before starting PrefixSync SVS. Useful for large topologies where DV needs time to converge before SVS starts. `0` = start immediately. |

#### `NdndAppHelper` — Application Factory

Creates and installs NDN applications on nodes.

```cpp
ndndsim::NdndAppHelper helper("ns3::ndndsim::NdndConsumer");
helper.SetAttribute("Prefix", StringValue("/ndn/test"));
helper.SetAttribute("Frequency", DoubleValue(100.0));

ApplicationContainer apps = helper.Install(nodes);
apps.Start(Seconds(1.0));
apps.Stop(Seconds(10.0));
```

#### `NdndTopologyReader` — Topology File Reader

Reads topology files in the standard ndnSIM annotated format and creates the
corresponding ns-3 nodes and point-to-point links.

```cpp
ndndsim::NdndTopologyReader reader;
reader.SetFileName("path/to/topology.txt");
NodeContainer nodes = reader.Read();

// Access link details
for (const auto& link : reader.GetLinks()) {
    // link.fromName, link.toName, link.dataRate, link.delay, ...
}
```

Nodes are registered in the ns-3 naming system (`Names::Find<Node>("NodeName")`)
and given `ConstantPositionMobilityModel` with coordinates from the file.

**Topology file format:**

```
# Anything after '#' is a comment

router

# node    comment    yPos    xPos
Node0     NA         0       0
Node1     NA         0       1
Node2     NA         1       0

link

# srcNode  dstNode  bandwidth  metric  delay  queue  [lossRate]
Node0      Node1    10Mbps     1       1ms    100
Node1      Node2    10Mbps     1       1ms    100    0.01
```

The `router` section defines nodes with optional grid positions. The `link`
section defines point-to-point connections. An optional `lossRate` column
(0.0–1.0) installs a `RateErrorModel` on the link.

#### `NdndRateTracer` — CSV Rate Statistics

Periodically writes per-node packet/byte counters to a CSV file. Connects to
the `InterestSent` and `DataSent` trace sources on Consumer and Producer
applications.

```cpp
// Trace all nodes, write every 0.5 seconds
ndndsim::NdndRateTracer::InstallAll("rates.csv", Seconds(0.5));

// Or trace specific nodes
ndndsim::NdndRateTracer::Install(someNodes, "rates.csv", Seconds(1.0));
```

**CSV output format:**

```csv
Time,Node,Type,Packets,KBytes
0.5,0,InterestSent,50,0
0.5,1,DataSent,50,51.2
1,0,InterestSent,50,0
```

Counters reset each period, so values represent the count within that interval.

#### `NdndLinkTracer` — Classified Link Traffic

Measures link-level traffic classified by NDN packet type. Connects to
the `MacTx` trace source on any `NetDevice` and parses the raw NDN TLV
(unwrapping NDNLPv2 framing) to classify each packet into one of six
categories.

| Category | Description |
|----------|-------------|
| `DvAdvert` | DV advertisement sync & data (`/localhop/.../32=DV/...`) |
| `PrefixSync` | DV prefix-table sync (`/.../32=DV/32=PFS/...`) |
| `Mgmt` | Local management (`/localhost/nlsr/...`) |
| `UserInterest` | Application-level Interest |
| `UserData` | Application-level Data |
| `Other` | Unrecognised / malformed |

```cpp
auto linkTracer = ndndsim::NdndLinkTracer::Create("link-traffic.csv", Seconds(1.0));
linkTracer->ConnectLink(p2pDevices);  // NetDeviceContainer
linkTracer->ConnectDevice(singleDev); // Individual device
// ...
linkTracer->Stop();
```

The tracer is L2-agnostic — works with PointToPoint, CSMA, WiFi, etc.
It uses an `NdnPayloadTag` (set internally by the Go bridge) to skip
the L2 header regardless of its length.

**CSV output format:**

```csv
Time,DvAdvert_Pkts,DvAdvert_Bytes,PrefixSync_Pkts,PrefixSync_Bytes,Mgmt_Pkts,Mgmt_Bytes,UserInterest_Pkts,UserInterest_Bytes,UserData_Pkts,UserData_Bytes,Other_Pkts,Other_Bytes
1,1078,233078,1549,218324,0,0,0,0,0,0,0,0
2,64,13384,0,0,0,0,0,0,0,0,0,0
```

Pair with `plot-ndndsim-traffic.py` to visualise:

```bash
python3 contrib/ndndSIM/examples/plot-ndndsim-traffic.py link-traffic.csv \
    -e events.csv -o traffic.png

# Control-plane only (exclude user traffic)
python3 contrib/ndndSIM/examples/plot-ndndsim-traffic.py link-traffic.csv \
    --exclude UserInterest UserData Other -o control-plane.png
```

## Examples

All examples are in `contrib/ndndSIM/examples/`. Run any example with:

```bash
./ns3 run ndndsim-<name>
```

### Basic Topologies

| Example | Description |
|---------|-------------|
| `ndndsim-simple` | 3-node linear: Consumer → Router → Producer |
| `ndndsim-grid` | 3×3 point-to-point grid |
| `ndndsim-tree` | Binary tree with 4 consumers and 1 producer |
| `ndndsim-csma` | 3 nodes on a shared CSMA bus |
| `ndndsim-wifi` | 2 nodes over 802.11a WiFi Ad-Hoc |

### DV Routing Scenarios

| Example | Description |
|---------|-------------|
| `ndndsim-grid-dv` | 3×3 grid with DV routing — automatic route discovery |
| `ndndsim-dv-multipath` | Diamond topology with redundant paths via DV |
| `ndndsim-dv-convergence` | 4×4 grid showing DV convergence ramp-up (configurable grid size, link delay, sim time) |

### Advanced Scenarios

| Example | Description |
|---------|-------------|
| `ndndsim-link-failure` | Link goes down at t=5s, recovers at t=8s |
| `ndndsim-custom-app` | Writing a custom `NdndApp` subclass (Ping app) |
| `ndndsim-zipf` | Zipf-Mandelbrot content popularity distribution |
| `ndndsim-topo-reader` | Read topology from file (supports `--topo`, `--consumer`, `--producer` CLI args) |
| `ndndsim-tree-tracers` | Tree topology from file + CSV rate tracing |
| `ndndsim-sprint-churn` | Sprint PoP topology with random link flaps, prefix add/remove, DV routing, and classified link traffic measurement |
| `ndndsim-atlas-scenario` | Fully parameterised grid scenario for DV convergence and traffic measurement (used by [atlas-scenarios](https://github.com/markverick/atlas-scenarios)) |
| `ndndsim-atlas-routing-scenario` | Routing-only measurement (no app traffic): DV-only, one-step, and two-step prefix routing on NxN grids or conf-based topologies |
| `ndndsim-atlas-churn-scenario` | Churn measurement: DV + prefix routing under link failure/recovery and prefix withdraw/re-announce events. Supports deferred churn scheduling (events start after DV convergence callback) and prefix-scaling mode |

### Topology Files

Two topology files are provided in `examples/topologies/`:

- **`topo-grid-3x3.txt`** — 9 nodes in a 3×3 grid, 1 Mbps / 10 ms links
- **`topo-tree.txt`** — 7-node binary tree, 10 Mbps / 1 ms links

### Custom App Example

The `ndndsim-custom-app` example shows how to subclass `NdndApp`:

```cpp
class NdndPing : public ndndsim::NdndApp
{
  public:
    static TypeId GetTypeId();  // Register attributes

  protected:
    void OnStart() override
    {
        NdndApp::OnStart();
        // Schedule your first event
        m_sendEvent = Simulator::Schedule(Seconds(0), &NdndPing::SendPing, this);
    }

    void OnStop() override
    {
        Simulator::Cancel(m_sendEvent);
        NdndApp::OnStop();
    }

  private:
    void SendPing();  // Build and send an Interest via the Go bridge
};
```

## Writing a Custom Application

1. **Inherit from `NdndApp`** — gives you `GetStack()` and lifecycle hooks.
2. **Override `OnStart()`** — schedule your first event. Always call
   `NdndApp::OnStart()` first.
3. **Override `OnStop()`** — cancel pending events. Always call
   `NdndApp::OnStop()` last.
4. **Register a TypeId** — use `AddConstructor<>()` and set the group to `"ndndSIM"`.
5. **Send packets** — encode an NDN Interest/Data as a TLV byte buffer and call
   `NdndSimReceivePacket()` via the Go bridge to inject it into the forwarder.

## Security & Trust

When DV routing is enabled, ndndSIM uses the same Ed25519 trust pipeline as
the real NDNd forwarder. This ensures that simulated DV advertisement and
prefix-sync packets are signed and validated with the same overhead as in
emulation or deployment.

**How it works:**

1. A shared Ed25519 **root key** is generated once per simulation run
   (identity: `/<network>/KEY/<random>`, self-signed, 10-year validity).
2. Each node receives its own Ed25519 **node key** (identity:
   `/<network>/<routerName>/32=DV`), with a certificate signed by the root.
3. All nodes load the root certificate as a trust anchor plus their own
   keychain (private key + certificate).
4. DV advertisements and prefix-sync messages are signed with the node key
   and validated against the trust schema (`#network_cert → #router_cert`).

This is handled automatically by `EnableDvRouting()` — no extra configuration
is needed. The signing overhead (~74 bytes for Ed25519 vs SHA-256 digest)
matches what the real forwarder produces, so traffic measurements are
faithful to real deployments.

## Running Tests

### C++ Test Suite (ns-3)

```bash
# Run the full ndndSIM test suite (33 tests)
./ns3 run test-runner -- --suite=ndndsim --verbose

# With extensive (integration) tests
./ns3 run test-runner -- --suite=ndndsim --verbose --fullness=EXTENSIVE
```

Tests cover TypeId registration, object lifecycle, attribute configuration
(Consumer LifeTime/Randomize, Producer Freshness, Zipf parameters),
stack installation, static routing, Dijkstra shortest-path routing,
DV routing (init, end-to-end, grid, multi-producer, Ed25519 trust),
end-to-end trace verification (InterestSent + DataReceived + DataSent),
multi-prefix topologies, topology reader correctness, sequence ordering,
multi-consumer scenarios, EtherType filtering, cleanup, app lifecycle
timing, and link failure/recovery.

### Go Test Suite (pure-Go simulation)

The `ndnd/sim/` package has its own test suite that exercises the Go
simulation bridge **without ns-3** using a `DeterministicClock`:

```bash
cd ndnd && go test -count=1 -v ./sim/
```

28 tests covering:

| Category | Tests | What they verify |
|----------|-------|------------------|
| Clock | 9 | Event scheduling, cancel, firing order, cross-clock isolation, self-rescheduling heartbeats |
| Consumer | 3 | Interest loop, counting, stop |
| DV integration | 16 | Two-node / three-node / diamond / line topologies, prefix withdrawal, producer mobility, link partition, link flap recovery, 3×3 grid reachability, link propagation delay, one-step mode, multiple prefixes, convergence time bound |

Key design: `core.NowFunc` is overridden to use `DeterministicClock.Now()` so
PIT expiration, CS freshness, and best-route strategy suppression all operate
in simulated time — matching the ns-3 path which overrides `core.NowFunc` via
`NdndSimGlobalInit`. Certificates are pre-populated across all nodes via
`SimTrust.PreGenerateCerts()` to eliminate timing-dependent cert fetches.

## Module Structure

```
contrib/ndndSIM/
├── CMakeLists.txt              # Build: Go archive + ns-3 module
├── README.md                   # This file
├── model/
│   ├── ndndsim-go-bridge.h/cc # C ↔ Go bridge via CGo
│   ├── ndndsim-stack.h/cc     # Per-node NDN stack (NdndStack)
│   └── ndndsim-app.h/cc       # Application base class (NdndApp)
├── apps/
│   ├── ndndsim-consumer.h/cc       # Periodic Interest consumer
│   ├── ndndsim-consumer-zipf.h/cc  # Zipf-Mandelbrot consumer
│   └── ndndsim-producer.h/cc       # Data producer
├── helper/
│   ├── ndndsim-stack-helper.h/cc      # Stack install + routing
│   ├── ndndsim-app-helper.h/cc        # App factory helper
│   ├── ndndsim-topology-reader.h/cc   # Topology file reader
│   ├── ndndsim-rate-tracer.h/cc       # CSV rate tracer
│   └── ndndsim-link-tracer.h/cc       # Classified link traffic tracer
├── examples/
│   ├── ndndsim-simple.cc         # 3-node linear
│   ├── ndndsim-tree.cc           # Binary tree
│   ├── ndndsim-grid.cc           # 3×3 grid (Dijkstra routing)
│   ├── ndndsim-grid-dv.cc        # 3×3 grid (DV routing)
│   ├── ndndsim-dv-multipath.cc   # Diamond topology (DV multipath)
│   ├── ndndsim-dv-convergence.cc # 4×4 grid (DV convergence)
│   ├── ndndsim-csma.cc           # CSMA bus
│   ├── ndndsim-wifi.cc           # WiFi Ad-Hoc
│   ├── ndndsim-link-failure.cc   # Link failure scenario
│   ├── ndndsim-custom-app.cc     # Custom app subclass
│   ├── ndndsim-zipf.cc           # Zipf consumer
│   ├── ndndsim-topo-reader.cc    # Topology file reader
│   ├── ndndsim-tree-tracers.cc   # Tree + CSV tracing
│   ├── ndndsim-sprint-churn.cc   # Sprint PoP topology + link/prefix churn
│   ├── ndndsim-atlas-scenario.cc          # Parameterised grid scenario (atlas)
│   ├── ndndsim-atlas-routing-scenario.cc  # Routing-only (one-step vs two-step)
│   ├── ndndsim-atlas-churn-scenario.cc    # Churn scenario (link/prefix events)
│   ├── plot-ndndsim-traffic.py   # Traffic composition plotter
│   └── topologies/               # Topology definition files
├── test/
│   └── ndndsim-test-suite.cc     # 33 unit/integration tests
└── ndnd/                         # NDNd Go forwarder (submodule)
    └── sim/                      # Go simulation bridge package
        ├── engine.go             #   SimEngine: synchronous packet dispatch
        ├── forwarder.go          #   SimForwarder: fw.Thread lifecycle
        ├── node.go               #   Per-node setup (faces, DV, trust)
        ├── trust.go              #   Ed25519 root + per-node keychain (PreGenerateCerts)
        ├── clock_test.go         #   DeterministicClock unit tests (9)
        ├── consumer_test.go      #   Consumer loop tests (3)
        └── dv_integration_test.go #  End-to-end DV integration tests (16)
```

## How It Works

1. **Build time**: The Go forwarder is compiled into a C archive (`libndndsim_go.a`)
   using `go build -buildmode=c-archive`. This archive contains the full NDNd
   forwarding engine.

2. **Initialization**: `NdndStackHelper::InitializeBridge()` registers C++
   callback functions (send packet, schedule event, cancel event, get time)
   with the Go runtime.

3. **Per-node setup**: `NdndStack::Install()` calls `NdndSimCreateNode()` in Go,
   which creates a `fw.Thread` (with FIB, PIT, CS), and one `SimFace` per
   NetDevice. Each face gets a unique global ID.

4. **Packet flow**:
   - **Outgoing**: Application → Go bridge (`NdndSimReceivePacket`) → forwarder
     → `SimFace.SendPacket()` → C callback → `NdndStack::ReceiveFromDevice()`
     → ns-3 `NetDevice::Send()`
   - **Incoming**: ns-3 `NetDevice::Receive()` → `NdndStack::ReceiveFromDevice()`
     → Go bridge (`NdndSimReceivePacket`) → forwarder PIT/CS/FIB lookup →
     forward or satisfy

5. **Time synchronization**: The Go side calls back into C++ for the current
   simulation time (`Simulator::Now()`) and for scheduling/canceling events
   (`Simulator::Schedule()`, `Simulator::Cancel()`).

6. **Routing**: Three routing strategies are available:
   - **Manual**: `AddRoute()` inserts individual FIB entries.
   - **Dijkstra**: `CalculateRoutes()` computes metric-weighted shortest paths
     from every node to the producer nodes and installs FIB entries.
   - **DV (Distance-Vector)**: `EnableDvRouting()` starts NDNd's real DV
     protocol with SVS-based state synchronization. Routes are discovered
     automatically—application prefixes are announced by producers and
     propagated through the network via DV advertisements.

7. **Security**: When DV routing is used, the Go side generates an Ed25519
   root key and per-node keychains (see [Security & Trust](#security--trust)).
   The forwarding pipeline signs DV packets and validates incoming ones,
   producing the same wire-level overhead as a real deployment.

8. **NextHopFaceId**: Applications can specify a preferred next-hop face
   (e.g. for certificate fetching). The engine wraps this as an NDNLPv2
   `NextHopFaceId` field, and the forwarder extracts it for the forwarding
   pipeline — matching the behaviour of the real NDNd forwarder.

## License

See the top-level ns-3 [LICENSE](../../LICENSE) file.
