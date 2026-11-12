"""Check that the network soft-limit qdisc and BPF filter survive portod reload."""

import json
import subprocess
import time

import porto
from test_common import ConfigurePortod, ExpectEq, GetPortodPid, ReloadPortod, RunContainer


NAME = 'test-net-limit-soft'
LIMIT = 10 * 1024 * 1024
PRIORITY = 61357


def tc(pid, *args):
    output = subprocess.check_output(
        ['nsenter', '--target', str(pid), '--net', 'tc', '-j'] + list(args))
    return json.loads(output)


def check_filter(pid):
    qdiscs = tc(pid, 'qdisc', 'show', 'dev', 'veth')
    clsact = [q for q in qdiscs if q['kind'] == 'clsact']
    ExpectEq(len(clsact), 1)
    ExpectEq(clsact[0]['handle'], 'ffff:')

    filters = tc(pid, 'filter', 'show', 'dev', 'veth', 'egress', 'chain', '0', 'pref', str(PRIORITY))
    # tc also emits a classifier summary without individual filter options.
    filters = [f for f in filters if f.get('options')]
    ExpectEq(len(filters), 1)
    filt = filters[0]
    ExpectEq(filt['kind'], 'bpf')
    ExpectEq(filt['protocol'], 'all')
    options = filt['options']
    ExpectEq(options['bpf_name'], 'porto%netlimit_soft')
    ExpectEq(options['direct-action'], True)
    program = options.get('prog', options)
    assert program['id'] > 0, filt
    assert program['tag'], filt
    return {'handle': options['handle'], 'id': program['id'], 'tag': program['tag']}


def wait_filter(pid, deadline, old_id=None):
    last_error = 'BPF program was not replaced'
    while True:
        try:
            state = check_filter(pid)
            if state['id'] != old_id:
                return state
        except AssertionError as error:
            last_error = str(error)
        assert time.monotonic() < deadline, 'soft-limit filter not ready: {}'.format(last_error)
        time.sleep(0.05)


def test_reload():
    conn = porto.Connection(timeout=30)
    try:
        ConfigurePortod(NAME, """
network {
    network_limit_soft_bpf_elf_path: "/usr/lib/porto/netlimit_soft.o"
}
""")
        with RunContainer(conn, NAME, weak=False, command='sleep 300', net='L3 veth',
                          net_limit_soft=str(LIMIT)) as ct:
            pid = ct['root_pid']
            before = wait_filter(pid, time.monotonic() + 30)
            old_server = GetPortodPid()

            ReloadPortod()
            deadline = time.monotonic() + 30
            while GetPortodPid() == old_server:
                assert time.monotonic() < deadline, 'portod did not reload'
                time.sleep(0.05)

            conn.Disconnect()
            ExpectEq(ct['state'], 'running')
            ExpectEq(ct['root_pid'], pid)
            ExpectEq(ct['net_limit_soft'], str(LIMIT))
            # Network repair is asynchronous: require the new daemon's attachment.
            after = wait_filter(pid, deadline, old_id=before['id'])
            ExpectEq(after['handle'], before['handle'])
            ExpectEq(after['tag'], before['tag'])
    finally:
        ConfigurePortod(NAME, '')


if __name__ == '__main__':
    test_reload()
