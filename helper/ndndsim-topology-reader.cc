/*
 * ndndSIM - ns-3 NDNd Simulation Module
 *
 * NdndTopologyReader implementation.
 */

#include "ndndsim-topology-reader.h"

#include "ns3/constant-position-mobility-model.h"
#include "ns3/error-model.h"
#include "ns3/log.h"
#include "ns3/names.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/pointer.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"

#include <fstream>
#include <set>
#include <sstream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("NdndTopologyReader");

namespace ndndsim
{

NdndTopologyReader::NdndTopologyReader()
{
}

NdndTopologyReader::~NdndTopologyReader()
{
}

void
NdndTopologyReader::SetFileName(const std::string& fileName)
{
    m_fileName = fileName;
}

NodeContainer
NdndTopologyReader::GetNodes() const
{
    return m_nodes;
}

const std::vector<NdndTopologyReader::LinkInfo>&
NdndTopologyReader::GetLinks() const
{
    return m_links;
}

NodeContainer
NdndTopologyReader::Read()
{
    NS_LOG_FUNCTION(this << m_fileName);

    std::ifstream topgen(m_fileName);
    NS_ABORT_MSG_IF(!topgen.is_open(), "Cannot open topology file: " << m_fileName);

    // ─── Parse "router" section ────────────────────────────────────

    // Seek to "router" keyword
    std::string line;
    while (std::getline(topgen, line))
    {
        // Trim whitespace
        auto pos = line.find_first_not_of(" \t\r\n");
        if (pos != std::string::npos)
        {
            line = line.substr(pos);
        }
        if (line == "router")
        {
            break;
        }
    }
    NS_ABORT_MSG_IF(topgen.eof(), "Topology file has no \"router\" section: " << m_fileName);

    // Read router entries until "link" keyword or EOF
    while (std::getline(topgen, line))
    {
        // Trim leading whitespace
        auto pos = line.find_first_not_of(" \t\r\n");
        if (pos == std::string::npos)
        {
            continue; // blank line
        }
        line = line.substr(pos);

        if (line[0] == '#')
        {
            continue; // comment
        }
        if (line == "link")
        {
            break; // start of link section
        }

        std::istringstream iss(line);
        std::string name;
        std::string city;
        double yPos = 0;
        double xPos = 0;

        iss >> name;
        if (name.empty())
        {
            continue;
        }
        iss >> city >> yPos >> xPos;
        // mpi-partition is parsed but ignored

        Ptr<Node> node = CreateObject<Node>();
        Names::Add(name, node);

        // Set position for visualization
        Ptr<ConstantPositionMobilityModel> mobility =
            CreateObject<ConstantPositionMobilityModel>();
        mobility->SetPosition(Vector(xPos, -yPos, 0));
        node->AggregateObject(mobility);

        m_nodes.Add(node);
        m_nodeMap[name] = node;

        NS_LOG_DEBUG("Node: " << name << " pos=(" << xPos << ", " << yPos << ")");
    }

    NS_LOG_INFO("Read " << m_nodes.GetN() << " nodes");

    // ─── Parse "link" section ──────────────────────────────────────

    std::map<std::string, std::set<std::string>> processed;

    while (std::getline(topgen, line))
    {
        // Trim leading whitespace
        auto pos = line.find_first_not_of(" \t\r\n");
        if (pos == std::string::npos)
        {
            continue;
        }
        line = line.substr(pos);

        if (line[0] == '#')
        {
            continue;
        }

        std::istringstream iss(line);
        std::string from;
        std::string to;
        std::string capacity;
        std::string metric;
        std::string delay;
        std::string maxPackets;
        std::string lossRate;

        iss >> from >> to >> capacity >> metric >> delay >> maxPackets >> lossRate;

        if (from.empty() || to.empty())
        {
            continue;
        }

        // Skip duplicate links
        if (processed[to].count(from) > 0)
        {
            NS_LOG_DEBUG("Skipping duplicate link: " << from << " <-> " << to);
            continue;
        }
        processed[from].insert(to);

        auto itFrom = m_nodeMap.find(from);
        auto itTo = m_nodeMap.find(to);
        NS_ABORT_MSG_IF(itFrom == m_nodeMap.end(), "Node not found: " << from);
        NS_ABORT_MSG_IF(itTo == m_nodeMap.end(), "Node not found: " << to);

        // Configure and install PointToPoint link
        PointToPointHelper p2p;

        if (!capacity.empty())
        {
            p2p.SetDeviceAttribute("DataRate", StringValue(capacity));
        }
        if (!delay.empty())
        {
            p2p.SetChannelAttribute("Delay", StringValue(delay));
        }
        if (!maxPackets.empty())
        {
            p2p.SetQueue("ns3::DropTailQueue<Packet>",
                         "MaxSize",
                         StringValue(maxPackets + "p"));
        }

        NetDeviceContainer nd = p2p.Install(itFrom->second, itTo->second);

        // Apply loss rate if specified
        if (!lossRate.empty())
        {
            Ptr<RateErrorModel> errorFrom = CreateObject<RateErrorModel>();
            errorFrom->SetRate(std::stod(lossRate));
            nd.Get(0)->SetAttribute("ReceiveErrorModel", PointerValue(errorFrom));

            Ptr<RateErrorModel> errorTo = CreateObject<RateErrorModel>();
            errorTo->SetRate(std::stod(lossRate));
            nd.Get(1)->SetAttribute("ReceiveErrorModel", PointerValue(errorTo));
        }

        LinkInfo info;
        info.fromNode = itFrom->second;
        info.toNode = itTo->second;
        info.fromName = from;
        info.toName = to;
        info.devices = nd;
        info.dataRate = capacity;
        info.delay = delay;
        info.metric = metric.empty() ? 1 : static_cast<uint16_t>(std::stoul(metric));
        info.maxPackets = maxPackets.empty() ? 0 : static_cast<uint32_t>(std::stoul(maxPackets));
        info.lossRate = lossRate;
        m_links.push_back(info);

        NS_LOG_DEBUG("Link: " << from << " <-> " << to << " " << capacity << " " << delay);
    }

    NS_LOG_INFO("Created " << m_links.size() << " links");
    topgen.close();

    return m_nodes;
}

} // namespace ndndsim
} // namespace ns3
