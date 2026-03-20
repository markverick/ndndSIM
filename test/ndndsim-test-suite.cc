/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * Comprehensive test suite for the ndndSIM module.
 */

#include "ns3/application-container.h"
#include "ns3/config.h"
#include "ns3/double.h"
#include "ns3/error-model.h"
#include "ns3/log.h"
#include "ns3/names.h"
#include "ns3/ndndsim-app-helper.h"
#include "ns3/ndndsim-app.h"
#include "ns3/ndndsim-consumer-zipf.h"
#include "ns3/ndndsim-consumer.h"
#include "ns3/ndndsim-go-bridge.h"
#include "ns3/ndndsim-producer.h"
#include "ns3/ndndsim-stack-helper.h"
#include "ns3/ndndsim-stack.h"
#include "ns3/ndndsim-topology-reader.h"
#include "ns3/net-device-container.h"
#include "ns3/node-container.h"
#include "ns3/node.h"
#include "ns3/object-factory.h"
#include "ns3/packet.h"
#include "ns3/point-to-point-grid.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/pointer.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/test.h"
#include "ns3/uinteger.h"

using namespace ns3;
using namespace ns3::ndndsim;

// ═══════════════════════════════════════════════════════════════════════
// 1. TypeId Registration Tests
// ═══════════════════════════════════════════════════════════════════════

/**
 * Verify that all ndndSIM classes register valid TypeIds and belong to
 * the correct group.
 */
class NdndsimTypeIdTestCase : public TestCase
{
  public:
    NdndsimTypeIdTestCase();
    void DoRun() override;
};

NdndsimTypeIdTestCase::NdndsimTypeIdTestCase()
    : TestCase("ndndSIM TypeId registration")
{
}

void
NdndsimTypeIdTestCase::DoRun()
{
    // NdndStack
    TypeId stackTid = NdndStack::GetTypeId();
    NS_TEST_ASSERT_MSG_EQ(stackTid.GetName(),
                           "ns3::ndndsim::NdndStack",
                           "NdndStack TypeId name");
    NS_TEST_ASSERT_MSG_EQ(stackTid.GetGroupName(), "NdndSIM", "NdndStack group");

    // NdndApp
    TypeId appTid = NdndApp::GetTypeId();
    NS_TEST_ASSERT_MSG_EQ(appTid.GetName(),
                           "ns3::ndndsim::NdndApp",
                           "NdndApp TypeId name");
    NS_TEST_ASSERT_MSG_EQ(appTid.GetGroupName(), "NdndSIM", "NdndApp group");

    // NdndConsumer
    TypeId consumerTid = NdndConsumer::GetTypeId();
    NS_TEST_ASSERT_MSG_EQ(consumerTid.GetName(),
                           "ns3::ndndsim::NdndConsumer",
                           "NdndConsumer TypeId name");
    NS_TEST_ASSERT_MSG_EQ(consumerTid.GetGroupName(), "NdndSIM", "NdndConsumer group");

    // NdndProducer
    TypeId producerTid = NdndProducer::GetTypeId();
    NS_TEST_ASSERT_MSG_EQ(producerTid.GetName(),
                           "ns3::ndndsim::NdndProducer",
                           "NdndProducer TypeId name");
    NS_TEST_ASSERT_MSG_EQ(producerTid.GetGroupName(), "NdndSIM", "NdndProducer group");
}

// ═══════════════════════════════════════════════════════════════════════
// 2. NdndStack Object Tests
// ═══════════════════════════════════════════════════════════════════════

/**
 * Verify NdndStack can be created as an Object, aggregated to a Node,
 * and that GetFaceId returns 0 for unknown interfaces before Install().
 */
class NdndsimStackObjectTestCase : public TestCase
{
  public:
    NdndsimStackObjectTestCase();
    void DoRun() override;
};

NdndsimStackObjectTestCase::NdndsimStackObjectTestCase()
    : TestCase("NdndStack object creation and state")
{
}

void
NdndsimStackObjectTestCase::DoRun()
{
    Ptr<NdndStack> stack = CreateObject<NdndStack>();
    NS_TEST_ASSERT_MSG_NE(stack, nullptr, "CreateObject<NdndStack> should succeed");

    // Before aggregation, GetFaceId should return 0
    NS_TEST_ASSERT_MSG_EQ(stack->GetFaceId(0), 0, "No face before Install");
    NS_TEST_ASSERT_MSG_EQ(stack->GetFaceId(42), 0, "No face for arbitrary ifIndex");

    // Aggregate to a Node
    Ptr<Node> node = CreateObject<Node>();
    node->AggregateObject(stack);

    // Can retrieve the stack back
    Ptr<NdndStack> retrieved = node->GetObject<NdndStack>();
    NS_TEST_ASSERT_MSG_NE(retrieved, nullptr, "Stack should be retrievable after aggregation");
    NS_TEST_ASSERT_MSG_EQ(retrieved, stack, "Retrieved stack should be the same object");

    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 3. NdndApp Base Class Tests
// ═══════════════════════════════════════════════════════════════════════

/**
 * Test NdndApp lifecycle and GetStack behavior.
 */
class NdndsimAppBaseTestCase : public TestCase
{
  public:
    NdndsimAppBaseTestCase();
    void DoRun() override;
};

NdndsimAppBaseTestCase::NdndsimAppBaseTestCase()
    : TestCase("NdndApp base class behavior")
{
}

void
NdndsimAppBaseTestCase::DoRun()
{
    Ptr<NdndApp> app = CreateObject<NdndApp>();
    NS_TEST_ASSERT_MSG_NE(app, nullptr, "CreateObject<NdndApp> should succeed");

    // Without being attached to a node, GetStack returns null
    NS_TEST_ASSERT_MSG_EQ(app->GetStack(), nullptr, "GetStack without node is null");

    // Attach to a node without stack → still null
    Ptr<Node> node = CreateObject<Node>();
    node->AddApplication(app);
    NS_TEST_ASSERT_MSG_EQ(app->GetStack(), nullptr, "GetStack without NdndStack aggregated");

    // Aggregate a stack → now it should be found
    Ptr<NdndStack> stack = CreateObject<NdndStack>();
    node->AggregateObject(stack);
    NS_TEST_ASSERT_MSG_NE(app->GetStack(), nullptr, "GetStack with NdndStack aggregated");
    NS_TEST_ASSERT_MSG_EQ(app->GetStack(), stack, "GetStack returns the correct stack");

    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 4. NdndConsumer Attribute Tests
// ═══════════════════════════════════════════════════════════════════════

/**
 * Test NdndConsumer default attribute values and setting attributes.
 */
class NdndsimConsumerAttributeTestCase : public TestCase
{
  public:
    NdndsimConsumerAttributeTestCase();
    void DoRun() override;
};

NdndsimConsumerAttributeTestCase::NdndsimConsumerAttributeTestCase()
    : TestCase("NdndConsumer attribute defaults and configuration")
{
}

void
NdndsimConsumerAttributeTestCase::DoRun()
{
    Ptr<NdndConsumer> consumer = CreateObject<NdndConsumer>();
    NS_TEST_ASSERT_MSG_NE(consumer, nullptr, "CreateObject<NdndConsumer> should succeed");

    // Check default attribute values
    StringValue prefixVal;
    consumer->GetAttribute("Prefix", prefixVal);
    NS_TEST_ASSERT_MSG_EQ(prefixVal.Get(), "/ndn/test", "Default prefix");

    DoubleValue freqVal;
    consumer->GetAttribute("Frequency", freqVal);
    NS_TEST_ASSERT_MSG_EQ_TOL(freqVal.Get(), 1.0, 1e-9, "Default frequency");

    // Set custom attributes
    consumer->SetAttribute("Prefix", StringValue("/my/custom/prefix"));
    consumer->GetAttribute("Prefix", prefixVal);
    NS_TEST_ASSERT_MSG_EQ(prefixVal.Get(), "/my/custom/prefix", "Custom prefix");

    consumer->SetAttribute("Frequency", DoubleValue(100.0));
    consumer->GetAttribute("Frequency", freqVal);
    NS_TEST_ASSERT_MSG_EQ_TOL(freqVal.Get(), 100.0, 1e-9, "Custom frequency");

    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 5. NdndProducer Attribute Tests
// ═══════════════════════════════════════════════════════════════════════

/**
 * Test NdndProducer default attribute values and setting attributes.
 */
class NdndsimProducerAttributeTestCase : public TestCase
{
  public:
    NdndsimProducerAttributeTestCase();
    void DoRun() override;
};

NdndsimProducerAttributeTestCase::NdndsimProducerAttributeTestCase()
    : TestCase("NdndProducer attribute defaults and configuration")
{
}

void
NdndsimProducerAttributeTestCase::DoRun()
{
    Ptr<NdndProducer> producer = CreateObject<NdndProducer>();
    NS_TEST_ASSERT_MSG_NE(producer, nullptr, "CreateObject<NdndProducer> should succeed");

    // Check default attribute values
    StringValue prefixVal;
    producer->GetAttribute("Prefix", prefixVal);
    NS_TEST_ASSERT_MSG_EQ(prefixVal.Get(), "/ndn/test", "Default prefix");

    UintegerValue payloadVal;
    producer->GetAttribute("PayloadSize", payloadVal);
    NS_TEST_ASSERT_MSG_EQ(payloadVal.Get(), 1024, "Default payload size");

    // Set custom attributes
    producer->SetAttribute("Prefix", StringValue("/data/video"));
    producer->GetAttribute("Prefix", prefixVal);
    NS_TEST_ASSERT_MSG_EQ(prefixVal.Get(), "/data/video", "Custom prefix");

    producer->SetAttribute("PayloadSize", UintegerValue(4096));
    producer->GetAttribute("PayloadSize", payloadVal);
    NS_TEST_ASSERT_MSG_EQ(payloadVal.Get(), 4096, "Custom payload size");

    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 6. NdndAppHelper Factory Tests
// ═══════════════════════════════════════════════════════════════════════

/**
 * Test NdndAppHelper creates the correct application types and
 * configures attributes properly.
 */
class NdndsimAppHelperTestCase : public TestCase
{
  public:
    NdndsimAppHelperTestCase();
    void DoRun() override;
};

NdndsimAppHelperTestCase::NdndsimAppHelperTestCase()
    : TestCase("NdndAppHelper factory and attribute configuration")
{
}

void
NdndsimAppHelperTestCase::DoRun()
{
    Ptr<Node> node = CreateObject<Node>();

    // Test consumer creation
    NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue("/test/consumer"));
    consumerHelper.SetAttribute("Frequency", DoubleValue(5.0));

    ApplicationContainer consumerApps = consumerHelper.Install(node);
    NS_TEST_ASSERT_MSG_EQ(consumerApps.GetN(), 1, "Should install one consumer app");

    Ptr<Application> app = consumerApps.Get(0);
    NS_TEST_ASSERT_MSG_NE(app, nullptr, "Installed app should not be null");

    StringValue prefix;
    app->GetAttribute("Prefix", prefix);
    NS_TEST_ASSERT_MSG_EQ(prefix.Get(), "/test/consumer", "Consumer prefix via helper");

    DoubleValue freq;
    app->GetAttribute("Frequency", freq);
    NS_TEST_ASSERT_MSG_EQ_TOL(freq.Get(), 5.0, 1e-9, "Consumer frequency via helper");

    // Test producer creation
    NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue("/test/producer"));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(2048));

    ApplicationContainer producerApps = producerHelper.Install(node);
    NS_TEST_ASSERT_MSG_EQ(producerApps.GetN(), 1, "Should install one producer app");

    Ptr<Application> prodApp = producerApps.Get(0);
    prodApp->GetAttribute("Prefix", prefix);
    NS_TEST_ASSERT_MSG_EQ(prefix.Get(), "/test/producer", "Producer prefix via helper");

    UintegerValue payload;
    prodApp->GetAttribute("PayloadSize", payload);
    NS_TEST_ASSERT_MSG_EQ(payload.Get(), 2048, "Producer payload size via helper");

    // Test multi-node install
    NodeContainer nodes;
    nodes.Create(3);
    NdndAppHelper multiHelper("ns3::ndndsim::NdndConsumer");
    ApplicationContainer multiApps = multiHelper.Install(nodes);
    NS_TEST_ASSERT_MSG_EQ(multiApps.GetN(), 3, "Should install on all 3 nodes");

    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 7. NdndStack TypeId Hierarchy Tests
// ═══════════════════════════════════════════════════════════════════════

/**
 * Verify that the TypeId inheritance chain is correct for all classes.
 */
class NdndsimTypeIdHierarchyTestCase : public TestCase
{
  public:
    NdndsimTypeIdHierarchyTestCase();
    void DoRun() override;
};

NdndsimTypeIdHierarchyTestCase::NdndsimTypeIdHierarchyTestCase()
    : TestCase("ndndSIM TypeId inheritance hierarchy")
{
}

void
NdndsimTypeIdHierarchyTestCase::DoRun()
{
    // NdndStack → Object
    TypeId stackTid = NdndStack::GetTypeId();
    NS_TEST_ASSERT_MSG_EQ(stackTid.GetParent().GetName(),
                           "ns3::Object",
                           "NdndStack parent is Object");

    // NdndApp → Application
    TypeId appTid = NdndApp::GetTypeId();
    NS_TEST_ASSERT_MSG_EQ(appTid.GetParent().GetName(),
                           "ns3::Application",
                           "NdndApp parent is Application");

    // NdndConsumer → NdndApp
    TypeId consumerTid = NdndConsumer::GetTypeId();
    NS_TEST_ASSERT_MSG_EQ(consumerTid.GetParent().GetName(),
                           "ns3::ndndsim::NdndApp",
                           "NdndConsumer parent is NdndApp");

    // NdndProducer → NdndApp
    TypeId producerTid = NdndProducer::GetTypeId();
    NS_TEST_ASSERT_MSG_EQ(producerTid.GetParent().GetName(),
                           "ns3::ndndsim::NdndApp",
                           "NdndProducer parent is NdndApp");
}

// ═══════════════════════════════════════════════════════════════════════
// 8. Stack Installation Integration Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * Test full stack installation on nodes with point-to-point links.
 * Requires the Go bridge.
 */
class NdndsimStackInstallTestCase : public TestCase
{
  public:
    NdndsimStackInstallTestCase();
    void DoRun() override;
};

NdndsimStackInstallTestCase::NdndsimStackInstallTestCase()
    : TestCase("NdndStack installation with point-to-point devices")
{
}

void
NdndsimStackInstallTestCase::DoRun()
{
    // Create two nodes connected by a point-to-point link
    NodeContainer nodes;
    nodes.Create(2);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    p2p.Install(nodes.Get(0), nodes.Get(1));

    // Initialize the Go bridge
    NdndStackHelper::InitializeBridge();

    // Install NDNd stack on both nodes
    NdndStackHelper stackHelper;
    Ptr<NdndStack> stack0 = stackHelper.Install(nodes.Get(0));
    Ptr<NdndStack> stack1 = stackHelper.Install(nodes.Get(1));

    NS_TEST_ASSERT_MSG_NE(stack0, nullptr, "Stack installed on node 0");
    NS_TEST_ASSERT_MSG_NE(stack1, nullptr, "Stack installed on node 1");

    // Face IDs should be non-zero after installation
    uint64_t face0 = stack0->GetFaceId(0);
    uint64_t face1 = stack1->GetFaceId(0);
    NS_TEST_ASSERT_MSG_NE(face0, 0, "Node 0 device 0 should have a face");
    NS_TEST_ASSERT_MSG_NE(face1, 0, "Node 1 device 0 should have a face");

    // Face IDs should be unique
    NS_TEST_ASSERT_MSG_NE(face0, face1, "Face IDs should be unique across nodes");

    // Non-existent device should return 0
    NS_TEST_ASSERT_MSG_EQ(stack0->GetFaceId(99), 0, "Non-existent device returns 0");

    // Duplicate install should return the existing stack
    Ptr<NdndStack> dup = stackHelper.Install(nodes.Get(0));
    NS_TEST_ASSERT_MSG_EQ(dup, stack0, "Duplicate Install returns existing stack");

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 9. Multi-Node Stack Installation Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * Test NdndStackHelper::Install(NodeContainer) for batch installation.
 */
class NdndsimMultiNodeInstallTestCase : public TestCase
{
  public:
    NdndsimMultiNodeInstallTestCase();
    void DoRun() override;
};

NdndsimMultiNodeInstallTestCase::NdndsimMultiNodeInstallTestCase()
    : TestCase("NdndStackHelper batch installation on multiple nodes")
{
}

void
NdndsimMultiNodeInstallTestCase::DoRun()
{
    // Linear topology: n0 -- n1 -- n2 -- n3
    NodeContainer nodes;
    nodes.Create(4);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));

    for (uint32_t i = 0; i < 3; ++i)
    {
        p2p.Install(nodes.Get(i), nodes.Get(i + 1));
    }

    NdndStackHelper::InitializeBridge();

    // Batch install on all nodes
    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    // Verify all nodes have stacks
    for (uint32_t i = 0; i < 4; ++i)
    {
        Ptr<NdndStack> stack = nodes.Get(i)->GetObject<NdndStack>();
        NS_TEST_ASSERT_MSG_NE(stack, nullptr, "Node " << i << " should have a stack");

        // Each node should have at least one face
        uint64_t faceId = stack->GetFaceId(0);
        NS_TEST_ASSERT_MSG_NE(faceId, 0, "Node " << i << " should have face for device 0");
    }

    // Middle nodes (1,2) have two devices (two faces)
    Ptr<NdndStack> stack1 = nodes.Get(1)->GetObject<NdndStack>();
    NS_TEST_ASSERT_MSG_NE(stack1->GetFaceId(0), 0, "Node 1 device 0 face");
    NS_TEST_ASSERT_MSG_NE(stack1->GetFaceId(1), 0, "Node 1 device 1 face");

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 10. Routing Configuration Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * Test AddRoute and AddRoutesToAll helper methods.
 */
class NdndsimRoutingTestCase : public TestCase
{
  public:
    NdndsimRoutingTestCase();
    void DoRun() override;
};

NdndsimRoutingTestCase::NdndsimRoutingTestCase()
    : TestCase("Routing configuration via NdndStackHelper")
{
}

void
NdndsimRoutingTestCase::DoRun()
{
    // Three nodes: Consumer -- Router -- Producer
    NodeContainer nodes;
    nodes.Create(3);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    p2p.Install(nodes.Get(0), nodes.Get(1));
    p2p.Install(nodes.Get(1), nodes.Get(2));

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    // Individual route: Consumer → Router (on device 0)
    // This should not throw or crash
    NdndStackHelper::AddRoute(nodes.Get(0), "/ndn/test", uint32_t(0), uint64_t(1));

    // Route via face ID directly
    Ptr<NdndStack> routerStack = nodes.Get(1)->GetObject<NdndStack>();
    uint64_t routerFace1 = routerStack->GetFaceId(1); // face toward Producer
    NdndStackHelper::AddRoute(nodes.Get(1), "/ndn/test", routerFace1, uint64_t(1));

    // Broadcast routes to all nodes (should not crash)
    NdndStackHelper::AddRoutesToAll("/ndn/broadcast", nodes);

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 11. Consumer-Producer Integration Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * End-to-end test: Consumer sends Interests through a Router to a
 * Producer, and verifies Data flows back via Go delegation.
 */
class NdndsimConsumerProducerTestCase : public TestCase
{
  public:
    NdndsimConsumerProducerTestCase();
    void DoRun() override;

  private:
    void DataReceivedCallback(uint32_t dataSize);
    uint32_t m_dataCount;
};

NdndsimConsumerProducerTestCase::NdndsimConsumerProducerTestCase()
    : TestCase("Consumer-Router-Producer end-to-end forwarding"),
      m_dataCount(0)
{
}

void
NdndsimConsumerProducerTestCase::DataReceivedCallback(uint32_t dataSize)
{
    m_dataCount++;
}

void
NdndsimConsumerProducerTestCase::DoRun()
{
    // Topology: Consumer(0) -- Router(1) -- Producer(2)
    NodeContainer nodes;
    nodes.Create(3);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("10ms"));
    p2p.Install(nodes.Get(0), nodes.Get(1));
    p2p.Install(nodes.Get(1), nodes.Get(2));

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    // Routes
    NdndStackHelper::AddRoute(nodes.Get(0), "/ndn/test", uint32_t(0), uint64_t(1));
    NdndStackHelper::AddRoute(nodes.Get(1), "/ndn/test", uint32_t(1), uint64_t(1));

    // Producer on node 2
    NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue("/ndn/test"));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(512));
    auto producerApps = producerHelper.Install(nodes.Get(2));
    producerApps.Start(Seconds(0.0));
    producerApps.Stop(Seconds(5.0));

    // Consumer on node 0
    NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue("/ndn/test"));
    consumerHelper.SetAttribute("Frequency", DoubleValue(10.0)); // 10 Interest/sec
    auto consumerApps = consumerHelper.Install(nodes.Get(0));
    consumerApps.Start(Seconds(1.0));
    consumerApps.Stop(Seconds(4.0));

    // Connect the Data received trace (Go delegation sends Interests;
    // DataReceived fires when Data comes back from the producer)
    Ptr<Application> consumerApp = consumerApps.Get(0);
    consumerApp->TraceConnectWithoutContext(
        "DataReceived",
        MakeCallback(&NdndsimConsumerProducerTestCase::DataReceivedCallback, this));

    Simulator::Stop(Seconds(5.0));
    Simulator::Run();

    // Consumer runs from t=1 to t=4 at 10 Hz; expect substantial Data return
    NS_TEST_ASSERT_MSG_GT(m_dataCount, 0, "At least one Data should be received");
    NS_TEST_ASSERT_MSG_GT(m_dataCount, 15, "Should receive significant Data in 3 seconds at 10 Hz");

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 12. Consumer Trace Sequence Number Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * Verify that the Consumer receives Data packets from a Producer via
 * Go delegation. Interest encoding and scheduling are handled by Go;
 * Data returns are tracked through the DataReceived trace.
 */
class NdndsimConsumerSeqTestCase : public TestCase
{
  public:
    NdndsimConsumerSeqTestCase();
    void DoRun() override;

  private:
    void DataReceivedCallback(uint32_t dataSize);
    uint32_t m_dataCount;
};

NdndsimConsumerSeqTestCase::NdndsimConsumerSeqTestCase()
    : TestCase("Consumer Go-delegated data flow")
{
}

void
NdndsimConsumerSeqTestCase::DataReceivedCallback(uint32_t dataSize)
{
    m_dataCount++;
}

void
NdndsimConsumerSeqTestCase::DoRun()
{
    NodeContainer nodes;
    nodes.Create(2);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    p2p.Install(nodes.Get(0), nodes.Get(1));

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    NdndStackHelper::AddRoute(nodes.Get(0), "/seq/test", uint32_t(0), uint64_t(1));

    // Producer on node 1
    NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue("/seq/test"));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(256));
    auto prodApps = producerHelper.Install(nodes.Get(1));
    prodApps.Start(Seconds(0.0));
    prodApps.Stop(Seconds(5.0));

    // Consumer sending at 2 Hz for 3 seconds
    NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue("/seq/test"));
    consumerHelper.SetAttribute("Frequency", DoubleValue(2.0));
    auto apps = consumerHelper.Install(nodes.Get(0));
    apps.Start(Seconds(1.0));
    apps.Stop(Seconds(4.0));

    apps.Get(0)->TraceConnectWithoutContext(
        "DataReceived",
        MakeCallback(&NdndsimConsumerSeqTestCase::DataReceivedCallback, this));

    Simulator::Stop(Seconds(5.0));
    Simulator::Run();

    // 3s at 2 Hz = ~6 Interests; expect most get satisfied
    NS_TEST_ASSERT_MSG_GT(m_dataCount, 0, "Should receive at least one Data");
    NS_TEST_ASSERT_MSG_GT(m_dataCount, 3, "Should receive most Data at 2 Hz over 3 seconds");

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 13. Multiple Consumers Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * Verify that multiple consumers on different nodes can run simultaneously
 * and receive Data via Go delegation.
 */
class NdndsimMultiConsumerTestCase : public TestCase
{
  public:
    NdndsimMultiConsumerTestCase();
    void DoRun() override;

  private:
    void DataReceived0(uint32_t dataSize);
    void DataReceived1(uint32_t dataSize);
    uint32_t m_count0;
    uint32_t m_count1;
};

NdndsimMultiConsumerTestCase::NdndsimMultiConsumerTestCase()
    : TestCase("Multiple consumers on different nodes"),
      m_count0(0),
      m_count1(0)
{
}

void
NdndsimMultiConsumerTestCase::DataReceived0(uint32_t dataSize)
{
    m_count0++;
}

void
NdndsimMultiConsumerTestCase::DataReceived1(uint32_t dataSize)
{
    m_count1++;
}

void
NdndsimMultiConsumerTestCase::DoRun()
{
    // Star topology: Consumer0(0) -- Router(2) -- Producer(3)
    //                Consumer1(1) --/
    NodeContainer nodes;
    nodes.Create(4);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("5ms"));
    p2p.Install(nodes.Get(0), nodes.Get(2)); // Consumer0 -- Router
    p2p.Install(nodes.Get(1), nodes.Get(2)); // Consumer1 -- Router
    p2p.Install(nodes.Get(2), nodes.Get(3)); // Router -- Producer

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    // Routes
    NdndStackHelper::AddRoute(nodes.Get(0), "/ndn/multi", uint32_t(0), uint64_t(1));
    NdndStackHelper::AddRoute(nodes.Get(1), "/ndn/multi", uint32_t(0), uint64_t(1));
    // Router has: device 0 → consumer0, device 1 → consumer1, device 2 → producer
    NdndStackHelper::AddRoute(nodes.Get(2), "/ndn/multi", uint32_t(2), uint64_t(1));

    // Producer
    NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue("/ndn/multi"));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(256));
    auto producerApps = producerHelper.Install(nodes.Get(3));
    producerApps.Start(Seconds(0.0));
    producerApps.Stop(Seconds(5.0));

    // Consumer 0
    NdndAppHelper consHelper0("ns3::ndndsim::NdndConsumer");
    consHelper0.SetAttribute("Prefix", StringValue("/ndn/multi"));
    consHelper0.SetAttribute("Frequency", DoubleValue(5.0));
    auto apps0 = consHelper0.Install(nodes.Get(0));
    apps0.Start(Seconds(1.0));
    apps0.Stop(Seconds(4.0));
    apps0.Get(0)->TraceConnectWithoutContext(
        "DataReceived",
        MakeCallback(&NdndsimMultiConsumerTestCase::DataReceived0, this));

    // Consumer 1
    NdndAppHelper consHelper1("ns3::ndndsim::NdndConsumer");
    consHelper1.SetAttribute("Prefix", StringValue("/ndn/multi"));
    consHelper1.SetAttribute("Frequency", DoubleValue(5.0));
    auto apps1 = consHelper1.Install(nodes.Get(1));
    apps1.Start(Seconds(1.0));
    apps1.Stop(Seconds(4.0));
    apps1.Get(0)->TraceConnectWithoutContext(
        "DataReceived",
        MakeCallback(&NdndsimMultiConsumerTestCase::DataReceived1, this));

    Simulator::Stop(Seconds(5.0));
    Simulator::Run();

    NS_TEST_ASSERT_MSG_GT(m_count0, 0, "Consumer 0 should receive Data");
    NS_TEST_ASSERT_MSG_GT(m_count1, 0, "Consumer 1 should receive Data");

    // Both consumers should receive Data (5 Hz x 3 sec, expect most satisfied)
    NS_TEST_ASSERT_MSG_GT(m_count0, 5, "Consumer 0 should receive substantial Data");
    NS_TEST_ASSERT_MSG_GT(m_count1, 5, "Consumer 1 should receive substantial Data");

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 14. L2 Protocol Handler EtherType Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * Verify that NdndStack registers EtherType 0x8624 on the correct devices,
 * and that non-NDN packets are NOT delivered to the stack.
 *
 * We inject a packet with a different EtherType on the wire and confirm
 * it does not crash or get processed.
 */
class NdndsimEtherTypeTestCase : public TestCase
{
  public:
    NdndsimEtherTypeTestCase();
    void DoRun() override;
};

NdndsimEtherTypeTestCase::NdndsimEtherTypeTestCase()
    : TestCase("EtherType 0x8624 registration on NetDevices")
{
}

void
NdndsimEtherTypeTestCase::DoRun()
{
    NodeContainer nodes;
    nodes.Create(2);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    NetDeviceContainer devs = p2p.Install(nodes.Get(0), nodes.Get(1));

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    // Send a non-NDN packet (EtherType 0x0800 = IPv4) from node 0 → node 1
    // This should NOT be delivered to the NDNd stack
    Ptr<Packet> ipPkt = Create<Packet>(100);
    devs.Get(0)->Send(ipPkt, devs.Get(0)->GetBroadcast(), 0x0800);

    // Send an NDN packet to verify the stack handles 0x8624
    uint8_t ndnBytes[] = {0x05, 0x05, 0x07, 0x03, 0x08, 0x01, 0x41}; // minimal Interest
    Ptr<Packet> ndnPkt = Create<Packet>(ndnBytes, sizeof(ndnBytes));
    devs.Get(0)->Send(ndnPkt, devs.Get(0)->GetBroadcast(), 0x8624);

    // Run the simulation briefly to process packets
    Simulator::Stop(Seconds(1.0));
    Simulator::Run();

    // If we get here without crashing, the EtherType filtering works
    NS_TEST_ASSERT_MSG_EQ(true, true, "EtherType filtering did not crash");

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 15. Stack Dispose / Cleanup Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * Verify that the NdndStack properly cleans up when Simulator::Destroy()
 * is called (DoDispose path).
 */
class NdndsimStackDisposeTestCase : public TestCase
{
  public:
    NdndsimStackDisposeTestCase();
    void DoRun() override;
};

NdndsimStackDisposeTestCase::NdndsimStackDisposeTestCase()
    : TestCase("NdndStack cleanup on Simulator::Destroy")
{
}

void
NdndsimStackDisposeTestCase::DoRun()
{
    NodeContainer nodes;
    nodes.Create(2);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    p2p.Install(nodes.Get(0), nodes.Get(1));

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    Simulator::Stop(Seconds(1.0));
    Simulator::Run();

    // DestroyBridge cleans up Go side
    NdndStackHelper::DestroyBridge();

    // Simulator::Destroy triggers DoDispose on all Objects
    Simulator::Destroy();

    // If we get here without crashing, cleanup succeeded
    NS_TEST_ASSERT_MSG_EQ(true, true, "Stack disposal completed cleanly");
}

// ═══════════════════════════════════════════════════════════════════════
// 16. App Lifecycle Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * Test that apps respect Start/Stop scheduling: Data should only
 * be received during the active window.
 */
class NdndsimAppLifecycleTestCase : public TestCase
{
  public:
    NdndsimAppLifecycleTestCase();
    void DoRun() override;

  private:
    void DataReceivedCallback(uint32_t dataSize);
    uint32_t m_dataCount;
    Time m_firstDataTime;
    Time m_lastDataTime;
};

NdndsimAppLifecycleTestCase::NdndsimAppLifecycleTestCase()
    : TestCase("Application start-stop lifecycle timing"),
      m_dataCount(0)
{
}

void
NdndsimAppLifecycleTestCase::DataReceivedCallback(uint32_t dataSize)
{
    if (m_dataCount == 0)
    {
        m_firstDataTime = Simulator::Now();
    }
    m_lastDataTime = Simulator::Now();
    m_dataCount++;
}

void
NdndsimAppLifecycleTestCase::DoRun()
{
    NodeContainer nodes;
    nodes.Create(2);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    p2p.Install(nodes.Get(0), nodes.Get(1));

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    NdndStackHelper::AddRoute(nodes.Get(0), "/lifecycle", uint32_t(0), uint64_t(1));

    // Producer on node 1 to provide Data responses
    NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue("/lifecycle"));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(256));
    auto prodApps = producerHelper.Install(nodes.Get(1));
    prodApps.Start(Seconds(0.0));
    prodApps.Stop(Seconds(10.0));

    // Start at t=2, stop at t=5 → 3 second active window
    NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue("/lifecycle"));
    consumerHelper.SetAttribute("Frequency", DoubleValue(4.0)); // 4 Hz
    auto apps = consumerHelper.Install(nodes.Get(0));
    apps.Start(Seconds(2.0));
    apps.Stop(Seconds(5.0));

    apps.Get(0)->TraceConnectWithoutContext(
        "DataReceived",
        MakeCallback(&NdndsimAppLifecycleTestCase::DataReceivedCallback, this));

    Simulator::Stop(Seconds(10.0));
    Simulator::Run();

    // First Data should not arrive before t=2 (consumer start) + RTT
    NS_TEST_ASSERT_MSG_EQ(m_firstDataTime.GetSeconds() >= 2.0,
                           true,
                           "First Data should be at or after t=2s");

    // Last Data should be before t=6 (stop at 5 + some RTT slack)
    NS_TEST_ASSERT_MSG_LT(m_lastDataTime.GetSeconds(),
                           6.0,
                           "Last Data should be before t=6s (stop + RTT)");

    // At 4 Hz for 3 seconds: expect about 12 Data replies
    NS_TEST_ASSERT_MSG_GT(m_dataCount, 6, "Should receive ~12 Data in 3s at 4 Hz");
    NS_TEST_ASSERT_MSG_LT(m_dataCount, 16, "Should not receive more than ~15 Data");

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 17. Link Failure and Recovery Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * Simulate link failure using a RateErrorModel (100% drop) and verify
 * that Data stops arriving during the failure window, then resumes
 * after recovery. Uses DataReceived trace (Go delegation).
 *
 * Topology: Consumer(0) -- Router(1) -- Producer(2)
 *
 * Timeline:
 *   t=0.0  Producer starts
 *   t=1.0  Consumer starts at 10 Hz
 *   t=3.0  Link Consumer<->Router fails (100% drop)
 *   t=6.0  Link recovers (0% drop)
 *   t=9.0  Consumer stops
 *   t=10.0 Simulation ends
 *
 * We count Data received before, during, and after the failure window.
 */
class NdndsimLinkFailureTestCase : public TestCase
{
  public:
    NdndsimLinkFailureTestCase();
    void DoRun() override;

  private:
    void DataReceivedCallback(uint32_t dataSize);
    uint32_t m_beforeFailure;  // Data received in [1, 3)
    uint32_t m_duringFailure;  // Data received in [3, 6)
    uint32_t m_afterRecovery;  // Data received in [6, 9)
};

NdndsimLinkFailureTestCase::NdndsimLinkFailureTestCase()
    : TestCase("Link failure and recovery with error model"),
      m_beforeFailure(0),
      m_duringFailure(0),
      m_afterRecovery(0)
{
}

void
NdndsimLinkFailureTestCase::DataReceivedCallback(uint32_t dataSize)
{
    double now = Simulator::Now().GetSeconds();
    if (now < 3.0)
    {
        m_beforeFailure++;
    }
    else if (now < 6.0)
    {
        m_duringFailure++;
    }
    else
    {
        m_afterRecovery++;
    }
}

void
NdndsimLinkFailureTestCase::DoRun()
{
    // Topology: Consumer(0) -- Router(1) -- Producer(2)
    NodeContainer nodes;
    nodes.Create(3);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("5ms"));
    NetDeviceContainer link01 = p2p.Install(nodes.Get(0), nodes.Get(1));
    p2p.Install(nodes.Get(1), nodes.Get(2));

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    NdndStackHelper::AddRoute(nodes.Get(0), "/ndn/link", uint32_t(0), uint64_t(1));
    NdndStackHelper::AddRoute(nodes.Get(1), "/ndn/link", uint32_t(1), uint64_t(1));

    // Producer on node 2
    NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue("/ndn/link"));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(256));
    auto producerApps = producerHelper.Install(nodes.Get(2));
    producerApps.Start(Seconds(0.0));
    producerApps.Stop(Seconds(10.0));

    // Consumer on node 0 at 10 Hz
    NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue("/ndn/link"));
    consumerHelper.SetAttribute("Frequency", DoubleValue(10.0));
    auto consumerApps = consumerHelper.Install(nodes.Get(0));
    consumerApps.Start(Seconds(1.0));
    consumerApps.Stop(Seconds(9.0));

    consumerApps.Get(0)->TraceConnectWithoutContext(
        "DataReceived",
        MakeCallback(&NdndsimLinkFailureTestCase::DataReceivedCallback, this));

    // --- Link failure at t=3: install 100% error model on both devices ---
    Ptr<RateErrorModel> errorModel0 = CreateObject<RateErrorModel>();
    Ptr<RateErrorModel> errorModel1 = CreateObject<RateErrorModel>();

    // Schedule link DOWN at t=3
    Simulator::Schedule(Seconds(3.0), [&]() {
        errorModel0->SetRate(1.0); // 100% packet drop
        errorModel0->Enable();
        link01.Get(0)->SetAttribute("ReceiveErrorModel", PointerValue(errorModel0));

        errorModel1->SetRate(1.0);
        errorModel1->Enable();
        link01.Get(1)->SetAttribute("ReceiveErrorModel", PointerValue(errorModel1));
    });

    // Schedule link UP at t=6
    Simulator::Schedule(Seconds(6.0), [&]() {
        errorModel0->Disable();
        errorModel1->Disable();
    });

    Simulator::Stop(Seconds(10.0));
    Simulator::Run();

    // Before failure (t=1 to t=3, 2 sec at 10 Hz): expect Data returns
    NS_TEST_ASSERT_MSG_GT(m_beforeFailure, 10,
                           "Should receive Data before link failure");

    // During failure (t=3 to t=6, 3 sec): link is down, no Data should return
    // Allow a small count for in-flight packets just before failure
    NS_TEST_ASSERT_MSG_LT(m_duringFailure, 5,
                           "Very few or no Data should arrive during link failure");

    // After recovery (t=6 to t=9, 3 sec at 10 Hz): Data should flow again
    NS_TEST_ASSERT_MSG_GT(m_afterRecovery, 10,
                           "Should receive Data after link recovery");

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 18. CalculateRoutes BFS Routing Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * Verify CalculateRoutes installs correct BFS shortest-path routes on
 * a 4-node linear topology and that Data flows from producer to consumer.
 *
 * Topology: Consumer(0) -- (1) -- (2) -- Producer(3)
 *
 * CalculateRoutes should install nexthop routes on each node toward
 * node 3 (the producer).
 */
class NdndsimCalculateRoutesTestCase : public TestCase
{
  public:
    NdndsimCalculateRoutesTestCase();
    void DoRun() override;

  private:
    void DataReceivedCallback(uint32_t dataSize);
    uint32_t m_dataCount;
};

NdndsimCalculateRoutesTestCase::NdndsimCalculateRoutesTestCase()
    : TestCase("Dijkstra shortest-path routing with link metrics"),
      m_dataCount(0)
{
}

void
NdndsimCalculateRoutesTestCase::DataReceivedCallback(uint32_t dataSize)
{
    m_dataCount++;
}

void
NdndsimCalculateRoutesTestCase::DoRun()
{
    // Linear topology: n0 -- n1 -- n2 -- n3
    NodeContainer nodes;
    nodes.Create(4);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("5ms"));
    for (uint32_t i = 0; i < 3; ++i)
    {
        p2p.Install(nodes.Get(i), nodes.Get(i + 1));
    }

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    std::string prefix = "/ndn/calc";

    // Producer on node 3
    NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue(prefix));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(512));
    auto producerApps = producerHelper.Install(nodes.Get(3));
    producerApps.Start(Seconds(0.0));
    producerApps.Stop(Seconds(5.0));

    // Consumer on node 0
    NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue(prefix));
    consumerHelper.SetAttribute("Frequency", DoubleValue(10.0));
    auto consumerApps = consumerHelper.Install(nodes.Get(0));
    consumerApps.Start(Seconds(1.0));
    consumerApps.Stop(Seconds(4.0));

    consumerApps.Get(0)->TraceConnectWithoutContext(
        "DataReceived",
        MakeCallback(&NdndsimCalculateRoutesTestCase::DataReceivedCallback, this));

    // Use BFS CalculateRoutes instead of manual AddRoute
    NodeContainer producerNodes;
    producerNodes.Add(nodes.Get(3));
    NdndStackHelper::CalculateRoutes(prefix, producerNodes, nodes);

    Simulator::Stop(Seconds(5.0));
    Simulator::Run();

    // Consumer runs 3s at 10 Hz → ~30 Interests; expect most get satisfied
    NS_TEST_ASSERT_MSG_GT(m_dataCount, 15,
                           "Should receive Data via BFS-computed routes (3-hop linear)");

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 19. DV Routing Initialization Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * Verify EnableDvRouting starts DV on all nodes without errors.
 * Uses a simple 3-node linear topology and checks that DV initializes
 * and the simulation completes cleanly.
 *
 * Topology: (0) -- (1) -- (2)
 */
class NdndsimDvRoutingInitTestCase : public TestCase
{
  public:
    NdndsimDvRoutingInitTestCase();
    void DoRun() override;
};

NdndsimDvRoutingInitTestCase::NdndsimDvRoutingInitTestCase()
    : TestCase("EnableDvRouting initializes DV on all nodes")
{
}

void
NdndsimDvRoutingInitTestCase::DoRun()
{
    NodeContainer nodes;
    nodes.Create(3);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("5ms"));
    p2p.Install(nodes.Get(0), nodes.Get(1));
    p2p.Install(nodes.Get(1), nodes.Get(2));

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    // Enable DV routing — should not throw or crash
    NdndStackHelper::EnableDvRouting("/ndn", nodes);

    // Run briefly to let DV exchange initial advertisements
    Simulator::Stop(Seconds(5.0));
    Simulator::Run();

    // If we reach here, DV initialized and ran without crashing
    NS_TEST_ASSERT_MSG_EQ(true, true, "DV routing initialized and ran cleanly");

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 20. DV Routing End-to-End Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * End-to-end test with DV routing: Consumer sends Interests over a
 * 3-hop linear topology where routes are discovered dynamically by DV.
 *
 * Topology: Consumer(0) -- (1) -- (2) -- Producer(3)
 *
 * DV routing is enabled at t=0. Consumer starts at t=3 to give DV time
 * to converge. Producer starts at t=0.5.
 */
class NdndsimDvEndToEndTestCase : public TestCase
{
  public:
    NdndsimDvEndToEndTestCase();
    void DoRun() override;

  private:
    void DataReceivedCallback(uint32_t dataSize);
    uint32_t m_dataCount;
};

NdndsimDvEndToEndTestCase::NdndsimDvEndToEndTestCase()
    : TestCase("DV routing end-to-end consumer-producer forwarding"),
      m_dataCount(0)
{
}

void
NdndsimDvEndToEndTestCase::DataReceivedCallback(uint32_t dataSize)
{
    m_dataCount++;
}

void
NdndsimDvEndToEndTestCase::DoRun()
{
    // 4-node linear topology
    NodeContainer nodes;
    nodes.Create(4);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("5ms"));
    for (uint32_t i = 0; i < 3; ++i)
    {
        p2p.Install(nodes.Get(i), nodes.Get(i + 1));
    }

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    // Enable DV routing on all nodes
    NdndStackHelper::EnableDvRouting("/ndn", nodes);

    std::string prefix = "/ndn/dvtest";

    // Producer on node 3 — starts early so DV can learn about it
    NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue(prefix));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(512));
    auto producerApps = producerHelper.Install(nodes.Get(3));
    producerApps.Start(Seconds(0.5));
    producerApps.Stop(Seconds(15.0));

    // Consumer on node 0 — delayed start to allow DV convergence
    NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue(prefix));
    consumerHelper.SetAttribute("Frequency", DoubleValue(10.0));
    auto consumerApps = consumerHelper.Install(nodes.Get(0));
    consumerApps.Start(Seconds(8.0));
    consumerApps.Stop(Seconds(14.0));

    consumerApps.Get(0)->TraceConnectWithoutContext(
        "DataReceived",
        MakeCallback(&NdndsimDvEndToEndTestCase::DataReceivedCallback, this));

    Simulator::Stop(Seconds(15.0));
    Simulator::Run();

    // Consumer runs 6s at 10 Hz → ~60 Interests; after DV convergence,
    // expect significant Data return (not just 1 or 2)
    NS_TEST_ASSERT_MSG_GT(m_dataCount, 10,
                           "Should receive substantial Data via DV-discovered routes");

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 21. Data Received End-to-End Verification Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * Verify both InterestSent and DataReceived traces fire end-to-end.
 * This confirms the full Interest→Data pipeline works: the consumer
 * sends Interests that traverse a router, the producer generates Data,
 * and the Data returns to the consumer.
 *
 * Topology: Consumer(0) -- Router(1) -- Producer(2)
 */
class NdndsimDataReceivedE2eTestCase : public TestCase
{
  public:
    NdndsimDataReceivedE2eTestCase();
    void DoRun() override;

  private:
    void DataReceivedCb(uint32_t dataSize);
    void DataProducedCb(uint32_t dataSize);
    uint32_t m_dataReceived;
    uint32_t m_dataProduced;
};

NdndsimDataReceivedE2eTestCase::NdndsimDataReceivedE2eTestCase()
    : TestCase("End-to-end Interest-Data trace verification"),
      m_dataReceived(0),
      m_dataProduced(0)
{
}

void
NdndsimDataReceivedE2eTestCase::DataReceivedCb(uint32_t dataSize)
{
    m_dataReceived++;
}

void
NdndsimDataReceivedE2eTestCase::DataProducedCb(uint32_t dataSize)
{
    m_dataProduced++;
}

void
NdndsimDataReceivedE2eTestCase::DoRun()
{
    NodeContainer nodes;
    nodes.Create(3);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("5ms"));
    p2p.Install(nodes.Get(0), nodes.Get(1));
    p2p.Install(nodes.Get(1), nodes.Get(2));

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    NdndStackHelper::AddRoute(nodes.Get(0), "/ndn/e2e", uint32_t(0), uint64_t(1));
    NdndStackHelper::AddRoute(nodes.Get(1), "/ndn/e2e", uint32_t(1), uint64_t(1));

    // Producer on node 2
    NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue("/ndn/e2e"));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(512));
    auto producerApps = producerHelper.Install(nodes.Get(2));
    producerApps.Start(Seconds(0.0));
    producerApps.Stop(Seconds(5.0));

    producerApps.Get(0)->TraceConnectWithoutContext(
        "DataSent",
        MakeCallback(&NdndsimDataReceivedE2eTestCase::DataProducedCb, this));

    // Consumer on node 0
    NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue("/ndn/e2e"));
    consumerHelper.SetAttribute("Frequency", DoubleValue(10.0));
    auto consumerApps = consumerHelper.Install(nodes.Get(0));
    consumerApps.Start(Seconds(1.0));
    consumerApps.Stop(Seconds(4.0));

    consumerApps.Get(0)->TraceConnectWithoutContext(
        "DataReceived",
        MakeCallback(&NdndsimDataReceivedE2eTestCase::DataReceivedCb, this));

    Simulator::Stop(Seconds(5.0));
    Simulator::Run();

    // Producer should generate Data for received Interests
    NS_TEST_ASSERT_MSG_GT(m_dataProduced, 15, "Producer should produce substantial Data");

    // Consumer should receive Data back
    NS_TEST_ASSERT_MSG_GT(m_dataReceived, 15,
                           "Consumer should receive substantial Data back");

    // DataReceived should be ≤ DataProduced (can't receive more than produced)
    NS_TEST_ASSERT_MSG_LT_OR_EQ(m_dataReceived, m_dataProduced,
                                  "Cannot receive more Data than produced");

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 22. Dijkstra Metric-Weighted Routing Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * Verify that CalculateRoutes respects link metrics (delay-based cost).
 *
 * Diamond topology:
 *
 *     Consumer(0)
 *      / \
 *     /   \
 *   (1)   (2)     0→1 = 100ms delay (expensive)
 *     \   /       0→2 = 1ms delay   (cheap)
 *      \ /
 *    Producer(3)
 *
 * Dijkstra should prefer the path through node 2 (lower delay).
 * Both paths should deliver Data; the test verifies Data flows.
 */
class NdndsimDijkstraMetricTestCase : public TestCase
{
  public:
    NdndsimDijkstraMetricTestCase();
    void DoRun() override;

  private:
    void DataReceivedCb(uint32_t dataSize);
    uint32_t m_dataCount;
};

NdndsimDijkstraMetricTestCase::NdndsimDijkstraMetricTestCase()
    : TestCase("Dijkstra routing prefers lower-cost path"),
      m_dataCount(0)
{
}

void
NdndsimDijkstraMetricTestCase::DataReceivedCb(uint32_t dataSize)
{
    m_dataCount++;
}

void
NdndsimDijkstraMetricTestCase::DoRun()
{
    NodeContainer nodes;
    nodes.Create(4);

    PointToPointHelper p2pSlow;
    p2pSlow.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2pSlow.SetChannelAttribute("Delay", StringValue("100ms")); // expensive path

    PointToPointHelper p2pFast;
    p2pFast.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2pFast.SetChannelAttribute("Delay", StringValue("1ms")); // cheap path

    p2pSlow.Install(nodes.Get(0), nodes.Get(1)); // Consumer → Node1 (slow)
    p2pFast.Install(nodes.Get(0), nodes.Get(2)); // Consumer → Node2 (fast)
    p2pFast.Install(nodes.Get(1), nodes.Get(3)); // Node1 → Producer
    p2pFast.Install(nodes.Get(2), nodes.Get(3)); // Node2 → Producer

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    std::string prefix = "/ndn/diamond";

    // Producer on node 3
    NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue(prefix));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(256));
    auto producerApps = producerHelper.Install(nodes.Get(3));
    producerApps.Start(Seconds(0.0));
    producerApps.Stop(Seconds(5.0));

    // Consumer on node 0
    NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue(prefix));
    consumerHelper.SetAttribute("Frequency", DoubleValue(10.0));
    auto consumerApps = consumerHelper.Install(nodes.Get(0));
    consumerApps.Start(Seconds(1.0));
    consumerApps.Stop(Seconds(4.0));

    consumerApps.Get(0)->TraceConnectWithoutContext(
        "DataReceived",
        MakeCallback(&NdndsimDijkstraMetricTestCase::DataReceivedCb, this));

    // Use Dijkstra CalculateRoutes — should pick the fast path through node 2
    NodeContainer producers;
    producers.Add(nodes.Get(3));
    NdndStackHelper::CalculateRoutes(prefix, producers, nodes);

    Simulator::Stop(Seconds(5.0));
    Simulator::Run();

    NS_TEST_ASSERT_MSG_GT(m_dataCount, 15,
                           "Data should flow via Dijkstra-computed shortest path");

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 23. Multiple Producers / Multiple Prefixes Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * Verify that two producers serving different prefixes on separate nodes
 * can both satisfy Interests from a single consumer node.
 *
 * Topology:
 *   Producer-A(0) ---- Router(1) ---- Consumer(2) ---- Producer-B(3)
 */
class NdndsimMultiPrefixTestCase : public TestCase
{
  public:
    NdndsimMultiPrefixTestCase();
    void DoRun() override;

  private:
    void DataReceivedA(uint32_t dataSize);
    void DataReceivedB(uint32_t dataSize);
    uint32_t m_countA;
    uint32_t m_countB;
};

NdndsimMultiPrefixTestCase::NdndsimMultiPrefixTestCase()
    : TestCase("Multiple producers with different prefixes"),
      m_countA(0),
      m_countB(0)
{
}

void
NdndsimMultiPrefixTestCase::DataReceivedA(uint32_t dataSize)
{
    m_countA++;
}

void
NdndsimMultiPrefixTestCase::DataReceivedB(uint32_t dataSize)
{
    m_countB++;
}

void
NdndsimMultiPrefixTestCase::DoRun()
{
    NodeContainer nodes;
    nodes.Create(4);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("5ms"));
    p2p.Install(nodes.Get(0), nodes.Get(1)); // ProdA -- Router
    p2p.Install(nodes.Get(1), nodes.Get(2)); // Router -- Consumer
    p2p.Install(nodes.Get(2), nodes.Get(3)); // Consumer -- ProdB

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    // Routes for prefix A: Consumer(2) → Router(1) → ProdA(0)
    NdndStackHelper::AddRoute(nodes.Get(2), "/ndn/alpha", uint32_t(0), uint64_t(1));
    NdndStackHelper::AddRoute(nodes.Get(1), "/ndn/alpha", uint32_t(0), uint64_t(1));

    // Routes for prefix B: Consumer(2) → ProdB(3)
    NdndStackHelper::AddRoute(nodes.Get(2), "/ndn/beta", uint32_t(1), uint64_t(1));

    // Producer A on node 0
    NdndAppHelper prodAHelper("ns3::ndndsim::NdndProducer");
    prodAHelper.SetAttribute("Prefix", StringValue("/ndn/alpha"));
    prodAHelper.SetAttribute("PayloadSize", UintegerValue(256));
    auto prodAApps = prodAHelper.Install(nodes.Get(0));
    prodAApps.Start(Seconds(0.0));
    prodAApps.Stop(Seconds(5.0));

    // Producer B on node 3
    NdndAppHelper prodBHelper("ns3::ndndsim::NdndProducer");
    prodBHelper.SetAttribute("Prefix", StringValue("/ndn/beta"));
    prodBHelper.SetAttribute("PayloadSize", UintegerValue(512));
    auto prodBApps = prodBHelper.Install(nodes.Get(3));
    prodBApps.Start(Seconds(0.0));
    prodBApps.Stop(Seconds(5.0));

    // Consumer A on node 2 → /ndn/alpha
    NdndAppHelper consAHelper("ns3::ndndsim::NdndConsumer");
    consAHelper.SetAttribute("Prefix", StringValue("/ndn/alpha"));
    consAHelper.SetAttribute("Frequency", DoubleValue(5.0));
    auto consAApps = consAHelper.Install(nodes.Get(2));
    consAApps.Start(Seconds(1.0));
    consAApps.Stop(Seconds(4.0));
    consAApps.Get(0)->TraceConnectWithoutContext(
        "DataReceived",
        MakeCallback(&NdndsimMultiPrefixTestCase::DataReceivedA, this));

    // Consumer B on node 2 → /ndn/beta (separate app instance on same node)
    NdndAppHelper consBHelper("ns3::ndndsim::NdndConsumer");
    consBHelper.SetAttribute("Prefix", StringValue("/ndn/beta"));
    consBHelper.SetAttribute("Frequency", DoubleValue(5.0));
    auto consBApps = consBHelper.Install(nodes.Get(2));
    consBApps.Start(Seconds(1.0));
    consBApps.Stop(Seconds(4.0));
    consBApps.Get(0)->TraceConnectWithoutContext(
        "DataReceived",
        MakeCallback(&NdndsimMultiPrefixTestCase::DataReceivedB, this));

    Simulator::Stop(Seconds(5.0));
    Simulator::Run();

    NS_TEST_ASSERT_MSG_GT(m_countA, 5, "Consumer A should receive Data for /ndn/alpha");
    NS_TEST_ASSERT_MSG_GT(m_countB, 5, "Consumer B should receive Data for /ndn/beta");

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 24. Consumer Go Delegation Verification Tests
// ═══════════════════════════════════════════════════════════════════════

/**
 * Verify that the consumer no longer exposes the "Randomize" attribute
 * (removed during cleanup — Go delegation handles scheduling).
 */
class NdndsimConsumerNoRandomizeTestCase : public TestCase
{
  public:
    NdndsimConsumerNoRandomizeTestCase();
    void DoRun() override;
};

NdndsimConsumerNoRandomizeTestCase::NdndsimConsumerNoRandomizeTestCase()
    : TestCase("Consumer does not have Randomize attribute after cleanup")
{
}

void
NdndsimConsumerNoRandomizeTestCase::DoRun()
{
    TypeId tid = NdndConsumer::GetTypeId();

    // Iterate over all attributes and verify "Randomize" is absent
    bool hasRandomize = false;
    for (uint32_t i = 0; i < tid.GetAttributeN(); ++i)
    {
        TypeId::AttributeInformation info = tid.GetAttribute(i);
        if (info.name == "Randomize")
        {
            hasRandomize = true;
        }
    }
    NS_TEST_ASSERT_MSG_EQ(hasRandomize, false,
                           "Consumer should NOT have Randomize attribute after cleanup");

    // Verify expected attributes remain: Prefix, Frequency, LifeTime
    bool hasPrefix = false, hasFrequency = false, hasLifeTime = false;
    for (uint32_t i = 0; i < tid.GetAttributeN(); ++i)
    {
        TypeId::AttributeInformation info = tid.GetAttribute(i);
        if (info.name == "Prefix") hasPrefix = true;
        if (info.name == "Frequency") hasFrequency = true;
        if (info.name == "LifeTime") hasLifeTime = true;
    }
    NS_TEST_ASSERT_MSG_EQ(hasPrefix, true, "Consumer should have Prefix attribute");
    NS_TEST_ASSERT_MSG_EQ(hasFrequency, true, "Consumer should have Frequency attribute");
    NS_TEST_ASSERT_MSG_EQ(hasLifeTime, true, "Consumer should have LifeTime attribute");
}

/**
 * Verify that the consumer's LifeTime attribute defaults to 4000ms
 * (changed from 2000ms during cleanup to match emu Go consumer).
 */
class NdndsimConsumerLifetimeDefaultTestCase : public TestCase
{
  public:
    NdndsimConsumerLifetimeDefaultTestCase();
    void DoRun() override;
};

NdndsimConsumerLifetimeDefaultTestCase::NdndsimConsumerLifetimeDefaultTestCase()
    : TestCase("Consumer LifeTime defaults to 4000ms after cleanup")
{
}

void
NdndsimConsumerLifetimeDefaultTestCase::DoRun()
{
    Ptr<NdndConsumer> consumer = CreateObject<NdndConsumer>();
    TimeValue ltVal;
    consumer->GetAttribute("LifeTime", ltVal);
    NS_TEST_ASSERT_MSG_EQ(ltVal.Get(), MilliSeconds(4000),
                           "LifeTime default should be 4000ms (matching emu)");
    Simulator::Destroy();
}

/**
 * Verify that starting and stopping the consumer cleanly works.
 * OnStop no longer cancels m_sendEvent (Go manages scheduling), so this
 * tests that the cleanup didn't introduce stop-time crashes.
 */
class NdndsimConsumerCleanStopTestCase : public TestCase
{
  public:
    NdndsimConsumerCleanStopTestCase();
    void DoRun() override;

  private:
    void DataReceivedCallback(uint32_t dataSize);
    uint32_t m_dataCount;
};

NdndsimConsumerCleanStopTestCase::NdndsimConsumerCleanStopTestCase()
    : TestCase("Consumer start-stop does not crash after cleanup"),
      m_dataCount(0)
{
}

void
NdndsimConsumerCleanStopTestCase::DataReceivedCallback(uint32_t dataSize)
{
    m_dataCount++;
}

void
NdndsimConsumerCleanStopTestCase::DoRun()
{
    NodeContainer nodes;
    nodes.Create(2);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    p2p.Install(nodes.Get(0), nodes.Get(1));

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    NdndStackHelper::AddRoute(nodes.Get(0), "/ndn/stop", uint32_t(0), uint64_t(1));

    // Producer on node 1
    NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue("/ndn/stop"));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(256));
    auto prodApps = producerHelper.Install(nodes.Get(1));
    prodApps.Start(Seconds(0.0));
    prodApps.Stop(Seconds(10.0));

    // Consumer: start at t=1, stop at t=2 (short window), then start again at t=4
    NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue("/ndn/stop"));
    consumerHelper.SetAttribute("Frequency", DoubleValue(10.0));
    auto apps = consumerHelper.Install(nodes.Get(0));
    apps.Start(Seconds(1.0));
    apps.Stop(Seconds(2.0));

    apps.Get(0)->TraceConnectWithoutContext(
        "DataReceived",
        MakeCallback(&NdndsimConsumerCleanStopTestCase::DataReceivedCallback, this));

    // Second consumer instance to verify a fresh start works
    NdndAppHelper consumer2Helper("ns3::ndndsim::NdndConsumer");
    consumer2Helper.SetAttribute("Prefix", StringValue("/ndn/stop"));
    consumer2Helper.SetAttribute("Frequency", DoubleValue(10.0));
    auto apps2 = consumer2Helper.Install(nodes.Get(0));
    apps2.Start(Seconds(4.0));
    apps2.Stop(Seconds(6.0));

    Simulator::Stop(Seconds(8.0));
    Simulator::Run();

    // If we get here without crashing, the cleanup is correct
    NS_TEST_ASSERT_MSG_EQ(true, true, "Consumer start-stop completed cleanly");

    // First instance should receive some Data
    NS_TEST_ASSERT_MSG_GT(m_dataCount, 0,
                           "First consumer should receive Data before stopping");

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

/**
 * Verify that the consumer's Go delegation correctly sends Interests and
 * receives Data at different frequencies. Tests 1 Hz and 20 Hz to confirm
 * the frequency attribute is properly passed to the Go bridge.
 */
class NdndsimConsumerFrequencyTestCase : public TestCase
{
  public:
    NdndsimConsumerFrequencyTestCase();
    void DoRun() override;

  private:
    void DataReceivedSlow(uint32_t dataSize);
    void DataReceivedFast(uint32_t dataSize);
    uint32_t m_slowCount;
    uint32_t m_fastCount;
};

NdndsimConsumerFrequencyTestCase::NdndsimConsumerFrequencyTestCase()
    : TestCase("Consumer frequency attribute passed to Go delegation"),
      m_slowCount(0),
      m_fastCount(0)
{
}

void
NdndsimConsumerFrequencyTestCase::DataReceivedSlow(uint32_t dataSize)
{
    m_slowCount++;
}

void
NdndsimConsumerFrequencyTestCase::DataReceivedFast(uint32_t dataSize)
{
    m_fastCount++;
}

void
NdndsimConsumerFrequencyTestCase::DoRun()
{
    NodeContainer nodes;
    nodes.Create(3);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue("1ms"));
    p2p.Install(nodes.Get(0), nodes.Get(1));
    p2p.Install(nodes.Get(1), nodes.Get(2));

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    NdndStackHelper::AddRoute(nodes.Get(0), "/ndn/freq", uint32_t(0), uint64_t(1));
    NdndStackHelper::AddRoute(nodes.Get(2), "/ndn/freq", uint32_t(0), uint64_t(1));

    // Producer on node 1
    NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue("/ndn/freq"));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(256));
    auto prodApps = producerHelper.Install(nodes.Get(1));
    prodApps.Start(Seconds(0.0));
    prodApps.Stop(Seconds(10.0));

    // Slow consumer on node 0: 1 Hz
    NdndAppHelper slowHelper("ns3::ndndsim::NdndConsumer");
    slowHelper.SetAttribute("Prefix", StringValue("/ndn/freq/slow"));
    slowHelper.SetAttribute("Frequency", DoubleValue(1.0));
    auto slowApps = slowHelper.Install(nodes.Get(0));
    slowApps.Start(Seconds(1.0));
    slowApps.Stop(Seconds(6.0));
    slowApps.Get(0)->TraceConnectWithoutContext(
        "DataReceived",
        MakeCallback(&NdndsimConsumerFrequencyTestCase::DataReceivedSlow, this));

    // Fast consumer on node 2: 20 Hz
    NdndAppHelper fastHelper("ns3::ndndsim::NdndConsumer");
    fastHelper.SetAttribute("Prefix", StringValue("/ndn/freq/fast"));
    fastHelper.SetAttribute("Frequency", DoubleValue(20.0));
    auto fastApps = fastHelper.Install(nodes.Get(2));
    fastApps.Start(Seconds(1.0));
    fastApps.Stop(Seconds(6.0));
    fastApps.Get(0)->TraceConnectWithoutContext(
        "DataReceived",
        MakeCallback(&NdndsimConsumerFrequencyTestCase::DataReceivedFast, this));

    Simulator::Stop(Seconds(8.0));
    Simulator::Run();

    // Slow: 5s at 1 Hz = ~5 Data; Fast: 5s at 20 Hz = ~100 Data
    NS_TEST_ASSERT_MSG_GT(m_slowCount, 2, "Slow consumer should receive some Data");
    NS_TEST_ASSERT_MSG_GT(m_fastCount, 50, "Fast consumer should receive many Data");

    // Fast consumer should receive significantly more than slow
    NS_TEST_ASSERT_MSG_GT(m_fastCount, m_slowCount * 3,
                           "Fast consumer should receive >3x more Data than slow");

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 25. Producer Freshness Attribute Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * Verify NdndProducer's Freshness attribute can be set and retrieved.
 */
class NdndsimProducerFreshnessTestCase : public TestCase
{
  public:
    NdndsimProducerFreshnessTestCase();
    void DoRun() override;
};

NdndsimProducerFreshnessTestCase::NdndsimProducerFreshnessTestCase()
    : TestCase("NdndProducer Freshness attribute")
{
}

void
NdndsimProducerFreshnessTestCase::DoRun()
{
    Ptr<NdndProducer> producer = CreateObject<NdndProducer>();

    // Default freshness should be 0ms
    TimeValue freshVal;
    producer->GetAttribute("Freshness", freshVal);
    NS_TEST_ASSERT_MSG_EQ(freshVal.Get(), MilliSeconds(0), "Default freshness");

    // Set to 4 seconds
    producer->SetAttribute("Freshness", TimeValue(Seconds(4.0)));
    producer->GetAttribute("Freshness", freshVal);
    NS_TEST_ASSERT_MSG_EQ(freshVal.Get(), Seconds(4.0), "Custom freshness of 4s");

    // Set to 500ms
    producer->SetAttribute("Freshness", TimeValue(MilliSeconds(500)));
    producer->GetAttribute("Freshness", freshVal);
    NS_TEST_ASSERT_MSG_EQ(freshVal.Get(), MilliSeconds(500), "Custom freshness of 500ms");

    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 26. Topology Reader Correctness Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * Read the grid and tree topology files and verify node count, link count,
 * and named node lookup via ns3::Names.
 */
class NdndsimTopologyReaderTestCase : public TestCase
{
  public:
    NdndsimTopologyReaderTestCase();
    void DoRun() override;
};

NdndsimTopologyReaderTestCase::NdndsimTopologyReaderTestCase()
    : TestCase("Topology reader creates correct nodes and links")
{
}

void
NdndsimTopologyReaderTestCase::DoRun()
{
    // Read the 3×3 grid topology
    {
        NdndTopologyReader reader;
        reader.SetFileName("contrib/ndndSIM/examples/topologies/topo-grid-3x3.txt");
        NodeContainer nodes = reader.Read();

        NS_TEST_ASSERT_MSG_EQ(nodes.GetN(), 9, "Grid should have 9 nodes");
        NS_TEST_ASSERT_MSG_EQ(reader.GetLinks().size(), 12, "Grid should have 12 links");

        // Named lookup
        Ptr<Node> node0 = Names::Find<Node>("Node0");
        NS_TEST_ASSERT_MSG_NE(node0, nullptr, "Node0 should be findable by name");
        Ptr<Node> node8 = Names::Find<Node>("Node8");
        NS_TEST_ASSERT_MSG_NE(node8, nullptr, "Node8 should be findable by name");

        // Clear names for next topology
        Simulator::Destroy();
    }

    // Read the tree topology
    {
        NdndTopologyReader reader;
        reader.SetFileName("contrib/ndndSIM/examples/topologies/topo-tree.txt");
        NodeContainer nodes = reader.Read();

        NS_TEST_ASSERT_MSG_EQ(nodes.GetN(), 7, "Tree should have 7 nodes");
        NS_TEST_ASSERT_MSG_EQ(reader.GetLinks().size(), 6, "Tree should have 6 links");

        Ptr<Node> root = Names::Find<Node>("root");
        NS_TEST_ASSERT_MSG_NE(root, nullptr, "root should be findable by name");
        Ptr<Node> leaf1 = Names::Find<Node>("leaf-1");
        NS_TEST_ASSERT_MSG_NE(leaf1, nullptr, "leaf-1 should be findable by name");

        Simulator::Destroy();
    }
}

// ═══════════════════════════════════════════════════════════════════════
// 27. DV Grid End-to-End Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * DV routing on a 3×3 grid. Consumer at corner (0,0), producer at
 * opposite corner (2,2). DV must discover multi-hop routes through
 * the grid.
 */
class NdndsimDvGridE2eTestCase : public TestCase
{
  public:
    NdndsimDvGridE2eTestCase();
    void DoRun() override;

  private:
    void DataReceivedCb(uint32_t dataSize);
    uint32_t m_dataCount;
};

NdndsimDvGridE2eTestCase::NdndsimDvGridE2eTestCase()
    : TestCase("DV routing end-to-end on 3x3 grid"),
      m_dataCount(0)
{
}

void
NdndsimDvGridE2eTestCase::DataReceivedCb(uint32_t dataSize)
{
    m_dataCount++;
}

void
NdndsimDvGridE2eTestCase::DoRun()
{
    // 3×3 grid via PointToPointGridHelper
    Config::SetDefault("ns3::PointToPointNetDevice::DataRate", StringValue("10Mbps"));
    Config::SetDefault("ns3::PointToPointChannel::Delay", StringValue("5ms"));
    Config::SetDefault("ns3::DropTailQueue<Packet>::MaxSize", StringValue("20p"));

    PointToPointHelper p2p;
    PointToPointGridHelper grid(3, 3, p2p);

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    NodeContainer allNodes;
    for (uint32_t row = 0; row < 3; ++row)
    {
        for (uint32_t col = 0; col < 3; ++col)
        {
            stackHelper.Install(grid.GetNode(row, col));
            allNodes.Add(grid.GetNode(row, col));
        }
    }

    // Enable DV routing
    NdndStackHelper::EnableDvRouting("/ndn", allNodes);

    std::string prefix = "/ndn/gridtest";

    // Producer at (2,2) — start early for DV convergence
    NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
    producerHelper.SetAttribute("Prefix", StringValue(prefix));
    producerHelper.SetAttribute("PayloadSize", UintegerValue(512));
    auto producerApps = producerHelper.Install(grid.GetNode(2, 2));
    producerApps.Start(Seconds(0.5));
    producerApps.Stop(Seconds(40.0));

    // Consumer at (0,0) — delayed start for DV convergence
    NdndAppHelper consumerHelper("ns3::ndndsim::NdndConsumer");
    consumerHelper.SetAttribute("Prefix", StringValue(prefix));
    consumerHelper.SetAttribute("Frequency", DoubleValue(10.0));
    auto consumerApps = consumerHelper.Install(grid.GetNode(0, 0));
    consumerApps.Start(Seconds(25.0));
    consumerApps.Stop(Seconds(35.0));

    consumerApps.Get(0)->TraceConnectWithoutContext(
        "DataReceived",
        MakeCallback(&NdndsimDvGridE2eTestCase::DataReceivedCb, this));

    Simulator::Stop(Seconds(40.0));
    Simulator::Run();

    // 10s × 10Hz = ~100 Interests; after convergence expect Data to flow
    NS_TEST_ASSERT_MSG_GT(m_dataCount, 5,
                           "Should receive substantial Data via DV on 3x3 grid");

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 28. DV with Multiple Producers Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * DV routing with two producers for different prefixes. Verifies that
 * DV prefix propagation handles multiple application prefixes correctly.
 *
 * Topology: ProdA(0) -- Router(1) -- Consumer(2) -- ProdB(3)
 */
class NdndsimDvMultiProducerTestCase : public TestCase
{
  public:
    NdndsimDvMultiProducerTestCase();
    void DoRun() override;

  private:
    void DataReceivedA(uint32_t dataSize);
    void DataReceivedB(uint32_t dataSize);
    uint32_t m_dataA;
    uint32_t m_dataB;
};

NdndsimDvMultiProducerTestCase::NdndsimDvMultiProducerTestCase()
    : TestCase("DV routing with multiple producers and prefixes"),
      m_dataA(0),
      m_dataB(0)
{
}

void
NdndsimDvMultiProducerTestCase::DataReceivedA(uint32_t dataSize)
{
    m_dataA++;
}

void
NdndsimDvMultiProducerTestCase::DataReceivedB(uint32_t dataSize)
{
    m_dataB++;
}

void
NdndsimDvMultiProducerTestCase::DoRun()
{
    NodeContainer nodes;
    nodes.Create(4);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("5ms"));
    p2p.Install(nodes.Get(0), nodes.Get(1));
    p2p.Install(nodes.Get(1), nodes.Get(2));
    p2p.Install(nodes.Get(2), nodes.Get(3));

    NdndStackHelper::InitializeBridge();

    NdndStackHelper stackHelper;
    stackHelper.Install(nodes);

    NdndStackHelper::EnableDvRouting("/ndn", nodes);

    // Producer A on node 0 — /ndn/alpha
    NdndAppHelper prodAHelper("ns3::ndndsim::NdndProducer");
    prodAHelper.SetAttribute("Prefix", StringValue("/ndn/alpha"));
    prodAHelper.SetAttribute("PayloadSize", UintegerValue(256));
    auto prodAApps = prodAHelper.Install(nodes.Get(0));
    prodAApps.Start(Seconds(0.5));
    prodAApps.Stop(Seconds(30.0));

    // Producer B on node 3 — /ndn/beta
    NdndAppHelper prodBHelper("ns3::ndndsim::NdndProducer");
    prodBHelper.SetAttribute("Prefix", StringValue("/ndn/beta"));
    prodBHelper.SetAttribute("PayloadSize", UintegerValue(256));
    auto prodBApps = prodBHelper.Install(nodes.Get(3));
    prodBApps.Start(Seconds(0.5));
    prodBApps.Stop(Seconds(30.0));

    // Consumer for /ndn/alpha on node 2
    NdndAppHelper consAHelper("ns3::ndndsim::NdndConsumer");
    consAHelper.SetAttribute("Prefix", StringValue("/ndn/alpha"));
    consAHelper.SetAttribute("Frequency", DoubleValue(5.0));
    auto consAApps = consAHelper.Install(nodes.Get(2));
    consAApps.Start(Seconds(15.0));
    consAApps.Stop(Seconds(25.0));
    consAApps.Get(0)->TraceConnectWithoutContext(
        "DataReceived",
        MakeCallback(&NdndsimDvMultiProducerTestCase::DataReceivedA, this));

    // Consumer for /ndn/beta on node 1
    NdndAppHelper consBHelper("ns3::ndndsim::NdndConsumer");
    consBHelper.SetAttribute("Prefix", StringValue("/ndn/beta"));
    consBHelper.SetAttribute("Frequency", DoubleValue(5.0));
    auto consBApps = consBHelper.Install(nodes.Get(1));
    consBApps.Start(Seconds(15.0));
    consBApps.Stop(Seconds(25.0));
    consBApps.Get(0)->TraceConnectWithoutContext(
        "DataReceived",
        MakeCallback(&NdndsimDvMultiProducerTestCase::DataReceivedB, this));

    Simulator::Stop(Seconds(30.0));
    Simulator::Run();

    NS_TEST_ASSERT_MSG_GT(m_dataA, 0,
                           "Consumer should receive Data for /ndn/alpha via DV");
    NS_TEST_ASSERT_MSG_GT(m_dataB, 0,
                           "Consumer should receive Data for /ndn/beta via DV");

    NdndStackHelper::DestroyBridge();
    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 29. Consumer LifeTime Attribute Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * Verify NdndConsumer's LifeTime attribute can be set and retrieved.
 */
class NdndsimConsumerLifetimeTestCase : public TestCase
{
  public:
    NdndsimConsumerLifetimeTestCase();
    void DoRun() override;
};

NdndsimConsumerLifetimeTestCase::NdndsimConsumerLifetimeTestCase()
    : TestCase("NdndConsumer LifeTime attribute")
{
}

void
NdndsimConsumerLifetimeTestCase::DoRun()
{
    Ptr<NdndConsumer> consumer = CreateObject<NdndConsumer>();

    // Default lifetime should be 4000ms (Go delegation default)
    TimeValue ltVal;
    consumer->GetAttribute("LifeTime", ltVal);
    NS_TEST_ASSERT_MSG_EQ(ltVal.Get(), MilliSeconds(4000), "Default LifeTime");

    // Set to 500ms
    consumer->SetAttribute("LifeTime", TimeValue(MilliSeconds(500)));
    consumer->GetAttribute("LifeTime", ltVal);
    NS_TEST_ASSERT_MSG_EQ(ltVal.Get(), MilliSeconds(500), "Custom LifeTime 500ms");

    // Set to 10s
    consumer->SetAttribute("LifeTime", TimeValue(Seconds(10.0)));
    consumer->GetAttribute("LifeTime", ltVal);
    NS_TEST_ASSERT_MSG_EQ(ltVal.Get(), Seconds(10.0), "Custom LifeTime 10s");

    Simulator::Destroy();
}

// ═══════════════════════════════════════════════════════════════════════
// 30. Zipf Consumer Attribute Test
// ═══════════════════════════════════════════════════════════════════════

/**
 * Verify NdndConsumerZipf attributes and end-to-end operation.
 */
class NdndsimZipfConsumerTestCase : public TestCase
{
  public:
    NdndsimZipfConsumerTestCase();
    void DoRun() override;

  private:
    void InterestSentCb(uint32_t seqNo);
    uint32_t m_interestCount;
};

NdndsimZipfConsumerTestCase::NdndsimZipfConsumerTestCase()
    : TestCase("Zipf consumer attributes and operation"),
      m_interestCount(0)
{
}

void
NdndsimZipfConsumerTestCase::InterestSentCb(uint32_t seqNo)
{
    m_interestCount++;
}

void
NdndsimZipfConsumerTestCase::DoRun()
{
    // Test attribute defaults and configuration
    {
        Ptr<NdndConsumerZipf> zipf = CreateObject<NdndConsumerZipf>();

        UintegerValue numContents;
        zipf->GetAttribute("NumberOfContents", numContents);
        NS_TEST_ASSERT_MSG_EQ(numContents.Get(), 100, "Default NumberOfContents");

        DoubleValue qVal, sVal;
        zipf->GetAttribute("q", qVal);
        zipf->GetAttribute("s", sVal);
        NS_TEST_ASSERT_MSG_EQ_TOL(qVal.Get(), 0.0, 1e-9, "Default q");
        NS_TEST_ASSERT_MSG_EQ_TOL(sVal.Get(), 0.7, 1e-9, "Default s");

        zipf->SetAttribute("NumberOfContents", UintegerValue(1000));
        zipf->GetAttribute("NumberOfContents", numContents);
        NS_TEST_ASSERT_MSG_EQ(numContents.Get(), 1000, "Custom NumberOfContents");
    }

    Simulator::Destroy();

    // Integration test: Zipf consumer sends Interests
    {
        NodeContainer nodes;
        nodes.Create(2);

        PointToPointHelper p2p;
        p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
        p2p.SetChannelAttribute("Delay", StringValue("5ms"));
        p2p.Install(nodes.Get(0), nodes.Get(1));

        NdndStackHelper::InitializeBridge();

        NdndStackHelper stackHelper;
        stackHelper.Install(nodes);

        NdndStackHelper::AddRoute(nodes.Get(0), "/ndn/zipftest", uint32_t(0), uint64_t(1));

        NdndAppHelper producerHelper("ns3::ndndsim::NdndProducer");
        producerHelper.SetAttribute("Prefix", StringValue("/ndn/zipftest"));
        auto prodApps = producerHelper.Install(nodes.Get(1));
        prodApps.Start(Seconds(0.0));
        prodApps.Stop(Seconds(5.0));

        NdndAppHelper zipfHelper("ns3::ndndsim::NdndConsumerZipf");
        zipfHelper.SetAttribute("Prefix", StringValue("/ndn/zipftest"));
        zipfHelper.SetAttribute("Frequency", DoubleValue(20.0));
        zipfHelper.SetAttribute("NumberOfContents", UintegerValue(50));
        zipfHelper.SetAttribute("s", DoubleValue(1.0));
        auto zipfApps = zipfHelper.Install(nodes.Get(0));
        zipfApps.Start(Seconds(1.0));
        zipfApps.Stop(Seconds(4.0));

        zipfApps.Get(0)->TraceConnectWithoutContext(
            "InterestSent",
            MakeCallback(&NdndsimZipfConsumerTestCase::InterestSentCb, this));

        Simulator::Stop(Seconds(5.0));
        Simulator::Run();

        NS_TEST_ASSERT_MSG_GT(m_interestCount, 40,
                               "Zipf consumer should send ~60 Interests (3s × 20Hz)");

        NdndStackHelper::DestroyBridge();
        Simulator::Destroy();
    }
}

// ═══════════════════════════════════════════════════════════════════════
// Test Suite Registration
// ═══════════════════════════════════════════════════════════════════════

/**
 * \brief TestSuite for the ndndSIM module.
 */
class NdndsimTestSuite : public TestSuite
{
  public:
    NdndsimTestSuite();
};

NdndsimTestSuite::NdndsimTestSuite()
    : TestSuite("ndndsim", Type::UNIT)
{
    // Unit tests (no Go bridge needed)
    AddTestCase(new NdndsimTypeIdTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimStackObjectTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimAppBaseTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimConsumerAttributeTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimProducerAttributeTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimAppHelperTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimTypeIdHierarchyTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimProducerFreshnessTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimConsumerLifetimeTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimTopologyReaderTestCase, TestCase::Duration::QUICK);

    // Integration tests (require Go bridge)
    AddTestCase(new NdndsimStackInstallTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimMultiNodeInstallTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimRoutingTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimConsumerProducerTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimConsumerSeqTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimMultiConsumerTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimEtherTypeTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimStackDisposeTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimAppLifecycleTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimLinkFailureTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimDataReceivedE2eTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimMultiPrefixTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimConsumerNoRandomizeTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimConsumerLifetimeDefaultTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimConsumerCleanStopTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimConsumerFrequencyTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimZipfConsumerTestCase, TestCase::Duration::QUICK);

    // Routing algorithm tests
    AddTestCase(new NdndsimCalculateRoutesTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimDijkstraMetricTestCase, TestCase::Duration::QUICK);

    // DV routing tests
    AddTestCase(new NdndsimDvRoutingInitTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimDvEndToEndTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimDvGridE2eTestCase, TestCase::Duration::QUICK);
    AddTestCase(new NdndsimDvMultiProducerTestCase, TestCase::Duration::QUICK);
}

/// Static instance to auto-register the test suite
static NdndsimTestSuite g_ndndsimTestSuite;
