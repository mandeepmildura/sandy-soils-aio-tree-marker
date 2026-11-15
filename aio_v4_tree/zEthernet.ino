bool ethStarted = false;

// treeline: once a second, put the static IP back if anything (a DHCP client
// restarted by a link drop) has cleared it.
void ethKeepStaticIP()
{
  static elapsedMillis t;
  if (!ethStarted || t < 1000) return;
  t = 0;
  if (Ethernet.localIP() != IPAddress(Eth_myip)) {
    fnet_dhcp_cln_release(fnet_dhcp_cln_get_by_netif(fnet_netif_get_default()));
    Ethernet.setLocalIP(Eth_myip);
    Serial.print("ETH: static IP restored to "); Serial.println(Ethernet.localIP());
  }
}

void EthernetStart()
{
  // start the Ethernet connection:
  Serial.println("Initializing ethernet with static IP address");

  // try to congifure using IP:
  Ethernet.begin(mac,0);          // Start Ethernet with IP 0.0.0.0

  // Check for Ethernet hardware present
  if (Ethernet.hardwareStatus() == EthernetNoHardware) 
  {
    Serial.println("Ethernet shield was not found. GPS via USB only.");

    return;
  }

  if (Ethernet.linkStatus() == LinkOFF) 
  {
    Serial.println("Ethernet cable is not connected - Who cares we will start ethernet anyway.");
  }

//grab the ip from EEPROM
  Eth_myip[0] = networkAddress.ipOne;
  Eth_myip[1] = networkAddress.ipTwo;
  Eth_myip[2] = networkAddress.ipThree;
  if (Autosteer_running) 
  {
    Eth_myip[3] = 126;  //126 is steer module, with or without GPS
  }
  else
  {
    Eth_myip[3] = 120;  //120 is GPS only module
  }

  // treeline: begin(mac,0) starts FNET's DHCP client if the link is already
  // up; with no DHCP server it later resets the IP to 0.0.0.0 and the board
  // drops off the network. Stop it before setting the static IP.
  fnet_dhcp_cln_release(fnet_dhcp_cln_get_by_netif(fnet_netif_get_default()));
  Ethernet.setLocalIP(Eth_myip);  // Change IP address to IP set by user
  ethStarted = true;
  Serial.println("\r\nEthernet status OK");
  Serial.print("IP set Manually: ");
  Serial.println(Ethernet.localIP());

  Eth_ipDestination[0] = Eth_myip[0];
  Eth_ipDestination[1] = Eth_myip[1];
  Eth_ipDestination[2] = Eth_myip[2];
  Eth_ipDestination[3] = 255;

  Serial.print("\r\nEthernet IP of module: "); Serial.println(Ethernet.localIP());
  Serial.print("Ethernet sending to IP: "); Serial.println(Eth_ipDestination);
  Serial.print("All data sending to port: "); Serial.println(portDestination);

  // init UPD Port sending to AOG
  if (Eth_udpPAOGI.begin(portMy))
  {
    Serial.print("Ethernet GPS UDP sending from port: ");
    Serial.println(portMy);
  }

  // init UPD Port getting NTRIP from AOG
  if (Eth_udpNtrip.begin(AOGNtripPort)) // AOGNtripPort
  {
    Serial.print("Ethernet NTRIP UDP listening to port: ");
    Serial.println(AOGNtripPort);
  }

  // init UPD Port getting AutoSteer data from AOG
  if (Eth_udpAutoSteer.begin(AOGAutoSteerPort)) // AOGAutoSteerPortipPort
  {
    Serial.print("Ethernet AutoSteer UDP listening to & send from port: ");
    Serial.println(AOGAutoSteerPort);
  }
}
