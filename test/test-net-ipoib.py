from test_common import *
import porto

conn = porto.Connection(timeout=30)

ALIAS = "porto-ipoib"


def BaseIpoib():
    """An IPoIB port of the host, if there is one: a base interface is its own link, a child is not."""
    for name in sorted(os.listdir('/sys/class/net')):
        base = '/sys/class/net/' + name
        try:
            if open(base + '/type').read().strip() != '32':  # ARPHRD_INFINIBAND
                continue
            if open(base + '/iflink').read().strip() == open(base + '/ifindex').read().strip():
                return name
        except IOError:
            continue
    return None


def ReadSys(path):
    return open(path).read().strip()


def PortPkeys(name):
    """The pkey table of the port behind an IPoIB interface."""
    base = '/sys/class/net/' + name
    ibdev = os.listdir(base + '/device/infiniband')[0]
    port = int(ReadSys(base + '/dev_port')) + 1
    table = '/sys/class/infiniband/{}/ports/{}/pkeys/'.format(ibdev, port)
    return [ReadSys(table + str(i)) for i in range(len(os.listdir(table)))]


def ChildPkey(net):
    ct = conn.Run("test-ipoib-pkey", net=net, command="ip -d -o link show ibt0", wait=10)
    try:
        ExpectEq(ct['exit_status'], '0')
        return re.search(r'\bpkey\s+(0x[0-9a-f]+)', ct['stdout']).group(1)
    finally:
        ct.Destroy()


def HostIpoib():
    """IPoIB interfaces of the host namespace, by name, with their alias."""
    out = subprocess.check_output(['ip', '-d', '-o', 'link', 'show', 'type', 'ipoib']).decode()
    links = {}
    for line in out.strip().split('\n'):
        if not line.strip():
            continue
        name = line.split(':')[1].strip().split('@')[0]
        alias = re.search(r'\balias (\S+)', line)
        links[name] = alias.group(1) if alias else None
    return links


# The address of an IPoIB link is made by the kernel, so asking for one is refused when net is
# parsed, before any link is made: this needs no IPoIB port on the host.
ct = conn.Create("test-ipoib-mac")
try:
    ExpectException(ct.SetProperty, porto.exceptions.InvalidValue,
                    "net", "ipoib ibt0 ib0; MAC ibt0 02:00:00:00:00:01")
    for pkey in ["0", "0x8000", "0x10000", "idx:", "idx:-1", "idx:x"]:
        ExpectException(ct.SetProperty, porto.exceptions.InvalidValue, "net", "ipoib ibt0 ib0 " + pkey)
finally:
    ct.Destroy()

MASTER = BaseIpoib()
if MASTER is None:
    print("No IPoIB port on the host, skipping")
    sys.exit(0)

NET = "ipoib ibt0 " + MASTER

before = HostIpoib()

# The container sees its child, and porto marked it.
ct = conn.Run("test-ipoib", net=NET, command="ip -d -o link show ibt0", wait=10)
try:
    ExpectEq(ct['exit_status'], '0')
    Expect(ALIAS in ct['stdout'], "the child is not marked: " + ct['stdout'])
finally:
    ct.Destroy()

# Without a pkey the child takes the one of its master.
ExpectEq(ChildPkey(NET), ReadSys('/sys/class/net/' + MASTER + '/pkey'))

# idx:N takes the pkey at that index of the master's port; an empty entry is refused.
pkeys = PortPkeys(MASTER)
ExpectEq(int(ChildPkey(NET + " idx:0"), 16), int(pkeys[0], 16) | 0x8000)
empty = [i for i, p in enumerate(pkeys) if not int(p, 16) & 0x7fff]
if empty:
    ExpectException(conn.Run, porto.exceptions.InvalidValue, "test-ipoib-empty-idx",
                    net=NET + " idx:" + str(empty[0]), command="true", wait=10)
    ExpectException(conn.Find, porto.exceptions.ContainerDoesNotExist, "test-ipoib-empty-idx")

# The child goes away with the container. The kernel moves an IPoIB child back to the host when
# its namespace dies rather than deleting it, so without the removal the host gains a dev<ifindex>
# for every container that had a rail.
time.sleep(1)  # the namespace dies in the kernel's own time
ExpectEq(HostIpoib(), before)

# A child that got back to the host anyway - porto was not there to remove it - is collected when
# the next one is made. The stray is made by hand the way the kernel leaves one: a marked child of
# the same port in the host namespace.
STRAY = "ibt-stray"
subprocess.check_call(['ip', 'link', 'add', 'link', MASTER, 'name', STRAY, 'type', 'ipoib'])
try:
    subprocess.check_call(['ip', 'link', 'set', STRAY, 'alias', ALIAS])

    # An unmarked child is not porto's, and stays.
    OTHER = "ibt-other"
    subprocess.check_call(['ip', 'link', 'add', 'link', MASTER, 'name', OTHER, 'type', 'ipoib'])
    try:
        ct = conn.Run("test-ipoib-collect", net=NET, command="true", wait=10)
        ct.Destroy()

        links = HostIpoib()
        Expect(STRAY not in links, "the stray child is still in the host")
        Expect(OTHER in links, "a child porto did not make was removed")
    finally:
        subprocess.call(['ip', 'link', 'del', OTHER])
finally:
    subprocess.call(['ip', 'link', 'del', STRAY], stderr=open(os.devnull, 'w'))

time.sleep(1)
ExpectEq(HostIpoib(), before)
