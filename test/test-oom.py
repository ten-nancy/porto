#!/usr/bin/python

import time
import porto
from test_common import *
import json
from collections import defaultdict


def wait_oom_kill(ct, oom_kills):
    ct.Wait(timeout_s=30)
    ExpectEq(ct['state'], 'dead')
    ExpectEq(int(ct['oom_kills']), oom_kills)
    ExpectEq(ct['exit_code'], '1')


def main():
    USE_CGROUP2 = GetUseCgroup2()
    print("use {} hierarchy".format("cgroup2" if USE_CGROUP2 else "cgroup1"))

    ConfigurePortod('test-oom', """
    container {
        memory_high_limit_proportion: 1
    }
    """)

    c = porto.Connection(timeout=30)
    def run(*args, oom_is_fatal=False, **kwargs):
        return c.Run(*args, oom_is_fatal=oom_is_fatal, **kwargs)

    stress_memory = "stress -m 1 --vm-bytes 402653184 --vm-hang 0" # 384MiB
    stress_memory_loop = "bash -c 'while true ; do {} ; done'".format(stress_memory)

    try:
        c.Destroy("test-oom")
    except:
        pass

    r = c.Find('/')

    initial_oom_count = int(r.GetProperty('oom_kills_total', sync=True))

    total_oom = initial_oom_count

    def get_oom_kills_from_cgroup(p, leaf=False):
        if USE_CGROUP2:
            if leaf:
                path = "/sys/fs/cgroup/porto/" + str(p) + "/leaf/memory.events"  # oom_kills_total
            else:
                path = "/sys/fs/cgroup/porto/" + str(p) + "/memory.events"  # oom_kills_total
        else:
            path = "/sys/fs/cgroup/memory/porto%" + str(p) + "/memory.oom_control"  # oom_kills
        with open(path, "r") as f:
            for line in f.readlines():
                ll = line.split()
                if len(ll) > 0 and ll[0] == "oom_kill":
                    return ll[1]


    # no oom
    print("no oom")

    a = run("test-oom", command="true", memory_limit="256M", cpu_limit="1c", wait=1)

    ExpectEq(a['state'], 'dead')
    ExpectEq(a['exit_code'], '0')
    ExpectEq(a['oom_killed'], False)
    ExpectEq(a.GetProperty('oom_kills', sync=True), '0')
    ExpectEq(a.GetProperty('oom_kills_total', sync=True), '0')
    ExpectEq(get_oom_kills_from_cgroup(a), '0')
    if USE_CGROUP2:
        ExpectEq(get_oom_kills_from_cgroup(a, True), '0')

    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))

    a.Destroy()

    # simple oom
    print("simple oom")

    a = run("test-oom", command=stress_memory, memory_limit="16M", cpu_limit="1c")
    wait_oom_kill(a, 1)

    ExpectEq(a.GetProperty('oom_kills_total', sync=True), '1')
    ExpectEq(get_oom_kills_from_cgroup(a), '1')
    if USE_CGROUP2:
        ExpectEq(get_oom_kills_from_cgroup(a, True), '1')

    total_oom += 1
    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))

    a.Destroy()


    # restore oom event
    print("restore oom event")

    m = run("test-oom", memory_limit="16M", cpu_limit="1c", weak=False)

    ReloadPortod()

    if not USE_CGROUP2:
        # porto drops counters using cgroup1
        total_oom = initial_oom_count

    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))

    a = run("test-oom/a", command=stress_memory, memory_limit="512M")
    wait_oom_kill(a, 1)

    ExpectEq(a.GetProperty('oom_kills_total', sync=True), '1')

    m.Wait(timeout=5000)
    ExpectEq(m['state'], 'meta')
    ExpectEq(m.GetProperty('oom_kills', sync=True), '0')
    ExpectEq(m.GetProperty('oom_kills_total', sync=True), '1')

    total_oom += 1
    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))

    m.Destroy()

    # non fatal oom
    print("non fatal oom")

    a = run("test-oom", command=stress_memory_loop, memory_limit="16M", cpu_limit="1c")

    time.sleep(3)
    ExpectEq(a['state'], 'running')
    ExpectLe(1, int(a['oom_kills']))
    ExpectNe(a.GetProperty('oom_kills_total', sync=True), str(total_oom))

    a.Destroy()

    total_oom = int(r.GetProperty('oom_kills_total', sync=True))

    # test container kill in case of oom_is_fatal=True (counting oom_kills in this case is racy, so dont check it)
    print('test oom_is_fatal=True container kill')
    m = run("foo", memory_limit="16M", cpu_limit="1c", oom_is_fatal=True)
    a = run("foo/bar", command=stress_memory_loop, memory_limit="512M", cpu_limit="1c", oom_is_fatal=True)
    a.Wait(timeout_s=30)

    ExpectEq(a['state'], 'dead')
    ExpectEq(a['oom_killed'], True)
    ExpectEq(m['state'], 'dead')
    ExpectEq(m['oom_killed'], True)

    total_oom = int(r.GetProperty('oom_kills_total', sync=True))

    # os move oom
    print("os move oom")

    a = run("test-oom", command=stress_memory, virt_mode="os", memory_limit="16M", cpu_limit="1c")
    wait_oom_kill(a, 1)

    ExpectEq(a.GetProperty('oom_kills_total', sync=True), '1')
    ExpectEq(get_oom_kills_from_cgroup(a), '1')
    if USE_CGROUP2:
        ExpectEq(get_oom_kills_from_cgroup(a, True), '1')

    total_oom += 1
    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))

    a.Destroy()


    # respawn after oom
    print("respawn after oom")

    a = run("test-oom", command=stress_memory, memory_limit="16M", cpu_limit="1c", respawn=True, max_respawns=2, respawn_delay='2s')
    while a['state'] != 'dead':
        a.Wait()

    wait_oom_kill(a, 3)

    ExpectEq(a['respawn_count'], '2')
    ExpectEq(int(a.GetProperty('oom_kills_total', sync=True)), 3)
    ExpectEq(int(get_oom_kills_from_cgroup(a)), 3)
    if USE_CGROUP2:
        ExpectEq(int(get_oom_kills_from_cgroup(a, True)), 3)

    total_oom += 3
    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))

    a.Destroy()


    # oom at parent
    print("oom at parent")

    a = run("test-oom", memory_limit="16M", cpu_limit="1c")
    b = run("test-oom/b", command=stress_memory, memory_limit="512M", cpu_limit="1c")
    wait_oom_kill(b, 1)

    ExpectEq(a.GetProperty('oom_kills_total', sync=True), '1')
    if USE_CGROUP2:
        ExpectEq(a.GetProperty('oom_kills', sync=True), '0')
        ExpectEq(get_oom_kills_from_cgroup(a), '1')
        ExpectEq(get_oom_kills_from_cgroup(a, True), '0')
    else:
        ExpectEq(a.GetProperty('oom_kills', sync=True), '0')
        ExpectEq(get_oom_kills_from_cgroup(a), '0')

    ExpectEq(b.GetProperty('oom_kills_total', sync=True), '1')
    ExpectEq(get_oom_kills_from_cgroup(b), '1')
    if USE_CGROUP2:
        ExpectEq(get_oom_kills_from_cgroup(b, True), '1')

    b.Destroy()
    a.Destroy()

    total_oom += 1
    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))

    # oom at child
    print("oom at child")

    a = run("test-oom", memory_limit="512M", cpu_limit="1c")
    b = run("test-oom/b", command=stress_memory, memory_limit="16M", cpu_limit="1c")
    wait_oom_kill(b, 1)

    ExpectEq(b.GetProperty('oom_kills_total', sync=True), '1')
    ExpectEq(get_oom_kills_from_cgroup(b), '1')
    if USE_CGROUP2:
        ExpectEq(get_oom_kills_from_cgroup(b, True), '1')

    ExpectEq(a['state'], 'meta')
    ExpectEq(a.GetProperty('oom_kills_total', sync=True), '1')
    if USE_CGROUP2:
        ExpectEq(a.GetProperty('oom_kills', sync=True), '1')
        ExpectEq(get_oom_kills_from_cgroup(a, True), '0')
        ExpectEq(get_oom_kills_from_cgroup(a), '1')
    else:
        ExpectEq(a.GetProperty('oom_kills', sync=True), '0')
        ExpectEq(get_oom_kills_from_cgroup(a), '0')


    total_oom += 1
    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))


    # second oom after restart
    print("second oom after restart")

    b.Stop()
    b.Start()
    wait_oom_kill(b, 1)

    ExpectEq(get_oom_kills_from_cgroup(b), '1')
    if USE_CGROUP2:
        # b cgroup was recreated, so counter was dropped
        ExpectEq(b.GetProperty('oom_kills_total', sync=True), '1')
        ExpectEq(get_oom_kills_from_cgroup(b, True), '1')
    else:
        ExpectEq(b.GetProperty('oom_kills_total', sync=True), '2')

    ExpectEq(a['state'], 'meta')
    ExpectEq(a.GetProperty('oom_kills_total', sync=True), '2')
    if USE_CGROUP2:
        ExpectEq(a.GetProperty('oom_kills', sync=True), '2')
        ExpectEq(get_oom_kills_from_cgroup(a, True), '0')
        ExpectEq(get_oom_kills_from_cgroup(a), '2')
    else:
        ExpectEq(a.GetProperty('oom_kills', sync=True), '0')
        ExpectEq(get_oom_kills_from_cgroup(a), '0')


    total_oom += 1
    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))


    # third oom at child after recreate
    print("third oom at child after recreate")

    b.Destroy()
    b = run("test-oom/b", command=stress_memory, memory_limit="16M", cpu_limit="1c")
    wait_oom_kill(b, 1)

    ExpectEq(b.GetProperty('oom_kills_total', sync=True), '1')
    ExpectEq(get_oom_kills_from_cgroup(b), '1')
    if USE_CGROUP2:
        ExpectEq(get_oom_kills_from_cgroup(b, True), '1')

    ExpectEq(a['state'], 'meta')
    ExpectEq(a.GetProperty('oom_kills_total', sync=True), '3')
    if USE_CGROUP2:
        ExpectEq(a.GetProperty('oom_kills', sync=True), '3')
        ExpectEq(get_oom_kills_from_cgroup(a, True), '0')
        ExpectEq(get_oom_kills_from_cgroup(a), '3')
    else:
        ExpectEq(a.GetProperty('oom_kills', sync=True), '0')
        ExpectEq(get_oom_kills_from_cgroup(a), '0')


    total_oom += 1
    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))

    b.Destroy()
    a.Destroy()


    # counting at subling
    print("counting at subling")

    N = 20
    stats = defaultdict(int)
    for i in range(N):
        slot = run("slot", memory_limit="16M", memory_guarantee="16M", cpu_limit="1c")
        yes = run("slot/yes", command="tail -f /dev/null", memory_limit="512M", memory_guarantee="16M", cpu_limit="1c")
        stress = run("slot/stress", command=stress_memory, memory_limit="512M", cpu_limit="1c")
        wait_oom_kill(stress, 1)

        stats[" ".join([slot['oom_kills'], slot['oom_kills_total'], yes['oom_kills'], yes['oom_kills_total'], stress['oom_kills'], stress['oom_kills_total']])] += 1

        slot.Destroy()

    print(json.dumps(stats, sort_keys=True, indent=4))
    if USE_CGROUP2:
        ExpectEq(list(stats.items()), [("1 1 0 0 1 1", N)])
    else:
        ExpectEq(list(stats.items()), [("0 1 0 0 1 1", N)])


if __name__=='__main__':
    version = GetKernelVersion()
    if version >= (5, 4):
        main()
    else:
        print("skip test on {}".format(".".join(map(lambda x: str(x), version))))
