#!/usr/bin/python3

import os
import porto
import subprocess
import time
from test_common import *

c = porto.Connection(timeout=10)

portod_cg = GetSystemdCg(GetPortodPid())
if WithSystemd():
    ExpectNe(portod_cg, "/")
else:
    ExpectEq(portod_cg, "/")

a = c.Run("a", virt_mode='os', root_volume={'layers': ["ubuntu-jammy"]}, **{"controllers[systemd]": True})
a_pid = a['root_pid']

b = c.Run("b", command="tail -f /dev/null")
b_pid = b['root_pid']

a_cg = GetSystemdCg(a_pid)
assert a_cg.startswith("/porto%a"), "{} must start with /porto%a".format(a_cg)

for i in range(10):
    if i:
        time.sleep(1)
    a_cg = GetSystemdCg(a_pid)
    if a_cg == "/porto%a/init.scope":
        break
ExpectEq(a_cg, "/porto%a/init.scope")


b_cg = GetSystemdCg(b_pid)
ExpectEq(b_cg, portod_cg)

mnt = ParseMountinfo(a_pid)

Expect("/sys/fs/cgroup" in mnt)
Expect('ro' in mnt["/sys/fs/cgroup"]['flag'])

Expect("/sys/fs/cgroup/systemd" in mnt)
Expect('ro' in mnt["/sys/fs/cgroup/systemd"]['flag'])

Expect("/sys/fs/cgroup/systemd/porto%a" in mnt)
Expect('rw' in mnt["/sys/fs/cgroup/systemd/porto%a"]['flag'])

if WithSystemd():
    subprocess.check_call(["systemctl", "daemon-reexec"])
    ExpectEq(GetSystemdCg(a_pid), a_cg)
    ExpectEq(GetSystemdCg(b_pid), b_cg)

a.Destroy()
b.Destroy()
