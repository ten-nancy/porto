from test_common import *
import porto

conn = porto.Connection(timeout=30)

def CheckEthIface(output, shouldInherit=True, ifaceName="eth0"):
    all_ipv6_ifaces = set(map(lambda x: x.strip().split()[-1], output.strip().split('\n')))
    if shouldInherit:
        Expect(ifaceName in all_ipv6_ifaces)
    else:
        Expect(ifaceName not in all_ipv6_ifaces)

def TestXvlanInheritance(inherit):
    if inherit:
        ConfigurePortod('test-net-xvlan', """
        network {
            network_inherit_xvlan: true
        }""")
    else:
        ConfigurePortod('test-net-xvlan', """
        network {
            network_inherit_xvlan: false
        }""")

    a = conn.Run(
        "a",
        net="L3 veth; ipvlan eth0 eth0;",
        ip="veth 198.51.100.0",
        default_gw="veth 198.51.100.1"
    )
    b = conn.Run("a/b", net="inherited")
    c = conn.Run(
        "a/b/c",
        net="L3 veth;",
        ip="veth 198.51.100.0",
        default_gw="veth 198.51.100.1",
        command="cat /proc/net/if_inet6",
        wait=1
    )
    CheckEthIface(c['stdout'], inherit)

    d = conn.Run(
        "a/b/d",
        net="L3 veth; ipvlan eth0 eth0;",
        ip="veth 198.51.100.0",
        default_gw="veth 198.51.100.1",
        command="cat /proc/net/if_inet6",
        wait=1
    )
    CheckEthIface(d['stdout'], True) # it should always has eth0

    a.Destroy()


TestXvlanInheritance(True)
TestXvlanInheritance(False)
