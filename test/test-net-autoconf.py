import porto
import socket
import subprocess
import threading
import traceback

from test_common import *

# Netlink has global locks, so autoconf done from a forked child could hang it.
# Start and stop autoconf containers in parallel to catch that.

ID = 'porto-autoconf'
THREADS = 8
ITERATIONS = 100

IFA_F_TENTATIVE = 0x40
IFA_F_DADFAILED = 0x08


def setup_net():
    subprocess.check_call(['ip', 'link', 'add', ID, 'type', 'dummy'])
    subprocess.check_call(['ip', 'address', 'add', 'fd00::1/64', 'dev', ID])
    subprocess.check_call(['ip', 'link', 'set', ID, 'up'])


def cleanup_net():
    subprocess.run(['ip', 'link', 'del', 'dev', ID])


def ParseInet6(output):
    # <addr> <ifindex> <prefixlen> <scope> <flags> <iface>
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 6:
            yield fields[5], fields[0], int(fields[4], 16)


def CheckAutoconf(ct, addr):
    for name, addr1, flags in ParseInet6(ct['stdout']):
        if name == 'veth' and addr1 == socket.inet_pton(socket.AF_INET6, addr).hex():
            Expect(not flags & IFA_F_TENTATIVE, 'dad is not completed at veth: {:#x}'.format(flags))
            Expect(not flags & IFA_F_DADFAILED, 'dad failed at veth: {:#x}'.format(flags))


def work(name, addr):
    # Container hang is detected by the connection timeout
    conn = porto.Connection(timeout=10)
    ct = conn.Create(name)
    try:
        ct['net'] = 'L3 veth'
        ct['ip'] = 'veth {}'.format(addr)
        # Autoconf must be completed before exec
        ct['command'] = 'cat /proc/net/if_inet6'

        for i in range(ITERATIONS):
            ct.Start()
            ct.WaitContainer(timeout=10)
            ExpectEq(ct['exit_code'], '0')
            CheckAutoconf(ct, addr)
            ct.Stop()
    finally:
        ct.Destroy()


def main():
    errors = []

    def run(name, addr):
        try:
            work(name, addr)
        except Exception:
            errors.append(traceback.format_exc())

    threads = []
    for i in range(THREADS):
        t = threading.Thread(target=run, args=('test-autoconf{}'.format(i), 'fd00::{}'.format(i + 2)))
        t.start()
        threads.append(t)

    for t in threads:
        t.join()

    for error in errors:
        print(error)

    ExpectEq(errors, [])


setup_net()
try:
    main()
finally:
    cleanup_net()
