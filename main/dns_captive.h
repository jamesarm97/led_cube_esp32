#pragma once
// Captive-portal DNS. Tiny UDP server on port 53 that replies to every A-record
// query with the AP's IP (192.168.4.1). Phones probe known URLs and, on
// getting our IP back, pop the "Sign in to network" sheet pointing at our UI.

void dns_captive_start(void);
