import contextlib
import multiprocessing
import os
import porto
import random
import sys
import time

from test_common import *

CPUNR = multiprocessing.cpu_count()


def parse_cpuset(s):
    xs = s.split(',')
    for x in xs:
        ys = x.split('-')
        if (len(ys) == 1):
            yield int(ys[0])
        else:
            a, b = ys
            for z in range(int(a), int(b) + 1):
                yield z


def get_intersection(a, b):
    a_set = set(parse_cpuset(a['cpu_set_affinity']))
    b_set = set(parse_cpuset(b['cpu_set_affinity']))
    return a_set & b_set


def check_intersection(a, b):
    inter = get_intersection(a, b)
    ExpectEq(inter, set())


def check_affinity(ct, affinity):
    cgroup = ct.GetProperty('cgroups[cpuset]')
    ct_affinity = ct['cpu_set_affinity']
    with open(os.path.join(cgroup, 'cpuset.cpus')) as f:
        cg_affinity = str(f.read()).strip()

    ExpectEq(ct_affinity, cg_affinity)

    if isinstance(affinity, int):
        ExpectEq(len(set(parse_cpuset(ct_affinity))), affinity)
    elif isinstance(affinity, set):
        ExpectEq(set(parse_cpuset(ct_affinity)), affinity)
    else:
        assert False, "invalid affinity: {}".format(repr(affinity))

def test_validation(conn, cleanup):
    a = cleanup.enter_context(CreateContainer(conn, 'a'))
    ExpectException(a.SetProperty, porto.exceptions.InvalidValue, 'cpu_set', 'jail')
    ExpectException(a.SetProperty, porto.exceptions.InvalidValue, 'cpu_set', 'jail 0')
    ExpectException(a.SetProperty, porto.exceptions.InvalidValue, 'cpu_set', 'jail {}'.format(CPUNR))
    ExpectException(a.SetProperty, porto.exceptions.InvalidValue, 'cpu_set', 'jail {}'.format(CPUNR + 1))


def test_basic(conn, cleanup):
    a = cleanup.enter_context(CreateContainer(conn, 'a'))
    ExpectEq(a['cpu_set'], '')
    ExpectEq(a['cpu_set_affinity'], '')

    a.SetProperty('cpu_set', 'jail 1')
    ExpectEq(a['cpu_set'], 'jail 1')
    ExpectEq(a['cpu_set_affinity'], '')

    a.Start()
    check_affinity(a, 1)

    a.SetProperty('cpu_set', '')
    check_affinity(a, CPUNR)

    b = cleanup.enter_context(RunContainer(conn, 'b'))
    for i in range(1, CPUNR):
        j = CPUNR - i

        a.SetProperty('cpu_set', 'jail {}'.format(i))
        ExpectEq(a['cpu_set'], 'jail {}'.format(i))
        check_affinity(a, i)

        b.SetProperty('cpu_set', 'jail {}'.format(j))
        ExpectEq(b['cpu_set'], 'jail {}'.format(j))
        check_affinity(b, j)
        check_intersection(a, b)

    ReloadPortod()
    ExpectEq(a['cpu_set'], 'jail {}'.format(CPUNR - 1))
    check_affinity(a, CPUNR - 1)


def find_exclusive(containers):
    for a in containers:
        if all(not get_intersection(a, b) for b in containers if b != a):
            return a
    return None


def test_balance(conn, cleanup):
    nr = CPUNR//2
    a = cleanup.enter_context(RunContainer(conn, 'a', cpu_set='jail {}'.format(nr)))
    check_affinity(a, nr)

    b = cleanup.enter_context(RunContainer(conn, 'b', cpu_set='jail {}'.format(nr)))
    check_affinity(b, nr)
    check_intersection(a, b)

    c = cleanup.enter_context(RunContainer(conn, 'c', cpu_set='jail {}'.format(nr)))
    check_affinity(c, nr)

    containers = [a, b, c]
    victim = find_exclusive(containers)
    victim.Destroy()

    containers = [ct for ct in containers if ct != victim]

    for ct in containers:
        check_affinity(ct, nr)

    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            check_intersection(*containers)
            break
        except AssertionError:
            time.sleep(0.1)
    else:
        check_intersection(*containers)


def test_conflicts(conn, cleanup):
    a = cleanup.enter_context(RunContainer(conn, 'a', cpu_set='jail 2'))

    cpu_set = set(parse_cpuset(a['cpu_set_affinity']))
    cpu_set1 = random.sample(list(set(range(CPUNR)) - cpu_set), k=2)

    b = cleanup.enter_context(CreateContainer(conn, 'a/b'))
    b.SetProperty('controllers[cpuset]', 'true')
    b.Start()
    ExpectEq(b['cpu_set'], '')
    check_affinity(b, 2)

    # TODO(ovov): replace Unknown with something more appropriate
    ExpectException(b.SetProperty, porto.exceptions.Unknown, 'cpu_set', ','.join(map(str, cpu_set1)))
    ExpectEq(a['cpu_set'], 'jail 2')
    check_affinity(a, 2)
    ExpectEq(b['cpu_set'], '')
    check_affinity(b, 2)

    b.SetProperty('cpu_set', ','.join(map(str, cpu_set)))
    ExpectEq(a['cpu_set'], 'jail 2')
    check_affinity(a, 2)
    check_affinity(b, 2)

    ExpectException(a.SetProperty, porto.exceptions.Unknown, 'cpu_set', 'jail 1')
    ExpectEq(a['cpu_set'], 'jail 2')
    check_affinity(a, 2)
    check_affinity(b, 2)


def test_nested(conn, cleanup):
    a = cleanup.enter_context(RunContainer(conn, 'a', weak=False))
    b = cleanup.enter_context(RunContainer(conn, 'a/b', **{'controllers[cpuset]': True}, weak=False))

    b.SetProperty('cpu_set', 'jail 1')
    ExpectException(a.SetProperty, porto.exceptions.ResourceNotAvailable, 'cpu_set', 'jail 1')
    check_affinity(a, CPUNR)
    check_affinity(b, 1)

    b.SetProperty('cpu_set', '')
    a.SetProperty('cpu_set', 'jail 1')
    check_affinity(a, 1)
    check_affinity(b, 1)

    c = cleanup.enter_context(RunContainer(conn, 'a/b/c', **{'controllers[cpuset]': True}, weak=False))

    for i in range(2, 4):
        a.SetProperty('cpu_set', 'jail {}'.format(i))
        check_affinity(a, i)
        check_affinity(b, i)
        check_affinity(c, i)

    ReloadPortod()
    check_affinity(a, 3)
    check_affinity(b, 3)
    check_affinity(c, 3)


def test_modes(conn, cleanup):
    a = cleanup.enter_context(RunContainer(conn, 'a', cpu_set='jail 2; node 0', weak=False))
    ExpectEq(a['cpu_set'], 'jail 2; node 0')
    check_affinity(a, 2)

    a.SetProperty('cpu_set', 'node 0')
    ExpectEq(a['cpu_set'], 'node 0')
    check_affinity(a, CPUNR)

    a.SetProperty('cpu_set', 'jail 2')
    ExpectEq(a['cpu_set'], 'jail 2')
    check_affinity(a, 2)

    a.SetProperty('cpu_set', 'jail 2; node 0')
    ExpectEq(a['cpu_set'], 'jail 2; node 0')
    check_affinity(a, 2)

    ReloadPortod()

    ExpectEq(a['cpu_set'], 'jail 2; node 0')
    check_affinity(a, 2)

    # jail -> raw cores -> jail -> none

    a.SetProperty('cpu_set', 'jail 2')
    ExpectEq(a['cpu_set'], 'jail 2')
    check_affinity(a, 2)

    a.SetProperty('cpu_set', '1-2')
    ExpectEq(a['cpu_set'], '1-2')
    check_affinity(a, {1, 2})

    a.SetProperty('cpu_set', 'jail 2')
    ExpectEq(a['cpu_set'], 'jail 2')
    check_affinity(a, 2)

    a.SetProperty('cpu_set', '')
    ExpectEq(a['cpu_set'], '')
    check_affinity(a, CPUNR)


def test_dead(conn, cleanup):
    a = cleanup.enter_context(RunContainer(conn, 'a'))
    b = cleanup.enter_context(RunContainer(conn, 'a/b'))
    ExpectEq(a['cpu_set'], '')
    check_affinity(a, CPUNR)
    ExpectEq(b['cpu_set'], '')
    check_affinity(b, CPUNR)

    c = cleanup.enter_context(RunContainer(conn, 'a/b/c', command='true'))
    c.Wait()

    ExpectEq(c['state'], 'dead')

    a.SetProperty('cpu_set', 'jail 1')
    ExpectEq(a['cpu_set'], 'jail 1')
    check_affinity(a, 1)
    ExpectEq(b['cpu_set'], '')
    check_affinity(b, 1)
    ExpectEq(b['cpu_set'], '')
    check_affinity(c, 1)


def main():
    if CPUNR < 4:
        print("Insufficient number of cpus for this test. Exiting...")
        return

    conn = porto.Connection(timeout=30)

    with contextlib.ExitStack() as cleanup:
        test_validation(conn, cleanup)

    with contextlib.ExitStack() as cleanup:
        test_basic(conn, cleanup)

    with contextlib.ExitStack() as cleanup:
        test_conflicts(conn, cleanup)

    with contextlib.ExitStack() as cleanup:
        test_balance(conn, cleanup)

    with contextlib.ExitStack() as cleanup:
        test_nested(conn, cleanup)

    with contextlib.ExitStack() as cleanup:
        test_modes(conn, cleanup)

    with contextlib.ExitStack() as cleanup:
        test_dead(conn, cleanup)


if __name__ == '__main__':
    main()
