import collections
import json
import os
import porto
import re
import subprocess

from test_common import *

ID = 'porto-net-sched'

server1 = ("fd00::1", 5201)
server2 = ("fd00::1", 5202)

dev = "eth0"
qdisc = ""
rate = 10


def print_all_qdiscs():
    print("\nCurrent qdiscs after Porto reload:")
    subprocess.call(["tc", "qdisc"])
    print()


def setup_net():
    with open('/proc/sys/net/ipv6/conf/all/forwarding', 'w') as f:
        f.write('1')
    with open('/proc/sys/net/ipv6/conf/all/accept_dad', 'w') as f:
        f.write('0')
    with open('/proc/sys/net/ipv6/conf/default/accept_dad', 'w') as f:
        f.write('0')
    with open('/proc/sys/net/ipv6/conf/all/dad_transmits', 'w') as f:
        f.write('0')
    with open('/proc/sys/net/ipv6/conf/default/dad_transmits', 'w') as f:
        f.write('0')

    subprocess.check_call(['ip', 'link', 'add', ID, 'type', 'dummy'])
    subprocess.check_call(['ip', 'address', 'add', 'fd00::1/64', 'dev', ID])
    subprocess.check_call(['ip', 'link', 'set', ID, 'up'])


def cleanup_net():
    subprocess.run(["ip", "link", "del", "dev", ID])


def start_iperf_servers():
    ConfigurePortod('test-net-sched', """
network {{
    managed_device: "{}"
}}
""".format(ID))

    run_iperf_server('veth-server1', *server1)
    run_iperf_server('veth-server2', *server2)

    cl = run_iperf_client('cl', server1, time=1, wait=3)
    res = int(cl['exit_code'])
    cl.Destroy()

    ExpectEq(res, 0)


def qdisc_is_classful():
    return qdisc == "htb" or qdisc == "hfsc"

def get_tx_queues_num():
    try:
        queues = os.listdir("/sys/class/net/%s/queues/" % dev)
        return len([q for q in queues if q.startswith("tx")])
    except FileNotFoundError:
        return 1

tx_queues_num = get_tx_queues_num()

def check_expected_qdisc():
    qdisc_pattern = r"""
        ^qdisc\s+
        (?P<qdisc>[a-z-_]+)\s+
        (?P<handle>[a-f\d\:]+)\s+
        .*$"""
    qdisc_re = re.compile(qdisc_pattern, re.X)

    qdiscs = collections.defaultdict(list)
    output = subprocess.check_output(["tc", "qdisc", "show", "dev", dev]).decode("utf-8")
    for line in output.splitlines():
        m = qdisc_re.match(line)
        if not m or len(m.groups()) != 2:
            continue
        result = m.groupdict()
        qdiscs[result.pop("qdisc")].append(result.pop("handle"))

    if qdisc_is_classful():
        Expect(len(qdiscs) == 2)
        Expect(len(qdiscs[qdisc]) == 1)
    else:
        if tx_queues_num > 1:
            Expect(len(qdiscs) == 2)
            Expect(len(qdiscs["mq"]) == 1)
        else:
            Expect(len(qdiscs) == 1)
        Expect(len(qdiscs[qdisc])) == tx_queues_num

def get_tc_classes():
    classes = set()

    class_pattern = r"""
        ^class\s+
        %s\s+
        (?P<class>[a-f\d\:]+)\s+
        .*$""" % qdisc
    class_re = re.compile(class_pattern, re.X)

    output = subprocess.check_output(["tc", "class", "show", "dev", dev]).decode("utf-8")
    for line in output.splitlines():
        m = class_re.match(line)
        if not m or len(m.groups()) != 1:
            continue
        result = m.groupdict()
        classes.add(result.pop("class"))

    return classes

def get_porto_net_classes(ct):
    return set(str(cls.split(': ')[1]) for cls in ct.GetProperty("net_class_id").split(';'))

def check_porto_net_classes(*cts):
    tc_classes = get_tc_classes()
    for ct in cts:
        ct_classes = get_porto_net_classes(ct)
        Expect(ct_classes.issubset(tc_classes), message=ct_classes.difference(tc_classes))

def run_iperf_client(name, server, wait, udp=False, mtn=False, cs=None, reverse=False, cfg={}, **kwargs):
    command = ['iperf3', '--client', server[0], '--port', server[1], '--json']

    if cs is not None:
        command += ['--tos', hex((cs & 7) << 6)]

    for k, v in kwargs.items():
        key = '--{}{}'.format(
            k.replace('_', '-'),
            "" if v is None else "={}".format(v)
        )
        command.append(key)

    if reverse:
        command.append('--reverse')
    if udp:
        command.append('--udp')
    # TODO: uncomment this after we get rid of xenial
    # command.append('--connect-timeout={}'.format(1000))

    print(command)

    net = "inherited"
    ip = ""
    if mtn:
        net = "L3 veth"
        ip = "veth fd00::100/128"

    ct = conn.Run(os.path.join(ID, name), command_argv='\t'.join(map(str, command)), wait=wait, net=net, ip=ip, **cfg)
    assert int(ct['exit_code']) == 0, 'stdout:\n{}\nstderr:\n{}'.format(ct['stdout'], ct['stderr'])

    # check classes of dead container
    if qdisc_is_classful():
        check_porto_net_classes(ct)
    else:
        ExpectProp(ct, "net_class_id", "1:0")
    return ct

def run_iperf_server(name, addr, port):
    command = "iperf3 --server --bind {} -p {}".format(addr, port)
    print(command)
    return conn.Run(os.path.join(ID, name), command=command, weak=False)

def bps(ct):
    ct.WaitContainer(5)
    ExpectEq(ct['state'], 'dead')
    res = json.loads(ct['stdout'])
    ct.Destroy()
    return res['end']['sum_sent']['bits_per_second'] / 2.**23

def sent_bytes(ct):
    ct.WaitContainer(5)
    res = json.loads(ct['stdout'])
    ct.Destroy()
    return res['end']['sum_sent']['bytes']


def run_host_limit_test():
    print("Test net_limit through qdisc classes")

    a = run_iperf_client("test-net-a", server1, time=3, wait=5, cfg={"net_limit": "default: 0"})
    res = bps(a)
    print("net_limit inf -> ", res)
    ExpectRange(res, rate * 0.8, rate * 1.33)

    a = run_iperf_client("test-net-a", server1, time=3, wait=5, cfg={"net_limit": "default: %sM" % int(rate * 0.1)})
    res = bps(a)
    print("net_limit %sM -> " % int(rate * 0.1), res)
    ExpectRange(res, rate * 0.05, rate * 0.2)

def run_mtn_limit_test():
    print("Test net_limit in MTN")

    a = run_iperf_client("test-net-a", server1, time=3, wait=10, mtn=True, cfg={"net_limit": "default: %sM" % rate})
    res = bps(a)
    print("net_limit %sM -> " % rate, res)
    ExpectRange(res, rate * 0.9, rate * 1.7)

    print("Test net_rx_limit in MTN")

    a = run_iperf_client("test-net-a", server1, time=3, wait=10, mtn=True, reverse=True, cfg={"net_rx_limit": "default: %sM" % rate})
    res = bps(a)
    print("net_rx_limit %sM -> " % rate, res)
    ExpectLe(res, rate * 1.7)

    print("Test both net_limit and net_rx_limit in MTN")

    a = run_iperf_client("test-net-a", server1, time=3, wait=10, mtn=True, reverse=False, cfg={"net_rx_limit": "default: %sM" % rate, "net_limit": "default: %sM" % rate})
    res = bps(a)
    print("net_limit/net_rx_limit %sM -> " % rate, res)
    ExpectLe(res, rate * 1.7)

    a = run_iperf_client("test-net-a", server1, time=3, wait=10, mtn=True, reverse=True, cfg={"net_rx_limit": "default: %sM" % rate, "net_limit": "default: %sM" % rate})
    res = bps(a)
    print("net_limit/net_rx_limit and reverse %sM -> " % rate, res)
    ExpectLe(res, rate * 1.7)

    if qdisc in ["fq_codel", "pfifo_fast"]:
        print("Check tx drops and overlimits")
        a = run_iperf_client("test-net-a", server1, time=5, bandwidth=0, length=1300, wait=10, udp=True, mtn=True, cfg={"net_limit": "default: 10M"})
        tx_drops = int(a["net_tx_drops[group default]"])
        tx_overlimits = int(a["net_overlimits[group default]"])
        ExpectLe(1, tx_overlimits)
        ExpectLe(100, tx_drops + tx_overlimits)
        rx_drops = int(a["net_rx_drops[group default]"])
        rx_overlimits = int(a["net_rx_overlimits[group default]"])
        ExpectLe(rx_drops + rx_overlimits, 10)
        #ExpectPropGe(a, "net_snmp[RetransSegs]", 1)

        Expect(int(a['net_tx_max_speed']) > 50 * 2**20)
        Expect(int(a['net_rx_max_speed']) < 2**20)

        tx_hgram = a['net_tx_speed_hgram'].split(';')
        rx_hgram = a['net_rx_speed_hgram'].split(';')
        ExpectEq(49, len(tx_hgram))
        ExpectEq(49, len(rx_hgram))

        def GetBytes(hgram):
            mbytes = 0
            for kv in hgram:
                bucket, value = [ int(v) for v in kv.split(':') ]
                # 25ms is watchdog period
                mbytes += value * bucket * 0.025
            return mbytes * 2**20

        tx = GetBytes(tx_hgram)
        rx = GetBytes(rx_hgram)
        ExpectRange(tx / float(a['net_tx_bytes[veth]']), 0.4, 1.2)
        ExpectEq(0, rx)

        a.Destroy()

        print("Check rx drops and overlimits")
        a = run_iperf_client("test-net-a", server1, time=5, bandwidth=0, length=1300, wait=10, udp=True, mtn=True, reverse=True, cfg={"net_rx_limit": "default: 50M"})
        tx_drops = int(a["net_tx_drops[group default]"])
        tx_overlimits = int(a["net_overlimits[group default]"])
        ExpectLe(tx_drops + tx_overlimits, 10)
        rx_drops = int(a["net_rx_drops[group default]"])
        rx_overlimits = int(a["net_rx_overlimits[group default]"])
        ExpectLe(1, rx_overlimits)
        ExpectLe(100, rx_drops + rx_overlimits)
        #ExpectPropGe(a, "net_snmp[RetransSegs]", 1)

        Expect(int(a['net_rx_max_speed']) > 50 * 2**20)
        Expect(int(a['net_tx_max_speed']) < 2.**20)

        tx_hgram = a['net_tx_speed_hgram'].split(';')
        rx_hgram = a['net_rx_speed_hgram'].split(';')
        ExpectEq(49, len(tx_hgram))
        ExpectEq(49, len(rx_hgram))
        tx = GetBytes(tx_hgram)
        rx = GetBytes(rx_hgram)
        ExpectRange(rx / float(a['net_rx_bytes[veth]']), 0.4, 1.2)
        ExpectEq(0, tx)

        a.Destroy()


def run_classless_test():
    check_expected_qdisc()
    ExpectException(run_host_limit_test, AssertionError)
    run_mtn_limit_test()

def set_qdisc(q):
    global qdisc
    qdisc = q
    print("Test %s scheduler" % qdisc)

def run_pfifo_fast_test():
    set_qdisc("pfifo_fast")

    ConfigurePortod('test-net-sched', """
network {{
    managed_device: "{}"
    enable_host_net_classes: false,
    default_qdisc: "default: {}"
}}
""".format(ID, qdisc))

    print_all_qdiscs()

    run_classless_test()

def run_fq_codel_test():
    set_qdisc("fq_codel")

    ConfigurePortod('test-net-sched', """
network {{
    managed_device: "{}"
    enable_host_net_classes: false,
    default_qdisc: "default: {}"
}}
""".format(ID, qdisc))

    print_all_qdiscs()

    run_classless_test()

def run_sock_diag_test():
    print("Test non mtn container net stat")

    set_qdisc("fq_codel")

    print_all_qdiscs()

    ConfigurePortod('test-net-sched', """
network {{
    enable_host_net_classes: false,
    watchdog_ms: 100,
    sock_diag_update_interval_ms: 500,
    default_qdisc: "default: {}"
}}
""".format(qdisc))

    meta_a = conn.Create(os.path.join(ID, 'meta-a'), weak=True)
    meta_a.Start()

    root_ct = conn.Find('/')
    loss_bytes_coeff = 4.5 / 5 - 0.01 # (iperf_time - sock_diag_update_interval_ms) / iperf_time - 1%
    iperf_total_sent = 0.

    # check ct stats
    a = run_iperf_client("meta-a/test-net-a", server1, time=5, wait=10)
    tx_bytes = float(a.GetProperty('net_tx_bytes[Uplink]'))
    iperf_sent_bytes = sent_bytes(a)
    ExpectRange(tx_bytes / iperf_sent_bytes, loss_bytes_coeff, 1.01)

    # check parent stats
    parent_tx_bytes = float(meta_a.GetProperty('net_tx_bytes[Uplink]'))
    ExpectRange(parent_tx_bytes / iperf_sent_bytes, loss_bytes_coeff, 1.01)

    # check root container stats
    iperf_total_sent += iperf_sent_bytes
    root_tx_bytes = float(root_ct.GetProperty('net_tx_bytes[SockDiag]'))
    root_rx_bytes = float(root_ct.GetProperty('net_rx_bytes[SockDiag]'))
    ExpectRange(root_tx_bytes / iperf_sent_bytes, loss_bytes_coeff, 1.01)
    ExpectRange(root_rx_bytes / iperf_sent_bytes, loss_bytes_coeff, 1.01)

    # check ct stats
    b = run_iperf_client("meta-a/test-net-a", server1, time=5, wait=10, reverse=True)
    rx_bytes = float(b.GetProperty('net_rx_bytes[Uplink]'))
    iperf_sent_bytes = sent_bytes(b)
    ExpectRange(rx_bytes / iperf_sent_bytes, loss_bytes_coeff, 1.01)

    # check parent stats
    parent_rx_bytes = float(meta_a.GetProperty('net_rx_bytes[Uplink]'))
    ExpectRange(parent_rx_bytes / iperf_sent_bytes, loss_bytes_coeff, 1.01)

    # check root container stats
    iperf_total_sent += iperf_sent_bytes
    root_rx_bytes = float(root_ct.GetProperty('net_rx_bytes[SockDiag]'))
    root_tx_bytes = float(root_ct.GetProperty('net_tx_bytes[SockDiag]'))
    ExpectRange(root_rx_bytes / iperf_total_sent, loss_bytes_coeff, 1.01)


conn = porto.Connection(timeout=60)

def main():
    ct = conn.Run(ID, isolate='false', enable_porto='true', weak=False)
    try:
        print("Setup network")
        setup_net()

        print("Start iperf servers")
        start_iperf_servers()

        run_pfifo_fast_test()
        run_fq_codel_test()
        run_sock_diag_test()

        ExpectEq(int(conn.GetProperty('/', 'porto_stat[errors]')), 0)
    finally:
        ConfigurePortod('test-net-sched', "")
        ct.Destroy()
        cleanup_net()


if __name__ == '__main__':
    main()
