# Port authentication (802.1X)

The switch can be an 802.1X authenticator: a port set to `auto` carries only
EAPOL until a RADIUS server accepts the device on it. One accepted device
opens the port.

```
dot1x radius <a.b.c.d> [<udp port>]   # the RADIUS server, port 1812 by default
dot1x secret <text>                   # its shared secret, up to 64 characters
dot1x port <n> auto                   # authenticate what is connected
dot1x port <n> force-auth             # no authentication (default)
dot1x port <n> force-unauth           # blocked
dot1x port <n> mab on|off             # MAC authentication, see below
dot1x port <n> restart                # authenticate again now
dot1x reauth <60-86400>|off           # seconds between re-authentications
dot1x on|off
dot1x                                 # status
```

Keep the port you manage the switch from at `force-auth`: frames from a
closed port do not reach the management interface either.

The EAP method is between the device and the server; the switch only relays
it. Access-Requests carry User-Name, NAS-IP-Address, NAS-Port (the front
port), Called- and Calling-Station-Id and a Message-Authenticator, and an
answer whose authenticators do not check out is ignored. Session-Timeout sets
the next re-authentication; a port stays open while re-authentication runs.
VLAN and other attributes are not used.

With `mab on`, a device that leaves three EAP requests unanswered (90 s) is
authenticated by its MAC address, twelve lower case digits as both user name
and password, with Service-Type Call-Check.

A failed device waits 60 s before the next try. A link that goes down, even
briefly, closes its port.

A RADIUS packet must fit one Ethernet frame: the switch has no IP fragments.
It asks the server to stay below that with Framed-MTU 1000, but a supplicant
may send EAP-TLS fragments of 1400 bytes when its certificate is that large.
Set its fragment size to 1200 or less then (wpa_supplicant:
`fragment_size=1200`). Up to four RADIUS requests are outstanding at a time.

A closed port also learns no addresses, so a device on it can't take over
another's; with `mab on` it learns one until the switch has seen a frame.
`l2 limit` applies once the port opens.

A closed port is kept apart with port isolation; `isolate` shows and sets the
configured members, which apply again once the port opens.
