# SNMP

A read-only SNMPv2c agent on UDP port 161, off by default.

```
snmp on|off
snmp community <name>     # default public
snmp location <text>      # sysLocation
snmp contact <text>       # sysContact
snmp                      # settings
```

Texts have up to 48 characters, without `"` and `\`. A request with another
community gets no answer, and the community crosses the network in clear
text, as always with SNMPv2c.

It answers Get, GetNext and GetBulk with

* the system group (1.3.6.1.2.1.1);
* ifNumber and ifTable (1.3.6.1.2.1.2), columns 1 to 20;
* ifXTable (1.3.6.1.2.1.31.1.1), columns 1 to 13, 15, 17 and 18, with the
  64-bit counters.

ifIndex is the front port number and ifAlias the port's name. The counters
are the ASIC's. ifLastChange is always 0. There are no traps.
