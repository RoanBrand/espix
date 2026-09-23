# Routing, NAT and bridging

espix forwards IPv4 between its interfaces and can bridge at layer 2. Both are
IPv4, and neither is a firewall: nothing is blocked.

## Interfaces

| name    | what |
|---------|------|
| `wlan0` | WiFi station (uplink) |
| `wlan1` | WiFi access point; a router AP by default, a bridge port if listed in `/etc/bridge.conf` |
| `eth0`  | Ethernet (S31); an addressable interface, or a bridge port if listed |
| `usb0`  | USB-NCM (device-role builds) |
| `br0`   | the bridge, when configured |

## NAT (a router)

`ESPIX_NET_ROUTER` (on by default) compiles in IPv4 forwarding and NAPT.

    nat status              # per interface
    nat on wlan1            # masquerade wlan1 out the default route
    nat off wlan1

An access point started with `wifi ap start` turns NAPT on for `wlan1` itself,
so an AP with an uplink is a working gateway. **The AP's clients are NATed, not
put on your subnet.**

## Bridging (one L2 segment)

`ESPIX_NET_BRIDGE` (on by default). The bridge owns the address and, if it
serves, the DHCP server; the ports carry frames and have neither.

    /etc/bridge.conf
      ports=eth0,wlan1
      addr=client           # client: the LAN's DHCP; server: br0 serves

    bridge show
    bridge add|del <port>   # writes the file; takes effect after reboot
    bridge addr client|server

`client` is the common "extend my LAN" case: `br0` leases an address from the
house router, and the AP's clients get one from it too -- the AP and the wire
are one segment. `server` is a standalone segment where `br0` hands out
addresses itself.

## Limitations

- A **station can never be a bridge port**. 802.11 frames carry three
  addresses; a bridge needs the fourth (WDS), which this radio does not do.
  Use NAT (`wifi ap` + `nat on wlan1`) or the L2 forwarder.
- **Membership is a boot-time choice.** A port is created as a port, so
  `bridge add/del` writes the file and asks for a reboot rather than pulling the
  address out from under live clients.
- **One AP.** The radio serves one softAP; there is no trusted+guest SSID pair.
- **No route table.** lwIP routes by interface subnet plus one default gateway;
  `ip route` is a view, not a table.
- **NAT is not a firewall.** Guest isolation and port-forwards are not here yet.
- Bridging puts the AP's clients on your LAN's L2; NAT isolates them. Choose per
  AP.

## L2 forwarder (planned, not built)

The one thing layer-2 bridging cannot carry is a WiFi **station**: 802.11
frames have three addresses, not four. The opt-in alternative is a 1-1
forwarder (IDF's `sta2eth`): relay raw frames between `wlan0` and one wired
port (`eth0` or `usb0`), rewriting MACs so the AP sees a single device.

It is deliberately last. It uses internal APIs (`esp_wifi_internal_tx`) and
promiscuous mode, it serves exactly one downstream client, and espix is not
reachable as itself on that uplink while it runs. NAT covers the same "give a
wired device WiFi" job today, with a double-NAT in the path.
