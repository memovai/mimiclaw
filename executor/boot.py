"""
MimiClaw Board B (Executor) - boot.py
Initializes WiFi STA interface for ESP-NOW (no connection needed).
Prints MAC address for pairing with Board A.
"""

import network
import ubinascii

# Enable WiFi STA (required for ESP-NOW, but no need to connect)
sta = network.WLAN(network.STA_IF)
sta.active(True)

# Print MAC address for Board A configuration
mac = sta.config('mac')
mac_str = ubinascii.hexlify(mac, ':').decode()
print('Board B MAC:', mac_str)
print('On Board A, run: set_espnow_peer', mac_str)
