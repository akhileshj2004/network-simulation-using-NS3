#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/mobility-module.h"
#include "ns3/internet-module.h"
#include "ns3/lte-module.h"
#include "ns3/applications-module.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/flow-monitor-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("IoT5GSim");

void PrintClientSending(uint32_t clientId) {
    std::ostringstream msg;
    msg << "Client " << clientId << " sending data...";
    NS_LOG_UNCOND(msg.str());
}

int main(int argc, char *argv[]) {
    Time::SetResolution(Time::NS);
    LogComponentEnable("UdpEchoClientApplication", LOG_LEVEL_INFO);
    LogComponentEnable("UdpEchoServerApplication", LOG_LEVEL_INFO);

    // Create 10 IoT Devices (UEs)
    NodeContainer ueNodes;
    ueNodes.Create(10);

    // LTE Base Station (eNodeB)
    NodeContainer enbNodes;
    enbNodes.Create(1);

    // Remote Server
    NodeContainer remoteHostContainer;
    remoteHostContainer.Create(1);
    Ptr<Node> remoteHost = remoteHostContainer.Get(0);

    // Install Internet stack on remote host and UEs
    InternetStackHelper internet;
    internet.Install(remoteHostContainer);
    internet.Install(ueNodes);

    // Create LTE and EPC Helper
    Ptr<PointToPointEpcHelper> epcHelper = CreateObject<PointToPointEpcHelper>();
    Ptr<LteHelper> lteHelper = CreateObject<LteHelper>();
    lteHelper->SetEpcHelper(epcHelper);

    // Connect PGW to remote server via point-to-point link
    Ptr<Node> pgw = epcHelper->GetPgwNode();
    PointToPointHelper p2ph;
    p2ph.SetDeviceAttribute("DataRate", DataRateValue(DataRate("256Kbps"))); // Bandwidth reduced to induce packet loss
    p2ph.SetChannelAttribute("Delay", TimeValue(Seconds(0.01)));
    NetDeviceContainer internetDevices = p2ph.Install(pgw, remoteHost);

    // Assign IP addresses to p2p link between PGW and remoteHost
    Ipv4AddressHelper ipv4h;
    ipv4h.SetBase("1.0.0.0", "255.0.0.0");
    Ipv4InterfaceContainer internetIfaces = ipv4h.Assign(internetDevices);
    Ipv4Address remoteHostAddr = internetIfaces.GetAddress(1);

    // Mobility model for eNodeB and UEs
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(enbNodes);
    mobility.Install(ueNodes);

    // Install LTE Devices on nodes
    NetDeviceContainer enbLteDevs = lteHelper->InstallEnbDevice(enbNodes);
    NetDeviceContainer ueLteDevs = lteHelper->InstallUeDevice(ueNodes);

    // Assign IP addresses to UEs
    Ipv4InterfaceContainer ueIpIfaces = epcHelper->AssignUeIpv4Address(ueLteDevs);

    // Attach UEs to eNodeB
    for (uint32_t i = 0; i < ueNodes.GetN(); ++i) {
        lteHelper->Attach(ueLteDevs.Get(i), enbLteDevs.Get(0));
    }

    // Setup static routing on remote host to route to UE subnet
    Ipv4StaticRoutingHelper ipv4RoutingHelper;
    Ptr<Ipv4StaticRouting> remoteHostStaticRouting =
        ipv4RoutingHelper.GetStaticRouting(remoteHost->GetObject<Ipv4>());
    remoteHostStaticRouting->AddNetworkRouteTo(Ipv4Address("7.0.0.0"), Ipv4Mask("255.0.0.0"), 1);

    // Install Echo Server on remote host
    uint16_t echoPort = 9;
    UdpEchoServerHelper echoServer(echoPort);
    ApplicationContainer serverApps = echoServer.Install(remoteHost);
    serverApps.Start(Seconds(1.0));
    serverApps.Stop(Seconds(20.0));

    // Install Echo Servers on each UE to enable reverse flows
    ApplicationContainer ueServerApps;
    for (uint32_t i = 0; i < ueNodes.GetN(); ++i) {
        UdpEchoServerHelper ueEchoServer(echoPort);
        ApplicationContainer app = ueEchoServer.Install(ueNodes.Get(i));
        app.Start(Seconds(1.0));
        app.Stop(Seconds(20.0));
        ueServerApps.Add(app);
    }

    // Echo Clients from UEs to remoteHost (forward flows)
    ApplicationContainer clientApps;
    for (uint32_t i = 0; i < ueNodes.GetN(); ++i) {
        Simulator::Schedule(Seconds(2.0 + i), &PrintClientSending, i + 1);

        UdpEchoClientHelper echoClient(remoteHostAddr, echoPort);
        echoClient.SetAttribute("MaxPackets", UintegerValue(10));
        echoClient.SetAttribute("Interval", TimeValue(Seconds(1.0)));
        echoClient.SetAttribute("PacketSize", UintegerValue(1024));

        ApplicationContainer apps = echoClient.Install(ueNodes.Get(i));
        apps.Start(Seconds(2.0 + i));
        apps.Stop(Seconds(20.0));
        clientApps.Add(apps);
    }

    // Echo Clients from remoteHost to each UE (reverse flows)
    ApplicationContainer remoteClients;
    for (uint32_t i = 0; i < ueNodes.GetN(); ++i) {
        UdpEchoClientHelper reverseClient(ueIpIfaces.GetAddress(i), echoPort);
        reverseClient.SetAttribute("MaxPackets", UintegerValue(10));
        reverseClient.SetAttribute("Interval", TimeValue(Seconds(1.0)));
        reverseClient.SetAttribute("PacketSize", UintegerValue(1024));

        ApplicationContainer apps = reverseClient.Install(remoteHost);
        apps.Start(Seconds(3.0 + i));
        apps.Stop(Seconds(20.0));
        remoteClients.Add(apps);
    }

    // Enable pcap tracing on PGW and remote host point-to-point link devices (optional)
    p2ph.EnablePcapAll("iot-5g-sim");

    // Install FlowMonitor on all nodes
    FlowMonitorHelper flowmonHelper;
    Ptr<FlowMonitor> flowmon = flowmonHelper.InstallAll();

    Simulator::Stop(Seconds(21.0));
    Simulator::Run();

    // Print Flow Monitor results
    Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier>(flowmonHelper.GetClassifier());
    std::map<FlowId, FlowMonitor::FlowStats> stats = flowmon->GetFlowStats();

    std::cout << "\nFlow Monitor Results:\n";
    for (auto const& flow : stats) {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(flow.first);

        std::cout << "Flow ID: " << flow.first
                  << " Src: " << t.sourceAddress
                  << " Dst: " << t.destinationAddress
                  << " Tx Packets: " << flow.second.txPackets
                  << " Rx Packets: " << flow.second.rxPackets
                  << " Lost Packets: " << flow.second.lostPackets
                  << " Delay Sum (s): " << flow.second.delaySum.GetSeconds()
                  << " Throughput (bps): " << flow.second.rxBytes * 8.0 /
                        (flow.second.timeLastRxPacket.GetSeconds() - flow.second.timeFirstTxPacket.GetSeconds())
                  << std::endl;
    }

    Simulator::Destroy();
    return 0;
}
