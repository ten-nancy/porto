import porto
from test_common import *
import os
import time
import shutil

conn = porto.Connection(timeout=30)

def ExpectRunlevel(ct, level):
    r = conn.Run(ct.name + '/runlevel', wait=10, command='bash -c \'for i in `seq 50` ; do [ "`runlevel`" = "{}" ] && break ; sleep 0.1 ; done; runlevel\''.format(level))
    ExpectEq(r['stdout'].strip(), level)
    ExpectEq(r['exit_code'], '0')
    r.Destroy()


def CheckSubCgroups(ct):
    warnings = conn.GetProperty("/", "porto_stat[warnings]")
    r = conn.Run(ct.name + '/child', wait=10,
                 command='''bash -c "mkdir -p /sys/fs/cgroup/freezer/dir123/subdir567;
                            echo $$ >/sys/fs/cgroup/freezer/dir123/subdir567/cgroup.procs;
                            chmod +x /portoctl; /portoctl list"''',
                            private='portoctl shell', isolate=False)
    ExpectEq(len(r['stderr']), 0)
    r.Destroy()
    ExpectEq(conn.GetProperty("/", "porto_stat[warnings]"), warnings)

def CheckCgroupHierarchy(ct, haveCgroups):
    # Check cgroup hierarchy in portoctl shell container
    r = conn.Run(ct.name + '/child', wait=10, command='ls /sys/fs/cgroup', private='portoctl shell', isolate=False)
    res_cgroups = r['stdout'].strip().split('\n')
    if haveCgroups:
        assert len(res_cgroups) == 16
    else:
        res_cgroups == ['systemd']
    r.Destroy()

    # Check cgroup hierarchy in child container
    r = conn.Run(ct.name + '/child', wait=10, command='ls /sys/fs/cgroup')
    res_cgroups = r['stdout'].strip().split('\n')
    # porto mounts new sysfs, so there are no cgroups
    assert len(res_cgroups) == 1
    r.Destroy()

    if haveCgroups:
        # Create subcgroup in portoctl shell containers
        r = conn.Run(ct.name + '/child', wait=10, command='mkdir /sys/fs/cgroup/freezer/cgroup123', private='portoctl shell', isolate=False)
        ExpectEq(r['exit_code'], '0')
        assert 'cgroup123' in  os.listdir("/sys/fs/cgroup/freezer/porto/" + ct.name)
        r.Destroy()

        # Create subcgroup in child containers
        r = conn.Run(ct.name + '/child', wait=10, command='ls /sys/fs/cgroup/freezer', private='portoctl shell', isolate=False)
        ExpectEq(r['exit_code'], '0')
        ExpectNe(len(r['stdout']), 0)
        r.Destroy()

        unmounted_cgroups = ['net_cls', 'net_prio', 'net_cls,net_prio']

        for unmounted_cgroup in unmounted_cgroups:
            r = conn.Run(ct.name + '/child', wait=10, command='ls /sys/fs/cgroup/{}'.format(unmounted_cgroup), private='portoctl shell', isolate=False)
            ExpectEq(r['exit_code'], '0')
            if not haveCgroups:
                ExpectEq(len(r['stdout']), 0)
            else:
                ExpectNe(len(r['stdout']), 0)
            r.Destroy()


def CheckSystemd(ct):
    for i in range(5):
        with RunContainer(conn, os.path.join(ct.name, 'child'), command='systemctl -a',
                          wait=10, private='portoctl shell', virt_mode='job') as r:
            if int(r['exit_code']) == 0:
                ExpectNe(r['stdout'].strip().splitlines(), [])
                ExpectEq(r['stderr'], '')
                return
        if i < 4:
            time.sleep(0.1)

    raise AssertionError("systemd is not ready")


try:
    ConfigurePortod('test-os', """
    container {
        use_os_mode_cgroupns : true
    }""")

    a = conn.Run("a", virt_mode='os', root_volume={'layers': ["ubuntu-jammy"]})
    ExpectRunlevel(a, 'N 5')
    a.Stop()
    a.Start()
    ExpectRunlevel(a, 'N 5')
    shutil.copyfile(portoctl, "{}/portoctl".format(a.GetData('root')))
    CheckSubCgroups(a)
    CheckSystemd(a)
    CheckCgroupHierarchy(a, True)
    a.Destroy()

    m = conn.Run("m", root_volume={'layers': ["ubuntu-jammy"]})
    a = conn.Create("m/a")
    a.SetProperty('virt_mode', 'os')
    a.SetProperty('capabilities[SYS_ADMIN]', 'false')
    a.Start()
    CheckCgroupHierarchy(a, True)
    CheckSystemd(a)
    m.Destroy()

    ConfigurePortod('test-os', """
    container {
        use_os_mode_cgroupns : false
    }""")

    m = conn.Run("m", root_volume={'layers': ["ubuntu-jammy"]})
    a = conn.Run("m/a", virt_mode='os')
    ExpectRunlevel(a, 'N 5')
    a.Stop()
    a.Start()
    CheckCgroupHierarchy(a, False)
    CheckSystemd(a)
    ExpectRunlevel(a, 'N 5')
    m.Destroy()

    m = conn.Run("m", root_volume={'layers': ["ubuntu-jammy"]})
    a = conn.Create("m/a")
    a.SetProperty('virt_mode', 'os')
    a.SetProperty('capabilities[SYS_ADMIN]', 'false')
    a.Start()
    CheckCgroupHierarchy(a, False)
    CheckSystemd(a)
    m.Destroy()

finally:
    ConfigurePortod('test-os', "")
