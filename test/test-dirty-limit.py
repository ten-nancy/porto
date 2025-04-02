import os
import porto
from test_common import *

conn = porto.Connection(timeout=30)
ct = conn.CreateWeakContainer("test-dirty-limit")
vol = conn.CreateVolume(private="test-dirty-limit", containers=ct.name)

ct.SetProperty("cwd", vol.path)
ct.SetProperty("command", "dd if=/dev/zero of=test conv=notrunc bs=1M count=100")
ct.SetProperty("dirty_limit", "10M")

if GetKernelVersion() > (3, 18):
    dirty = "dirty"
else:
    dirty = "fs_dirty"

def Run():
    ct.Start()
    ct.Wait(60000)
    ExpectProp(ct, "state", "dead")
    ExpectProp(ct, "exit_code", "0")
    ExpectMemoryStatLe(ct, dirty, 10 << 20)
    ExpectEq(os.path.getsize(vol.path + '/test'), 100 << 20)
    ct.Stop()

print('- write')
Run()

print('- rewrite')
Run()

ct.SetProperty("memory_limit", "10M")

print('- hit limit')
Run()

ct.Destroy()


# test memory dirty limit bound

def check_dirty_limit_in_cgroup(ct, value):
    path = "{}/memory.dirty_limit_in_bytes".format(ct["cgroups[memory]"])
    with open(path, "r") as f:
        ExpectEq(f.read(), value + "\n")


_10Mb = "10485760"
_5Mb = "5242880"
_1Mb = "1048576"
_NIL = "0"

a = conn.Run("a", dirty_limit="10M", weak=True)
ExpectProp(a, 'dirty_limit', _10Mb)
ExpectProp(a, 'dirty_limit_bound', _10Mb)
check_dirty_limit_in_cgroup(a, _10Mb)

# b has no memory controllers
b = conn.Run("a/b", weak=True)
ExpectProp(b, 'dirty_limit', _NIL)
ExpectProp(b, 'dirty_limit_bound', _10Mb)
check_dirty_limit_in_cgroup(b, _10Mb)

c = conn.Run("a/b/c", dirty_limit="5M", weak=True)
ExpectProp(c, 'dirty_limit', _5Mb)
ExpectProp(c, 'dirty_limit_bound', _5Mb)
check_dirty_limit_in_cgroup(c, _5Mb)

d = conn.Run("a/b/c/d", dirty_limit="10M", weak=True)
ExpectProp(d, 'dirty_limit', _10Mb)
ExpectProp(d, 'dirty_limit_bound', _5Mb)
check_dirty_limit_in_cgroup(d, _5Mb)

# disable dirty_limit at c
c.SetProperty('dirty_limit', _NIL)

ExpectProp(c, 'dirty_limit', _NIL)
ExpectProp(c, 'dirty_limit_bound', _10Mb)
check_dirty_limit_in_cgroup(c, _NIL)

ExpectProp(d, 'dirty_limit', _10Mb)
ExpectProp(d, 'dirty_limit_bound', _10Mb)
check_dirty_limit_in_cgroup(d, _10Mb)

# enable the smallest dirty_limit at a
a.SetProperty('dirty_limit', _1Mb)

ExpectProp(a, 'dirty_limit', _1Mb)
ExpectProp(a, 'dirty_limit_bound', _1Mb)
check_dirty_limit_in_cgroup(a, _1Mb)

ExpectProp(b, 'dirty_limit', _NIL)
ExpectProp(b, 'dirty_limit_bound', _1Mb)
check_dirty_limit_in_cgroup(b, _1Mb)

ExpectProp(c, 'dirty_limit', _NIL)
ExpectProp(c, 'dirty_limit_bound', _1Mb)
check_dirty_limit_in_cgroup(c, _NIL)

ExpectProp(d, 'dirty_limit', _10Mb)
ExpectProp(d, 'dirty_limit_bound', _1Mb)
check_dirty_limit_in_cgroup(d, _1Mb)

# disable dirty_limit at a
a.SetProperty('dirty_limit', _NIL)

ExpectProp(a, 'dirty_limit', _NIL)
ExpectProp(a, 'dirty_limit_bound', "")
check_dirty_limit_in_cgroup(a, _NIL)

ExpectProp(b, 'dirty_limit', _NIL)
ExpectProp(b, 'dirty_limit_bound', "")
check_dirty_limit_in_cgroup(b, _NIL)

ExpectProp(c, 'dirty_limit', _NIL)
ExpectProp(c, 'dirty_limit_bound', "")
check_dirty_limit_in_cgroup(c, _NIL)

ExpectProp(d, 'dirty_limit', _10Mb)
ExpectProp(d, 'dirty_limit_bound', _10Mb)
check_dirty_limit_in_cgroup(d, _10Mb)

a.Destroy()
