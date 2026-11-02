/* Interface flags, Linux's values, as getifaddrs() reports them. Both builds. */
#pragma once

#define IF_NAMESIZE 16

#define IFF_UP        0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK  0x8
#define IFF_RUNNING   0x40
#define IFF_MULTICAST 0x1000
