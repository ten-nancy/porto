from test_common import *
import porto
import time

conn = porto.Connection(timeout=30)

USE_CGROUP2 = GetUseCgroup2()
print("use {} hierarchy".format("cgroup2" if USE_CGROUP2 else "cgroup1"))

try:
    def CheckCgroupfsNone():
        a = conn.Run('a', cgroupfs='none', wait=0, root_volume={'layers': ['ubuntu-jammy']})

        b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False, command='cat /proc/self/cgroup')
        ExpectEq('0', b['exit_code'])
        ExpectEq(4 if USE_CGROUP2 else 8, b['stdout'].count('porto%a'))
        b.Destroy()

        b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False, command='ls /sys/fs/cgroup')
        ExpectEq('0', b['exit_code'])
        ExpectEq('', b['stdout'])
        b.Destroy()

        a.Destroy()


    def CheckCgroupfsRo():
        a = conn.Run('a', cgroupfs='ro', wait=0, root_volume={'layers': ['ubuntu-jammy']})

        b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False, command='cat /proc/self/cgroup')
        ExpectEq('0', b['exit_code'])
        ExpectEq(0, b['stdout'].count('porto%a'))
        b.Destroy()

        b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False, command='ls /sys/fs/cgroup')
        ExpectEq('0', b['exit_code'])
        ExpectEq(7 if USE_CGROUP2 else 16, len(b['stdout'].split()))
        b.Destroy()

        # check cpu cgroup symlink
        if not USE_CGROUP2:
            b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False, command='bash -c "ls /sys/fs/cgroup/cpu | wc -l"')
            ExpectEq('0', b['exit_code'])
            ExpectNe('0', b['stdout'].strip())
            b.Destroy()

        b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False,
                     command='bash -c "mkdir /sys/fs/cgroup/freezer/test && echo $$ | tee /sys/fs/cgroup/freezer/test"')
        ExpectNe('0', b['exit_code'])
        ExpectNe(-1, b['stderr'].find('Read-only file system'))
        b.Destroy()

        a.Destroy()


    def CheckCgroupfsRw(is_os, userns=False, enable_net_cgroups=False):
        if is_os:
            ExpectEq(porto.exceptions.PermissionError,
                     Catch(conn.Run, 'a', cgroupfs='rw', wait=0, root_volume={'layers': ['ubuntu-jammy']}))

        # TODO(ovov): user='1044' if userns else '0',
        a = conn.Run('a', cgroupfs='rw', userns=userns, user='0',
                     virt_mode=('os' if is_os else 'app'), wait=0, root_volume={'layers': ['ubuntu-jammy']})
        a.Wait(100)
        ExpectIn(a['state'], ('running', 'meta'))

        b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False, command='cat /proc/self/cgroup')
        ExpectEq('0', b['exit_code'])
        ExpectEq(0, b['stdout'].count('porto%a'))
        b.Destroy()

        b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False, command='ls /sys/fs/cgroup')
        ExpectEq('0', b['exit_code'])
        ExpectEq(7 if USE_CGROUP2 else 16, len(b['stdout'].split()))
        b.Destroy()

        with RunContainer(conn, 'a/b', wait=30, virt_mode='job', isolate=False, command='mkdir /sys/fs/cgroup/freezer/test') as ct:
            ExpectEq(ct['exit_status'], '0')

        with RunContainer(conn, 'a/b', wait=30, virt_mode='job', isolate=False, command="sh -c 'echo $$ > /sys/fs/cgroup/freezer/test/cgroup.procs'") as ct:
            ExpectEq(ct['exit_status'], '0')

        if not USE_CGROUP2:
            b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False,
                         command='bash -c "mkdir /sys/fs/cgroup/net_cls/test && rmdir /sys/fs/cgroup/net_cls/test"')

            if enable_net_cgroups:
                ExpectEq('0', b['exit_code'])
            else:
                ExpectNe('0', b['exit_code'])
            b.Destroy()

        a.Destroy()

    ConfigurePortod('test-cgroupns', """
    container {
        enable_rw_cgroupfs: false
    }""")

    CheckCgroupfsNone()
    CheckCgroupfsRo()
    ExpectEq(porto.exceptions.PermissionError, \
             Catch(conn.Run, 'a', cgroupfs='rw', wait=0, root_volume={'layers': ['ubuntu-jammy']}))

    ConfigurePortod('test-cgroupns', """
    container {
        enable_rw_cgroupfs: false
        use_os_mode_cgroupns : true
    }""")

    # check that container restored correctly
    a = conn.Run('a', weak=False, virt_mode='os', wait=0, root_volume={'layers': ['ubuntu-jammy']})
    ReloadPortod()
    a.Destroy()

    CheckCgroupfsNone()
    CheckCgroupfsRo()
    CheckCgroupfsRw(is_os=True)
    CheckCgroupfsRw(is_os=True, userns=True)

    ConfigurePortod('test-cgroupns', """
    container {
        enable_rw_cgroupfs: true
    }""")

    CheckCgroupfsNone()
    CheckCgroupfsRo()
    CheckCgroupfsRw(is_os=False)
    CheckCgroupfsRw(is_os=False, userns=True)

    ConfigurePortod('test-cgroupns', """
    container {
        enable_rw_cgroupfs: true
        enable_rw_net_cgroups: true
    }""")

    CheckCgroupfsRw(is_os=False, enable_net_cgroups=True)
    CheckCgroupfsRw(is_os=False, userns=True, enable_net_cgroups=True)

    if not USE_CGROUP2:
        # check link_memory_writeback_blkio
        a = conn.Run('a', wait=5, cgroupfs='ro', command='cat /sys/fs/cgroup/memory/memory.writeback_blkio')
        ExpectEq(a['stdout'].strip(), '/')
        a.Destroy()

        a = conn.Run('a', wait=5, cgroupfs='ro', command='cat /sys/fs/cgroup/memory/memory.writeback_blkio',
                     link_memory_writeback_blkio=False)
        ExpectEq(a['stdout'].strip(), '/')
        a.Destroy()

        a = conn.Run('a', wait=5, cgroupfs='ro', command='cat /sys/fs/cgroup/memory/memory.writeback_blkio',
                     link_memory_writeback_blkio=True)
        ExpectEq(a['stdout'].strip(), '/porto%a')
        a.Destroy()

    # check CgroupCleanup with cgroupfs
    ConfigurePortod('test-cgroupns', """
    container {
        enable_rw_cgroupfs: true
    }""")

    try:
        a = conn.Run('a', cgroupfs='rw', command='bash -c "sleep inf & wait $!"', weak=False)
        b = conn.Run('a/b', wait=5, virt_mode='job', isolate=False, \
                     command="bash -c 'mkdir /sys/fs/cgroup/freezer/test && echo 3 > /sys/fs/cgroup/freezer/test/cgroup.procs'")

        pids = []
        with open('/sys/fs/cgroup/freezer/porto/a/test/cgroup.procs') as f:
            pids = f.read().strip().split()
        ExpectEq(len(pids), 1)
        state = a['state']

        ReloadPortod()

        with open('/sys/fs/cgroup/freezer/porto/a/test/cgroup.procs') as f:
            ExpectEq(f.read().strip().split(), pids)
        ExpectEq(a['state'], state)

    finally:
        a.Destroy()

finally:
    ConfigurePortod('test-cgroupns', "")
