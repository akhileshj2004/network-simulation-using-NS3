#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/mobility-module.h"
#include "ns3/internet-module.h"
#include "ns3/lte-module.h"
#include "ns3/applications-module.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/traffic-control-module.h"

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

    NodeContainer ueNodes;
    ueNodes.Create(10);

    NodeContainer enbNodes;
    enbNodes.Create(1);

    NodeContainer remoteHostContainer;
    remoteHostContainer.Create(1);
    Ptr<Node> remoteHost = remoteHostContainer.Get(0);

    InternetStackHelper internet;
    internet.Install(remoteHostContainer);
    internet.Install(ueNodes);

    Ptr<PointToPointEpcHelper> epcHelper = CreateObject<PointToPointEpcHelper>();
    Ptr<LteHelper> lteHelper = CreateObject<LteHelper>();
    lteHelper->SetEpcHelper(epcHelper);

    Ptr<Node> pgw = epcHelper->GetPgwNode();
    PointToPointHelper p2ph;

    // 🎯 Induce packet loss with small bandwidth and queuing limit
    p2ph.SetDeviceAttribute("DataRate", DataRateValue(DataRate("256Kbps")));
    p2ph.SetChannelAttribute("Delay", TimeValue(MilliSeconds(10)));

    NetDeviceContainer internetDevices = p2ph.Install(pgw, remoteHost);

    // Configure a limited queue to allow packet drops
    TrafficControlHelper tch;
    tch.SetRootQueueDisc("ns3::PfifoFastQueueDisc", "MaxSize", StringValue("10p"));
    tch.Install(internetDevices);

    Ipv4AddressHelper ipv4h;
    ipv4h.SetBase("1.0.0.0", "255.0.0.0");
    Ipv4InterfaceContainer internetIfaces = ipv4h.Assign(internetDevices);
    Ipv4Address remoteHostAddr = internetIfaces.GetAddress(1);

    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(enbNodes);
    mobility.Install(ueNodes);

    NetDeviceContainer enbLteDevs = lteHelper->InstallEnbDevice(enbNodes);
    NetDeviceContainer ueLteDevs = lteHelper->InstallUeDevice(ueNodes);

    Ipv4InterfaceContainer ueIpIfaces = epcHelper->AssignUeIpv4Address(ueLteDevs);

    for (uint32_t i = 0; i < ueNodes.GetN(); ++i) {
        lteHelper->Attach(ueLteDevs.Get(i), enbLteDevs.Get(0));
    }

    Ipv4StaticRoutingHelper ipv4RoutingHelper;
    Ptr<Ipv4StaticRouting> remoteHostStaticRouting =
        ipv4RoutingHelper.GetStaticRouting(remoteHost->GetObject<Ipv4>());
    remoteHostStaticRouting->AddNetworkRouteTo(Ipv4Address("7.0.0.0"), Ipv4Mask("255.0.0.0"), 1);

    uint16_t echoPort = 9;
    UdpEchoServerHelper echoServer(echoPort);
    ApplicationContainer serverApps = echoServer.Install(remoteHost);
    serverApps.Start(Seconds(1.0));
    serverApps.Stop(Seconds(20.0));

    for (uint32_t i = 0; i < ueNodes.GetN(); ++i) {
        UdpEchoServerHelper ueEchoServer(echoPort);
        ApplicationContainer app = ueEchoServer.Install(ueNodes.Get(i));
        app.Start(Seconds(1.0));
        app.Stop(Seconds(20.0));
    }

    ApplicationContainer clientApps;
    for (uint32_t i = 0; i < ueNodes.GetN(); ++i) {
        Simulator::Schedule(Seconds(2.0 + i), &PrintClientSending, i + 1);

        UdpEchoClientHelper echoClient(remoteHostAddr, echoPort);
        echoClient.SetAttribute("MaxPackets", UintegerValue(100));
        echoClient.SetAttribute("Interval", TimeValue(Seconds(0.01)));
        echoClient.SetAttribute("PacketSize", UintegerValue(2048));

        ApplicationContainer apps = echoClient.Install(ueNodes.Get(i));
        apps.Start(Seconds(2.0 + i));
        apps.Stop(Seconds(20.0));
        clientApps.Add(apps);
    }

    ApplicationContainer remoteClients;
    for (uint32_t i = 0; i < ueNodes.GetN(); ++i) {
        UdpEchoClientHelper reverseClient(ueIpIfaces.GetAddress(i), echoPort);
        reverseClient.SetAttribute("MaxPackets", UintegerValue(100));
        reverseClient.SetAttribute("Interval", TimeValue(Seconds(0.01)));
        reverseClient.SetAttribute("PacketSize", UintegerValue(2048));

        ApplicationContainer apps = reverseClient.Install(remoteHost);
        apps.Start(Seconds(3.0 + i));
        apps.Stop(Seconds(20.0));
        remoteClients.Add(apps);
    }

    p2ph.EnablePcapAll("iot-5g-sim");

    FlowMonitorHelper flowmonHelper;
    Ptr<FlowMonitor> flowmon = flowmonHelper.InstallAll();

    Simulator::Stop(Seconds(21.0));
    Simulator::Run();

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
