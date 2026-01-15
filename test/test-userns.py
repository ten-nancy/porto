import porto
from test_common import *

conn = porto.Connection(timeout=30)

v = conn.CreateVolume(layers=['ubuntu-noble'])

def CheckUserNs(userns=True, virt_mode='app'):
    os_mode = virt_mode == 'os'
    devices = '/dev/fuse rw' if virt_mode != 'job' else ''

    a = conn.Run('a', userns=userns, user='1044', wait=0, net='L3 veth', devices=devices)
    b = conn.Run('a/b', user='1044', root=v.path)

    # check NET_ADMIN
    # TODO(ovov): without explicit  net='inherited' net becomes none and this does not work
    c = conn.Run('a/b/c', wait=5, unshare_on_exec=userns, user='1044', devices=devices, net='inherited',
                 virt_mode=virt_mode, command="bash -c 'ip netns add test && ip netns list'")
    (ExpectEq if userns else ExpectNe)(c['exit_code'], '0')
    if not os_mode:
        ExpectEq('test' if userns else '', c['stdout'].strip())
    c.Destroy()

    # check SYS_ADMIN
    c = conn.Run('a/b/c', wait=5, unshare_on_exec=userns, user='1044', devices=devices, virt_mode=virt_mode, command="bash -c 'mount -t tmpfs tmpfs /tmp && df /tmp | grep -o tmpfs'")
    (ExpectEq if userns else ExpectNe)(c['exit_code'], '0')
    if not os_mode:
        ExpectEq('tmpfs' if userns else '', c['stdout'].strip())
    c.Destroy()

    # check devices ownership
    if devices:
        c = conn.Run('a/b/c', wait=5, unshare_on_exec=userns, user='1044', devices=devices, virt_mode=virt_mode, command="stat -c '%U %G' /dev/fuse")
        ExpectEq('0', c['exit_code'])
        if not os_mode:
            ExpectEq('root root', c['stdout'].strip())
        c.Destroy()

    b.Destroy()
    a.Destroy()

CheckUserNs(userns=False)
CheckUserNs(virt_mode='app')
CheckUserNs(virt_mode='job')
CheckUserNs(virt_mode='os')

conn.UnlinkVolume(v.path)

a = conn.Run('a', userns=True)
try:
    b = conn.Run('a/b', command='bash -c "echo lala > /proc/sys/kernel/core_pattern"', wait=10)
    ExpectNe(b['exit_code'], '0')
    ExpectEq(b['stderr'].count('Read-only file system'), 1)
finally:
    a.Destroy()

