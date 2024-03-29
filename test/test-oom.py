#!/usr/bin/python

import time
import porto
from test_common import *
import json
from collections import defaultdict

MAX_ATTEMPT_COUNT = 3

def main():
    ConfigurePortod('test-oom', """
    container {
        memory_high_limit_proportion: 1
    }
    """)

    c = porto.Connection(timeout=30)
    stress_memory = "bash -c 'while true; do stress -m 1 ; done'"

    try:
        c.Destroy("test-oom")
    except:
        pass

    r = c.Find('/')

    initial_oom_count = int(r.GetProperty('oom_kills_total', sync=True))

    total_oom = initial_oom_count

    def get_oom_kills_from_cgroup(p):
        path = "/sys/fs/cgroup/memory/porto%" + str(p) + "/memory.oom_control"
        with open(path, "r") as f:
            for line in f.readlines():
                ll = line.split()
                if len(ll) > 0 and ll[0] == "oom_kill":
                    return ll[1]


    # no oom
    print("no oom")

    a = c.Run("test-oom", command="true", memory_limit="256M", wait=1)

    ExpectEq(a['state'], 'dead')
    ExpectEq(a['exit_code'], '0')
    ExpectEq(a['oom_killed'], False)
    ExpectEq(a.GetProperty('oom_kills', sync=True), '0')
    ExpectEq(a.GetProperty('oom_kills_total', sync=True), '0')
    ExpectEq(get_oom_kills_from_cgroup(a), '0')

    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))

    a.Destroy()


    # simple oom
    print("simple oom")

    a = c.Run("test-oom", command=stress_memory, memory_limit="256M", wait=5)

    ExpectEq(a['state'], 'dead')
    ExpectEq(a['exit_code'], '-99')
    ExpectEq(a['oom_killed'], True)
    ExpectEq(a.GetProperty('oom_kills', sync=True), '1')
    ExpectEq(a.GetProperty('oom_kills_total', sync=True), '1')
    ExpectEq(get_oom_kills_from_cgroup(a), '1')

    total_oom += 1
    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))

    a.Destroy()


    # restore oom event
    print("restore oom event")

    m = c.Run("test-oom", memory_limit="256M", weak=False)

    ReloadPortod()

    total_oom = initial_oom_count
    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))

    a = c.Run("test-oom/a", command=stress_memory, wait=5)

    ExpectEq(a['state'], 'dead')
    ExpectEq(a['oom_killed'], True)
    ExpectEq(a['exit_code'], '-99')

    ExpectEq(a.GetProperty('oom_kills', sync=True), '0')
    ExpectEq(a.GetProperty('oom_kills_total', sync=True), '0')

    ExpectEq(m['state'], 'dead')
    ExpectEq(m['oom_killed'], True)
    ExpectEq(m['exit_code'], '-99')

    ExpectEq(m.GetProperty('oom_kills', sync=True), '1')
    ExpectEq(m.GetProperty('oom_kills_total', sync=True), '1')
    ExpectEq(get_oom_kills_from_cgroup(m), '1')

    total_oom += 1
    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))

    m.Destroy()


    # non fatal oom
    print("non fatal oom")

    a = c.Run("test-oom", command=stress_memory, memory_limit="256M", oom_is_fatal=False)

    a.Wait(timeout_s=5)

    ExpectEq(a['state'], 'running')
    ExpectNe(a.GetProperty('oom_kills', sync=True), '0')
    ExpectNe(a.GetProperty('oom_kills', sync=True), '1')
    ExpectNe(a.GetProperty('oom_kills_total', sync=True), '0')
    ExpectNe(a.GetProperty('oom_kills_total', sync=True), '1')
    ExpectNe(get_oom_kills_from_cgroup(a), '0')
    ExpectNe(get_oom_kills_from_cgroup(a), '1')

    ExpectNe(r.GetProperty('oom_kills_total', sync=True), str(total_oom))

    a.Destroy()


    total_oom = int(r.GetProperty('oom_kills_total', sync=True))


    # os move oom
    print("os move oom")

    a = c.Run("test-oom", command=stress_memory, virt_mode="os", memory_limit="256M", wait=5)

    ExpectEq(a['state'], 'dead')
    ExpectEq(a['exit_code'], '-99')
    ExpectEq(a['oom_killed'], True)
    ExpectEq(a.GetProperty('oom_kills', sync=True), '1')
    ExpectEq(a.GetProperty('oom_kills_total', sync=True), '1')
    ExpectEq(get_oom_kills_from_cgroup(a), '1')

    total_oom += 1
    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))

    a.Destroy()


    # respawn after oom
    print("respawn after oom")

    a = c.Run("test-oom", command=stress_memory, memory_limit="256M", respawn=True, max_respawns=2, respawn_delay='2s')

    while a['state'] != 'dead':
        a.Wait()
    time.sleep(5)

    ExpectEq(a['state'], 'dead')
    ExpectEq(a['respawn_count'], '2')
    ExpectEq(a['exit_code'], '-99')
    ExpectEq(a['oom_killed'], True)
    ExpectLe(2, int(a.GetProperty('oom_kills', sync=True)))
    ExpectLe(2, int(a.GetProperty('oom_kills_total', sync=True)))
    ExpectLe(2, int(get_oom_kills_from_cgroup(a)))

    total_oom += 3
    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))

    a.Destroy()


    # oom at parent
    print("oom at parent")

    a = c.Run("test-oom", memory_limit="256M")
    b = c.Run("test-oom/b", command=stress_memory, memory_limit="512M", wait=5)

    time.sleep(1)

    ExpectEq(a['state'], 'dead')
    ExpectEq(a['exit_code'], '-99')
    ExpectEq(a['oom_killed'], True)

    ExpectEq(b['state'], 'dead')
    ExpectEq(b['exit_code'], '-99')
    ExpectEq(b['oom_killed'], True)

    ExpectEq(a.GetProperty('oom_kills_total', sync=True), '1')
    ExpectEq(b.GetProperty('oom_kills_total', sync=True), '1')

    # Race: Speculative OOM could be detected in a or test-oom/b
    #       (kernel stuff)
    print("speculative oom")

    deadline = time.time() + 30
    oom_speculative = 0

    while time.time() < deadline and oom_speculative == 0:
        oom_speculative = int(a.GetProperty('oom_kills', sync=True)) + int(b.GetProperty('oom_kills', sync=True))
        time.sleep(1)

    ExpectLe(1, oom_speculative)
    ExpectLe(1, int(a.GetProperty('oom_kills_total', sync=True)))

    total_oom += 1
    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))

    b.Destroy()
    a.Destroy()


    # oom at child
    print("oom at child")

    a = c.Run("test-oom", memory_limit="512M")
    b = c.Run("test-oom/b", command=stress_memory, memory_limit="256M", wait=5)

    ExpectEq(a['state'], 'meta')
    ExpectEq(a.GetProperty('oom_kills', sync=True), '0')
    ExpectEq(a.GetProperty('oom_kills_total', sync=True), '1')
    ExpectEq(get_oom_kills_from_cgroup(a), '0')

    ExpectEq(b['state'], 'dead')
    ExpectEq(b['exit_code'], '-99')
    ExpectEq(b['oom_killed'], True)
    ExpectEq(b.GetProperty('oom_kills', sync=True), '1')
    ExpectEq(b.GetProperty('oom_kills_total', sync=True), '1')
    ExpectEq(get_oom_kills_from_cgroup(b), '1')

    total_oom += 1
    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))


    # second oom after restart
    print("second oom after restart")

    b.Stop()
    b.Start()
    b.WaitContainer(5)

    ExpectEq(a['state'], 'meta')
    ExpectEq(a.GetProperty('oom_kills', sync=True), '0')
    ExpectEq(a.GetProperty('oom_kills_total', sync=True), '2')
    ExpectEq(get_oom_kills_from_cgroup(a), '0')

    ExpectEq(b['state'], 'dead')
    ExpectEq(b['exit_code'], '-99')
    ExpectEq(b['oom_killed'], True)
    ExpectEq(b.GetProperty('oom_kills', sync=True), '1')
    ExpectEq(b.GetProperty('oom_kills_total', sync=True), '2')
    ExpectEq(get_oom_kills_from_cgroup(b), '1')

    total_oom += 1
    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))


    # third oom at child after recreate
    print("third oom at child after recreate")

    b.Destroy()
    b = c.Run("test-oom/b", command=stress_memory, memory_limit="256M", wait=5)

    ExpectEq(a['state'], 'meta')
    ExpectEq(a.GetProperty('oom_kills', sync=True), '0')
    ExpectEq(a.GetProperty('oom_kills_total', sync=True), '3')
    ExpectEq(get_oom_kills_from_cgroup(a), '0')

    ExpectEq(b['state'], 'dead')
    ExpectEq(b['exit_code'], '-99')
    ExpectEq(b['oom_killed'], True)
    ExpectEq(b.GetProperty('oom_kills', sync=True), '1')
    ExpectEq(b.GetProperty('oom_kills_total', sync=True), '1')
    ExpectEq(get_oom_kills_from_cgroup(b), '1')

    total_oom += 1
    ExpectEq(r.GetProperty('oom_kills_total', sync=True), str(total_oom))


    b.Destroy()
    a.Destroy()


    # counting at subling
    print("counting at subling")

    N = 20
    stats = defaultdict(int)
    try:
        for i in range(N):
            slot = c.Run("slot", wait=5, memory_limit="256M", oom_is_fatal=False, weak=True)
            meta = c.Run("slot/meta")
            yes = c.Run("slot/meta/yes", command="yes", memory_limit="256M", oom_is_fatal=False, weak=True)
            stress = c.Run("slot/meta/stress", command="stress -m 1", memory_limit="512M", wait=5, oom_is_fatal=False, weak=True)

            stats[" ".join([slot['oom_kills'], slot['oom_kills_total'], yes['oom_kills'], yes['oom_kills_total'], stress['oom_kills'], stress['oom_kills_total']])] += 1

            slot.Destroy()

    except Exception as exc:
        raise exc

    finally:
        print(json.dumps(stats, sort_keys=True, indent=4))
        ExpectEq(list(stats.items()), [("0 1 0 0 1 1", N)])


if __name__=='__main__':
    version = GetKernelVersion()
    if version >= (5, 4):
        excs = []

        for i in range(MAX_ATTEMPT_COUNT):
            try:
                main()
                break

            except Exception as exc:
                print(exc)
                excs.append(exc)

        if len(excs) == MAX_ATTEMPT_COUNT:
            raise Exception(excs)

    else:
        print("skip test on {}".format(".".join(map(lambda x: str(x), version))))
