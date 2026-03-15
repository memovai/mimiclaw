#!/usr/bin/env python3
import ipaddress
import re
from pathlib import Path


SRC = Path(__file__).resolve().parents[1] / "main" / "onboard" / "wifi_onboard.c"


def candidates_from_source():
    text = SRC.read_text()
    body = re.search(r"ONBOARD_AP_IP_CANDIDATES\[\]\s*=\s*\{(.*?)\};", text, re.S).group(1)
    return [
        ipaddress.ip_address(".".join(parts))
        for parts in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", body)
    ]


def choose(sta_cidr):
    sta = ipaddress.ip_interface(sta_cidr)
    for candidate in candidates_from_source():
        if candidate not in sta.network:
            return candidate
    return candidates_from_source()[0]


def main():
    text = SRC.read_text()
    assert choose("192.168.4.68/22") == ipaddress.ip_address("192.168.50.1")
    assert choose("192.168.50.23/24") == ipaddress.ip_address("192.168.60.1")
    assert "http://192.168.4.1" not in text
    assert "ONBOARD_AP_IP_STR" not in text
    assert "s_onboard_ap_ip_info.ip" in text


if __name__ == "__main__":
    main()
