#!/usr/bin/python

import porto
from test_common import *

import contextlib
from tempfile import TemporaryDirectory
import random
import string
import stat
import os

conn = porto.Connection(timeout=30)

@contextlib.contextmanager
def runc(conn, *args, **kwargs):
    ct = conn.Run(*args, **kwargs)
    try:
        yield ct
    finally:
        try:
            ct.Destroy()
        except porto.exceptions.ContainerDoesNotExist:
            pass

# foo
# foo/bar
# foo/bar/baz
# foo/bar/qux
# foo/bar/kek


def test_nesting(cleanup, tmpdir):
    os.makedirs(os.path.join(tmpdir, "foo/bar/baz"))
    os.makedirs(os.path.join(tmpdir, "foo/bar/qux"))
    os.makedirs(os.path.join(tmpdir, "foo/bar/kek"))

    tty0 = os.stat('/dev/tty0')
    tty1 = os.stat('/dev/tty1')

    foo = cleanup.enter_context(runc(conn, "foo", root=os.path.join(tmpdir, "foo"), devices="/dev/tty0 rw"))
    ExpectFile("/proc/{}/root/dev/tty0".format(foo['root_pid']), mode=tty0.st_mode, dev=tty0.st_rdev)

    bar = cleanup.enter_context(runc(conn, "foo/bar", root='/bar'))
    ExpectFile("/proc/{}/root/dev/tty0".format(bar['root_pid']), mode=tty0.st_mode, dev=tty0.st_rdev)

    foo.SetProperty("devices", "/dev/tty0 rw; /dev/tty1 rw")
    for ct in [foo, bar]:
        ExpectFile("/proc/{}/root/dev/tty1".format(ct['root_pid']), mode=tty1.st_mode, dev=tty1.st_rdev)


    baz = cleanup.enter_context(runc(conn, 'foo/bar/baz', root='/baz'))
    ExpectFile("/proc/{}/root/dev/tty0".format(baz['root_pid']), mode=tty0.st_mode, dev=tty0.st_rdev)
    ExpectFile("/proc/{}/root/dev/tty1".format(baz['root_pid']), mode=tty1.st_mode, dev=tty1.st_rdev)

    ExpectException(baz.SetProperty, porto.exceptions.Permission, "devices", "/dev/tty2 rw")

    foo.SetProperty("devices[/dev/tty0]", "-")
    for ct in [foo, bar, baz]:
        ExpectNotExists("/proc/{}/root/dev/tty0".format(ct['root_pid']))

    qux = cleanup.enter_context(runc(conn, 'foo/bar/qux', root='/qux'))
    ExpectFile("/proc/{}/root/dev/tty1".format(qux['root_pid']), mode=tty1.st_mode, dev=tty1.st_rdev)

    bar.SetProperty("devices[/dev/tty1]", "-")
    for ct in [bar, baz, qux]:
        ExpectNotExists("/proc/{}/root/dev/tty1".format(ct['root_pid']))

    kek = cleanup.enter_context(runc(conn, 'foo/bar/kek', root='/kek'))

    ExpectNotExists("/proc/{}/root/dev/tty0".format(kek['root_pid']))
    ExpectNotExists("/proc/{}/root/dev/tty1".format(kek['root_pid']))

    foo.SetProperty("devices[/dev/tty0]", "rw")
    ExpectFile("/proc/{}/root/dev/tty0".format(foo['root_pid']), mode=tty0.st_mode, dev=tty0.st_rdev)
    for ct in [bar, baz, qux, kek]:
        ExpectNotExists("/proc/{}/root/dev/tty0".format(ct['root_pid']))

    ExpectException(baz.SetProperty, porto.exceptions.Permission, "devices", "/dev/tty2 rw")
    ExpectException(baz.SetProperty, porto.exceptions.Permission, "devices", "/dev/tty1 rw")
    ExpectException(baz.SetProperty, porto.exceptions.Permission, "devices", "/dev/tty0 rw")

    bar.SetProperty("devices[/dev/tty1]", "rw")
    for ct in [bar, baz, qux, kek]:
        ExpectFile("/proc/{}/root/dev/tty1".format(ct['root_pid']), mode=tty1.st_mode, dev=tty1.st_rdev)


def test_nesting_fuse(cleanup, tmpdir):
    os.makedirs(os.path.join(tmpdir, "foo/bar/baz"))
    os.makedirs(os.path.join(tmpdir, "foo/bar/qux"))
    os.makedirs(os.path.join(tmpdir, "foo/bar/kek"))

    tty0 = os.stat('/dev/tty0')
    tty1 = os.stat('/dev/tty1')
    fuse = os.stat('/dev/fuse')

    foo = cleanup.enter_context(runc(conn, "foo", root=os.path.join(tmpdir, "foo"), devices="/dev/tty0 rw; /dev/fuse rw"))
    ExpectFile("/proc/{}/root/dev/tty0".format(foo['root_pid']), mode=tty0.st_mode, dev=tty0.st_rdev)
    ExpectFile("/proc/{}/root/dev/fuse".format(foo['root_pid']), mode=fuse.st_mode, dev=fuse.st_rdev)

    bar = cleanup.enter_context(runc(conn, "foo/bar", root="/bar", enable_fuse="true"))
    ExpectFile("/proc/{}/root/dev/tty0".format(bar['root_pid']), mode=tty0.st_mode, dev=tty0.st_rdev)
    ExpectFile("/proc/{}/root/dev/fuse".format(bar['root_pid']), mode=fuse.st_mode, dev=fuse.st_rdev)

    baz = cleanup.enter_context(runc(conn, "foo/bar/baz", root="/baz", devices="/dev/tty0 rw"))
    ExpectFile("/proc/{}/root/dev/tty0".format(baz['root_pid']), mode=tty0.st_mode, dev=tty0.st_rdev)
    ExpectNotExists("/proc/{}/root/dev/fuse".format(baz['root_pid']))

    # TODO(ovov): qux = cleanup.enter_context(runc(conn, "foo/bar/qux", root="/qux", enable_fuse="true"))
    qux = cleanup.enter_context(runc(conn, "foo/bar/qux", root="/qux"))
    ExpectFile("/proc/{}/root/dev/tty0".format(qux['root_pid']), mode=tty0.st_mode, dev=tty0.st_rdev)
    ExpectFile("/proc/{}/root/dev/fuse".format(qux['root_pid']), mode=fuse.st_mode, dev=fuse.st_rdev)

    foo.SetProperty("devices[/dev/tty1]", "rw")
    kek = cleanup.enter_context(runc(conn, "foo/bar/kek", root="/kek"))
    for ct in [foo, bar, qux, kek]:
        ExpectFile("/proc/{}/root/dev/tty0".format(ct['root_pid']), mode=tty0.st_mode, dev=tty0.st_rdev)
        ExpectFile("/proc/{}/root/dev/tty1".format(ct['root_pid']), mode=tty1.st_mode, dev=tty1.st_rdev)

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


with TemporaryDirectory() as tmpdir, contextlib.ExitStack() as cleanup:
    test_nesting(cleanup, tmpdir)

with TemporaryDirectory() as tmpdir, contextlib.ExitStack() as cleanup:
    test_nesting_fuse(cleanup, tmpdir)

with TemporaryDirectory() as tmpdir, contextlib.ExitStack() as cleanup:
    test_removed_dev(cleanup, tmpdir)
