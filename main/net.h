#pragma once
// WiFi SoftAP bring-up. SSID/password/channel come from config.
//
// Creates the AP, assigns 192.168.4.1, and runs the DHCP server so phones
// auto-join and get an IP.

void net_start_ap(void);
