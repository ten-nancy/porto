#!/usr/bin/python3

import porto
from test_common import *

import contextlib
import functools
from tempfile import TemporaryDirectory
import random
import string
import stat
import os
from uuid import uuid4

conn = porto.Connection(timeout=30)

@contextlib.contextmanager
def runc(conn, *args, devices_explicit=True, **kwargs):
    ct = conn.Run(*args, devices_explicit=devices_explicit, **kwargs)
    try:
        yield ct
    finally:
        try:
            ct.Destroy()
        except porto.exceptions.ContainerDoesNotExist:
            pass


def ExpectDevice(ct, dev, access):
    ipath = "/proc/{}/root{}".format(ct['root_pid'], dev.path)
    devcg = GetCgroup(ct, "devices")

    if access == "-":
        ExpectNotExists(ipath)
        mode = 'b' if stat.S_ISBLK(dev.stat.st_rdev) else 'c'

        cmd = "echo $$ > {}/cgroup.procs && mknod {} {} {} {}".format(devcg, ipath, mode, os.major(dev.stat.st_rdev), os.minor(dev.stat.st_rdev))
        ExpectNe(subprocess.call(cmd, shell=True), 0)
    else:
        ExpectFile(ipath, mode=dev.stat.st_mode, dev=dev.stat.st_rdev)

        can_read = not subprocess.call("echo $$ > {}/cgroup.procs && test -r {}".format(devcg, ipath), shell=True)
        can_write = not subprocess.call("echo $$ > {}/cgroup.procs && test -w {}".format(devcg, ipath), shell=True)

        ExpectEq(can_read, 'r' in access)
        ExpectEq(can_write, 'w' in access)


class Device:
    def __init__(self, path):
        self.path = path
        self.stat = os.stat(path)


TTY0 = Device('/dev/tty0')
TTY1 = Device('/dev/tty1')
TTY2 = Device('/dev/tty2')
FUSE= Device('/dev/fuse')


def test_nesting(cleanup, tmpdir):
    os.makedirs(os.path.join(tmpdir, "foo/cheburek"))
    os.makedirs(os.path.join(tmpdir, "foo/bar/baz"))
    os.makedirs(os.path.join(tmpdir, "foo/bar/qux"))
    os.makedirs(os.path.join(tmpdir, "foo/bar/kek"))

    tty0 = os.stat('/dev/tty0')
    tty1 = os.stat('/dev/tty1')

    foo = cleanup.enter_context(runc(conn, "foo", root=os.path.join(tmpdir, "foo"), devices="/dev/tty0 rw"))
    ExpectDevice(foo, TTY0, 'rw')

    cheburek = cleanup.enter_context(runc(conn, "foo/cheburek", root="/cheburek", start=False, devices="/dev/tty0 rw"))

    bar = cleanup.enter_context(runc(conn, "foo/bar", root='/bar'))
    ExpectDevice(bar, TTY0, "rw")

    foo.SetProperty("devices", "/dev/tty0 rw; /dev/tty1 rw")
    ExpectDevice(foo, TTY1, "rw")
    ExpectDevice(bar, TTY1, "rw")

    baz = cleanup.enter_context(runc(conn, 'foo/bar/baz', root='/baz'))
    ExpectDevice(baz, TTY1, "rw")
    ExpectDevice(baz, TTY0, "rw")

    ExpectException(baz.SetProperty, porto.exceptions.Permission, "devices", "/dev/tty2 rw")

    foo.SetProperty("devices[/dev/tty0]", "-")
    for ct in [foo, bar, baz]:
        ExpectDevice(ct, TTY0, "-")

    qux = cleanup.enter_context(runc(conn, 'foo/bar/qux', root='/qux'))
    ExpectDevice(qux, TTY1, "rw")

    bar.SetProperty("devices[/dev/tty1]", "-")
    for ct in [bar, baz, qux]:
        ExpectDevice(ct, TTY1, "-")

    kek = cleanup.enter_context(runc(conn, 'foo/bar/kek', root='/kek'))

    ExpectNotExists("/proc/{}/root/dev/tty0".format(kek['root_pid']))
    ExpectNotExists("/proc/{}/root/dev/tty1".format(kek['root_pid']))

    foo.SetProperty("devices[/dev/tty0]", "rw")
    ExpectDevice(foo, TTY0, "rw")
    for ct in [bar, baz, qux, kek]:
        ExpectDevice(ct, TTY0, "-")

    ExpectException(baz.SetProperty, porto.exceptions.Permission, "devices", "/dev/tty2 rw")
    ExpectException(baz.SetProperty, porto.exceptions.Permission, "devices", "/dev/tty1 rw")
    ExpectException(baz.SetProperty, porto.exceptions.Permission, "devices", "/dev/tty0 rw")

    bar.SetProperty("devices[/dev/tty1]", "rw")
    for ct in [bar, baz, qux, kek]:
        ExpectDevice(ct, TTY1, "rw")



def test_nesting_fuse(cleanup, tmpdir):
    os.makedirs(os.path.join(tmpdir, "foo/bar/baz"))
    os.makedirs(os.path.join(tmpdir, "foo/bar/qux"))
    os.makedirs(os.path.join(tmpdir, "foo/bar/kek"))

    foo = cleanup.enter_context(runc(conn, "foo", root=os.path.join(tmpdir, "foo"), devices="/dev/tty0 rw; /dev/fuse rw"))
    ExpectDevice(foo, TTY0, "rw")
    ExpectDevice(foo, FUSE, "rw")

    bar = cleanup.enter_context(runc(conn, "foo/bar", root="/bar", enable_fuse="true"))
    ExpectDevice(bar, TTY0, "rw")
    ExpectDevice(bar, FUSE, "rw")

    baz = cleanup.enter_context(runc(conn, "foo/bar/baz", root="/baz", devices="/dev/tty0 rw"))
    ExpectDevice(baz, TTY0, "rw")
    ExpectDevice(baz, FUSE, "-")

    # TODO(ovov): qux = cleanup.enter_context(runc(conn, "foo/bar/qux", root="/qux", enable_fuse="true"))
    qux = cleanup.enter_context(runc(conn, "foo/bar/qux", root="/qux"))
    ExpectDevice(qux, TTY0, "rw")
    ExpectDevice(qux, FUSE, "rw")

    foo.SetProperty("devices[/dev/tty1]", "rw")
    for ct in [bar, qux]:
        ExpectDevice(ct, FUSE, "rw")
    ExpectDevice(baz, FUSE, "-")

    kek = cleanup.enter_context(runc(conn, "foo/bar/kek", root="/kek"))
    ExpectDevice(kek, TTY0, "rw")
    ExpectDevice(kek, TTY1, "rw")


@contextlib.contextmanager
def mktmpnod(path, *args, **kwargs):
    os.mknod(path, *args, **kwargs)
    try:
        yield path
    finally:
        try:
            os.remove(path)
        except OSError:
            pass


def test_removed_dev(cleanup, tmpdir):
    tmpnod = "/dev/{}".format(''.join(random.choice(string.ascii_uppercase) for _ in range(8)))
    cleanup.enter_context(mktmpnod(tmpnod, mode=0o600|stat.S_IFCHR))
    os.makedirs(os.path.join(tmpdir, "foo/bar"))

    foo = cleanup.enter_context(runc(conn, "foo", root=os.path.join(tmpdir, "foo"), devices="{} rw".format(tmpnod)))
    foo.SetProperty("devices[{}]".format(tmpnod), '-')
    os.remove(tmpnod)
    bar = cleanup.enter_context(runc(conn, "foo/bar", root="/bar"))



def test_something(cleanup, tmpdir):
    os.makedirs(os.path.join(tmpdir, 'foo/bar'))

    foo = cleanup.enter_context(runc(conn, 'foo', root=os.path.join(tmpdir, 'foo')))
    bar = cleanup.enter_context(runc(conn, 'foo/bar', root='/bar', devices='/dev/null rwm'))
    ExpectException(foo.SetProperty, porto.exceptions.Permission, 'devices[/dev/zero]', '-')


def test_removal1(cleanup, tmpdir):
    os.makedirs(os.path.join(tmpdir, 'foo/bar'))
    os.makedirs(os.path.join(tmpdir, 'foo/baz'))


def test_removal_implicit(cleanup, tmpdir):
    os.makedirs(os.path.join(tmpdir, 'foo/bar'))
    os.makedirs(os.path.join(tmpdir, 'foo/baz'))

    foo = cleanup.enter_context(runc(conn, 'foo', root=os.path.join(tmpdir, 'foo'), devices='/dev/tty0 rwm; /dev/tty1 rwm; /dev/tty2 rwm'))
    bar = cleanup.enter_context(runc(conn, 'foo/bar', root='/bar', devices='/dev/tty0 rwm', devices_explicit=False))
    ExpectDevice(bar, TTY1, "rw")
    ExpectDevice(bar, TTY2, "rw")

    foo.SetProperty('devices', '/dev/tty0 rwm; /dev/tty1 rwm')
    bar.SetProperty('devices', '/dev/tty0 rwm')

    ExpectDevice(bar, TTY1, "rw")
    ExpectDevice(foo, TTY2, "-")
    ExpectDevice(bar, TTY2, "-")

    ExpectException(foo.SetProperty, porto.exceptions.InvalidState, 'devices', '/dev/tty1 rwm')

    baz = cleanup.enter_context(runc(conn, 'foo/baz', root='/baz'))

    bar.SetProperty("devices", "")
    foo.SetProperty('devices', '/dev/tty1 rwm')

    for ct in [foo, bar]:
        ExpectDevice(ct, TTY0, "-")
        ExpectDevice(ct, TTY1, "rw")
        ExpectDevice(ct, TTY2, "-")


def test_removal_explicit(cleanup, tmpdir):
    os.makedirs(os.path.join(tmpdir, 'foo/bar'))
    os.makedirs(os.path.join(tmpdir, 'foo/baz'))

    foo = cleanup.enter_context(runc(conn, 'foo', root=os.path.join(tmpdir, 'foo'), weak=False,
                                     devices='/dev/tty0 rwm; /dev/tty1 rwm; /dev/tty2 rwm'))
    bar = cleanup.enter_context(runc(conn, 'foo/bar', root='/bar', devices='/dev/tty0 rwm', weak=False))

    ExpectException(foo.SetProperty, porto.exceptions.InvalidState, 'devices', '/dev/tty1 rwm')

    ExpectDevice(foo, TTY0, "rw")
    ExpectDevice(foo, TTY1, "rw")
    ExpectDevice(foo, TTY2, "rw")

    ExpectDevice(bar, TTY0, "rw")
    ExpectDevice(bar, TTY1, "-")
    ExpectDevice(bar, TTY2, "-")

    baz = cleanup.enter_context(runc(conn, 'foo/baz', root='/baz', weak=False))

    bar.SetProperty("devices", "")
    foo.SetProperty('devices', '/dev/tty1 rm')
    for ct in [foo, bar, baz]:
        ExpectDevice(ct, TTY0, "-")
        ExpectDevice(ct, TTY1, "rm")
        ExpectDevice(ct, TTY2, "-")

    ReloadPortod()
    conn.Connect()
    foo = conn.Find('foo')
    bar = conn.Find('foo/bar')

    foo.SetProperty('devices', '/dev/tty2 wm')
    for ct in [foo, bar, baz]:
        ExpectDevice(ct, TTY0, "-")
        ExpectDevice(ct, TTY1, "-")
        ExpectDevice(ct, TTY2, "wm")


def test_misc(cleanup, tmpdir):
    os.makedirs(os.path.join(tmpdir, 'foo/bar'))
    os.makedirs(os.path.join(tmpdir, 'foo/baz'))

    foo = cleanup.enter_context(runc(conn, 'foo', root=os.path.join(tmpdir, 'foo'),
                                     devices='/dev/tty0 rwm; /dev/tty1 rwm'))
    bar = cleanup.enter_context(runc(conn, 'foo/bar', root='/bar', devices='/dev/tty0 wm', devices_explicit=False))
    baz = cleanup.enter_context(runc(conn, 'foo/baz', root='/bar', devices='/dev/tty1 rm'))

    ExpectDevice(bar, TTY0, "wm")
    ExpectDevice(bar, TTY1, "rwm")

    ExpectDevice(baz, TTY0, "-")
    ExpectDevice(baz, TTY1, "rm")

    bar.SetProperty("devices[/dev/tty0]", "rm")
    ExpectDevice(bar, TTY0, "rm")

    ExpectException(bar.SetProperty, porto.exceptions.Permission, "devices[/dev/tty2]", "rm")

    ExpectException(foo.SetProperty, porto.exceptions.InvalidState, "devices[/dev/tty0]", "wm")
    ExpectException(foo.SetProperty, porto.exceptions.InvalidState, "devices[/dev/tty1]", "wm")

    ExpectDevice(bar, TTY1, "rwm")
    foo.SetProperty("devices[/dev/tty1]", "rm")
    ExpectDevice(bar, TTY0, "rm")


def test_virt_mode_host(cleanup, tmpdir):
    os.makedirs(os.path.join(tmpdir, 'bar'))

    foo = cleanup.enter_context(runc(conn, 'foo', command='tail -f /dev/null', virt_mode='host'))
    ExpectDevice(foo, TTY2, "rw")
    bar = cleanup.enter_context(runc(conn, 'foo/bar', root=os.path.join(tmpdir, 'bar'), devices='/dev/tty0 wm', devices_explicit=False))
    ExpectDevice(bar, TTY2, "-")
    ExpectDevice(bar, TTY0, "w")


with CreateVolume(conn) as vol, contextlib.ExitStack() as cleanup:
    test_nesting(cleanup, vol.path)

with CreateVolume(conn) as vol, contextlib.ExitStack() as cleanup:
    test_nesting_fuse(cleanup, vol.path)

with CreateVolume(conn) as vol, contextlib.ExitStack() as cleanup:
    test_removed_dev(cleanup, vol.path)

with CreateVolume(conn) as vol, contextlib.ExitStack() as cleanup:
    test_removal_implicit(cleanup, vol.path)

with CreateVolume(conn) as vol, contextlib.ExitStack() as cleanup:
    test_removal_explicit(cleanup, vol.path)

with CreateVolume(conn) as vol, contextlib.ExitStack() as cleanup:
    test_misc(cleanup, vol.path)

with CreateVolume(conn) as vol, contextlib.ExitStack() as cleanup:
    test_virt_mode_host(cleanup, vol.path)
