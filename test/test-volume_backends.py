#!/usr/bin/python3

import os
import time
import array
import errno
import functools
import fcntl
import struct
import shutil
import tarfile
import subprocess
import traceback
import tempfile

import porto
from test_common import *

DIR = None
DUMMYLAYER = None
PLACE = "/place/porto_volumes"


def DumpObjectState(r, keys):
    for k in keys:
        try:
            value = r.GetProperty(k)
            try:
                value = value.rstrip()
            except:
                pass
        except:
            value = "n/a"

        print("{} : \"{}\"".format(k, value))

    print()

def porto_reconnect(c):
    c.disconnect()
    return porto.Connection(timeout=30)

def get_quota_fs_projid(path):
    fmt = "IIII12x"
    arr = array.array('B')
    arr.frombytes(b"\x00" * struct.calcsize(fmt))
    fd = os.open(path , os.O_RDONLY | os.O_NOCTTY | os.O_NOFOLLOW |
                       os.O_NOATIME | os.O_NONBLOCK)
    fcntl.ioctl(fd, 0x801c581f, arr)
    projid = struct.unpack("IIII12x", arr.tobytes())[3]
    os.close(fd)
    return projid

def check_readonly(c, path, **args):
    args["read_only"] = "true"
    with CreateVolume(c, path, **args) as v:
        cmd = "bash -c 'echo 123 > {}/123.txt'".format(v.path)
        with RunContainer(c, name='test', command=cmd, wait=30) as ct:
            assert ct["exit_status"] == "256"

def check_place(c, path, **args):
    place_num = len(os.listdir(PLACE))
    vol_num = len(c.ListVolumes())

    with CreateVolume(c, path, **args) as v:
        place_volumes = os.listdir(PLACE)
        assert len(place_volumes) == place_num + 1
        assert len(c.ListVolumes()) == vol_num + 1

        os.stat(os.path.join(PLACE, v['id'], args["backend"]))

    assert len(os.listdir(PLACE)) == place_num
    assert len(c.ListVolumes()) == vol_num

def check_destroy_linked_non_root(c, path, **args):
    place_num = len(os.listdir(PLACE))
    vol_num = len(c.ListVolumes())

    with CreateContainer(c, 'test') as r, CreateVolume(c, path, **args) as v:
        place_volumes = os.listdir(PLACE)
        assert len(place_volumes) == place_num + 1
        assert len(c.ListVolumes()) == vol_num + 1

        r.SetProperty("capabilities[SYS_RESOURCE]", "true")
        v.Link("test")
        v.Unlink("/")
        r.SetProperty("command", "echo 123")
        r.Start()
        r.Wait()

    assert len(os.listdir(PLACE)) == place_num
    assert len(c.ListVolumes()) == vol_num

def check_mounted(c, path, **args):
    r = None
    with CreateContainer(c, 'test') as r, CreateVolume(c, path, **args) as v:
        open(v.path + "/file.txt", "w").write("aabb")
        r.SetProperty("capabilities[SYS_RESOURCE]", "true")
        v.Link("test")
        v.Unlink("/")
        r.SetProperty("command", "bash -c \"cat " + v.path + "/file.txt; echo -n 3210 > " +\
                      v.path + "/file.txt  \"")
        r.Start()
        r.Wait()
        assert r.GetProperty("stdout") == "aabb"
        if path is not None:
            assert open(path + "/file.txt", "r").read() == "3210"
        assert open(v.path + "/file.txt", "r").read() == "3210"
        path = v.path

    if args["backend"] != "quota":
        assert Catch(os.stat, path + "/file.txt") == FileNotFoundError
    else:
        os.remove(path + "/file.txt")

def check_space_limit(c, r, v, path, limits):
    r.SetProperty("capabilities[SYS_RESOURCE]", "true")
    v.Link("test")
    v.Unlink("/")
    r.SetProperty("command", "dd if=/dev/zero of=" + v.path + "/file.zeroes " + \
                  "bs=1048576 count=" + limits[1])
    r.Start()
    r.Wait()

    assert os.stat(v.path + "/file.zeroes").st_size <= int(limits[2])

def check_tune_space_limit(c, path, limits=None, **args):
    if limits is None:
        limits = ("1M", "3", "1048576", "4M", "3145728")

    args["space_limit"] = limits[0]

    with CreateContainer(c, name="test") as r, CreateVolume(c, path, **args) as v:
        check_space_limit(c, r, v, path, limits)
        r.Stop()
        os.remove(v.path + "/file.zeroes")
        v.Tune(space_limit=limits[3])
        r.Start()
        r.Wait()
        assert os.stat(v.path + "/file.zeroes").st_size == int(limits[4])

def check_inode_limit(c, r, v, path):
    r.SetProperty("capabilities[SYS_RESOURCE]", "true")
    v.Link("test")
    v.Unlink("/")
    r.SetProperty("command", "bash -c \"for i in \$(seq 1 24); do touch " + v.path +\
                  "/\$i; done\"")
    r.Start()
    r.Wait()
    assert r.GetProperty("exit_status") == "256"
    assert len(os.listdir(v.path)) < 24

def check_tune_inode_limit(c, path, **args):
    if "inode_limit" not in args:
        args["inode_limit"] = "16"
    with CreateContainer(c, 'test') as r, CreateVolume(c, path, **args) as v:
        check_inode_limit(c, r, v, path)
        r.Stop()
        v.Tune(inode_limit="32")
        r.Start()
        r.Wait()
        assert r.GetProperty("exit_status") == "0"
        assert len(os.listdir(v.path)) >= 24
        r.Destroy()

@contextlib.contextmanager
def _check_layers(c, path, cleanup=True, **args):
    args["layers"] = ["ubuntu-noble", "test-volumes"]
    with CreateContainer(c, 'test') as r, CreateVolume(c, path, **args) as v:
        r.SetProperty("capabilities[SYS_RESOURCE]", "true")
        v.Link("test")
        v.Unlink("/")
        r.SetProperty("root", v.path)
        r.SetProperty("command", "cat /test_file.txt")
        r.Start()
        r.Wait()
        assert r.GetProperty("exit_status") == "0"
        assert r.GetProperty("stdout") == "1234567890"
        yield r, v

def check_layers(*args, cleanup=True, **kwargs):
    if cleanup:
        with _check_layers(*args, cleanup=cleanup, **kwargs) as (r, v):
            pass
    return _check_layers(*args, cleanup=cleanup, **kwargs)


def check_projid_is_set(c, path, **args):
    with CreateVolume(c, path, **args) as v:
        open(path + "/file.txt", "w").write("111")

        #Checking quota project id is set
        assert get_quota_fs_projid(DIR) == 0
        projid = get_quota_fs_projid(path)
        ino_id = os.stat(path).st_ino

        assert projid == ino_id + 2 ** 31
        assert get_quota_fs_projid(path + "/file.txt") == projid

        v.Unlink("/")

        # FIXME keep_project_quota_id
        assert get_quota_fs_projid(path) == projid
        assert get_quota_fs_projid(path + "/file.txt") == projid

        os.unlink(path + "/file.txt")

def backend_plain(c):
    args = dict()
    args["backend"] = "plain"

    TMPDIR = os.path.join(DIR, args["backend"])
    os.mkdir(TMPDIR)

    for path in [None, TMPDIR]:

        check_readonly(c, path, **args)
        check_place(c, path, **args)
        check_mounted(c, path, **args)
        check_destroy_linked_non_root(c, path, **args)

        assert Catch(check_tune_space_limit, c, path, **args) ==\
                     porto.exceptions.InvalidProperty
        assert Catch(check_tune_inode_limit, c, path, **args) ==\
                     porto.exceptions.InvalidProperty
        check_layers(c, path, **args)

    os.rmdir(TMPDIR)


def backend_bind(c):
    args = dict()
    args["backend"] = "bind"

    TMPDIR = DIR + "/bind"
    os.mkdir(TMPDIR)

    assert Catch(check_mounted, c, None, **args) == porto.exceptions.InvalidProperty
    assert Catch(check_mounted, c, TMPDIR, **args) == porto.exceptions.InvalidProperty

    BIND_STORAGE_DIR = DIR + "/bind_storage"
    os.mkdir(BIND_STORAGE_DIR)
    args["storage"] = BIND_STORAGE_DIR

    check_readonly(c, TMPDIR, **args)
    check_destroy_linked_non_root(c, TMPDIR, **args)
    check_mounted(c, TMPDIR, **args)

    ExpectException(check_tune_space_limit, porto.exceptions.InvalidProperty,
                    c, TMPDIR, **args)
    ExpectException(check_tune_inode_limit, porto.exceptions.InvalidProperty,
                    c, TMPDIR, **args)
    ExpectException(check_layers, porto.exceptions.InvalidProperty, c, TMPDIR, **args)

    os.rmdir(TMPDIR)
    os.remove(BIND_STORAGE_DIR + "/file.txt")
    os.rmdir(BIND_STORAGE_DIR)


def backend_bind_regular(conn):
    with contextlib.ExitStack() as cleanup:
        rootfs = cleanup.enter_context(CreateVolume(conn, backend="overlay", layers=["ubuntu-noble"]))
        shell = 'touch /foo /bar && portoctl vcreate /foo backend=bind storage=/bar'
        ct = cleanup.enter_context(
            RunContainer(
                conn,
                name="foobar",
                root=rootfs.path,
                command="sh -c '{}'".format(shell),
                bind='{} /bin/portoctl ro'.format(portoctl),
                wait=5,
                enable_porto='isolate'
            )
        )
        ExpectEq(ct['state'], 'dead')
        assert ct['exit_code'] == '0', 'exit_code={}\n{}'.format(ct['exit_code'], ct['stderr'])


def backend_tmpfs(c):
    args = dict()
    args["backend"] = "tmpfs"
    TMPDIR = DIR + "/tmpfs"
    os.mkdir(TMPDIR)


    for path in [None, TMPDIR]:
        args["space_limit"] = "1M"
        check_readonly(c, path, **args)
        check_mounted(c, path, **args)
        check_destroy_linked_non_root(c, path, **args)
        check_tune_space_limit(c, path, **args)
        check_tune_inode_limit(c, path, **args)

    assert Catch(check_layers, c, TMPDIR, **args) == porto.exceptions.InvalidProperty

    os.rmdir(TMPDIR)

def backend_quota(c):
    args = dict()
    args["backend"] = "quota"
    TMPDIR = os.path.join(DIR, "quota")
    os.mkdir(TMPDIR)

    assert Catch(check_mounted, c, None, **args) == porto.exceptions.InvalidProperty
    assert Catch(check_mounted, c, TMPDIR, **args) == porto.exceptions.InvalidProperty

    args["space_limit"] = "1M"
    check_mounted(c, TMPDIR, **args)
    assert Catch(check_readonly, c, TMPDIR, **args) == porto.exceptions.InvalidProperty

    check_destroy_linked_non_root(c, TMPDIR, **args)

    assert Catch(check_tune_space_limit, c, TMPDIR, **args) == AssertionError
    os.remove(os.path.join(TMPDIR, "file.zeroes"))

    os.chown(TMPDIR, alice_uid, alice_gid)
    AsAlice()
    c = porto_reconnect(c)

    check_tune_space_limit(c, TMPDIR, **args)

    AsRoot()
    c = porto_reconnect(c)

    assert Catch(check_tune_inode_limit, c, TMPDIR, **args) == AssertionError
    for i in os.listdir(TMPDIR):
        os.remove(TMPDIR + "/" + i)

    AsAlice()
    c = porto_reconnect(c)

    check_tune_inode_limit(c, TMPDIR, **args)

    assert Catch(check_layers, c, TMPDIR, **args) == porto.exceptions.InvalidProperty

    AsRoot()
    c = porto_reconnect(c)

    args["space_limit"] = "1M"
    check_projid_is_set(c, TMPDIR, **args)

    for i in os.listdir(TMPDIR):
        if int(i) <= 24:
            os.remove(TMPDIR+"/"+i)
    os.rmdir(TMPDIR)

    # do not set FS_XFLAG_PROJINHERIT for files
    v = c.CreateVolume()
    with open(os.path.join(v.path, 'f'), 'w') as _:
        pass
    v2 = c.CreateVolume(storage=v.path, space_limit='1M')
    v2.Unlink()
    v.Unlink()

def backend_native(c):
    args = dict()
    args["backend"] = "native"

    TMPDIR = DIR + "/" + args["backend"]
    os.mkdir(TMPDIR)
    os.chown(TMPDIR, alice_uid, alice_gid)

    for path in [None, TMPDIR]:
        check_readonly(c, path, **args)
        check_place(c, path, **args)
        check_mounted(c, path, **args)
        check_destroy_linked_non_root(c, path, **args)

        AsAlice()
        c = porto_reconnect(c)
        check_tune_space_limit(c, path, **args)
        check_tune_inode_limit(c, path, **args)
        AsRoot()
        c = porto_reconnect(c)
        check_layers(c, path, **args)

    os.rmdir(TMPDIR)

def backend_overlay(c):
    def copyup_quota(dest):
        ALAYER = TMPDIR + "/a_layer.tar"

        f1 = tempfile.TemporaryFile()
        fzero = open("/dev/zero", "rb")
        f1.write(b"\x01" * (32 * 1048576))
        size1 = os.fstat(f1.fileno()).st_size
        f1.seek(0)

        f2 = tempfile.TemporaryFile()
        f2.write(b"\x02" * (32 * 1048576))
        size2 = os.fstat(f2.fileno()).st_size
        f2.seek(0)

        t = tarfile.open(name=ALAYER, mode="w")
        t.addfile(t.gettarinfo(arcname="a1", fileobj=f1), fileobj = f1)
        t.addfile(t.gettarinfo(arcname="a2", fileobj=f2), fileobj = f2)
        t.close()
        f1.close()
        f2.close()

        c.ImportLayer("a_layer", ALAYER)
        os.unlink(ALAYER)
        space_limit = (size1 + size2) * 3 / 4
        with CreateVolume(c, dest, layers=["a_layer"], space_limit=str(space_limit)) as v:
            with CreateContainer(c, "a") as r:
                r.SetProperty("command", "bash -c \'echo 123 >> {}/a1\'".format(v.path))
                r.Start()
                r.Wait()
                assert r.GetProperty("exit_status") == "0"
                assert int(v.GetProperty("space_used")) <= int(v.GetProperty("space_limit"))

                r.Stop()
                r.SetProperty("command", "bash -c \'echo 456 >> {}/a2 || true\'".format(v.path))
                r.Start()
                r.Wait()
                assert r.GetProperty("exit_status") == "0"
                assert int(v.GetProperty("space_used")) <= int(v.GetProperty("space_limit"))
                assert os.statvfs(v.path).f_bfree != 0

        c.RemoveLayer("a_layer")

    def opaque_xattr(dest):
        DLAYER = TMPDIR + "/d_layer.tar"

        os.mkdir(TMPDIR + "/d_dir")

        t = tarfile.open(name=DLAYER, mode="w")
        t.add(TMPDIR + "/d_dir", arcname="d1")
        t.add(TMPDIR + "/d_dir", arcname="d2")

        f = tempfile.TemporaryFile()

        f.write("a1".encode('utf-8'))
        f.seek(0)
        t.addfile(t.gettarinfo(arcname="d1/a1", fileobj=f), fileobj=f)
        f.seek(0)
        t.addfile(t.gettarinfo(arcname="d2/a1", fileobj=f), fileobj=f)

        f.seek(0)
        f.write("a2".encode('utf-8'))
        f.seek(0)
        t.addfile(t.gettarinfo(arcname="d1/a2", fileobj=f), fileobj=f)
        f.seek(0)
        t.addfile(t.gettarinfo(arcname="d2/a2", fileobj=f), fileobj=f)

        t.close()
        f.close()
        os.rmdir(TMPDIR + "/d_dir")

        c.ImportLayer("d_layer", DLAYER)
        os.unlink(DLAYER)

        v = c.CreateVolume(dest, layers=["d_layer"])

        assert os.path.exists(v.path + "/d1")
        assert os.path.exists(v.path + "/d1/a1")
        assert os.path.exists(v.path + "/d1/a2")
        assert os.path.exists(v.path + "/d2")
        assert os.path.exists(v.path + "/d2/a1")
        assert os.path.exists(v.path + "/d2/a2")

        os.unlink(v.path + "/d2/a1")
        os.unlink(v.path + "/d2/a2")
        os.rmdir(v.path + "/d2")

        os.mkdir(v.path + "/d2")
        open(v.path + "/d2/a3", "w").write("a3")

        c.ExportLayer(v.path, DLAYER)
        c.ImportLayer("d_removed_layer", DLAYER)
        v.Unlink()

        v = c.CreateVolume(dest, layers=["d_removed_layer", "d_layer"])

        assert os.path.exists(v.path + "/d1")
        assert os.path.exists(v.path + "/d1/a1")
        assert os.path.exists(v.path + "/d1/a2")
        assert os.path.exists(v.path + "/d2")
        assert os.path.exists(v.path + "/d2/a3")
        assert open(v.path + "/d2/a3", "r").read() == "a3"
        try:
            assert not os.path.exists(v.path + "/d2/a1")
            assert not os.path.exists(v.path + "/d2/a2")
        except AssertionError:
            #FIXME: remove when tar --xargs wiil be used
            print("Directory opaqueness is lost as expected")
            pass

        v.Unlink()
        c.RemoveLayer("d_removed_layer")
        c.RemoveLayer("d_layer")

    args = dict()
    args["backend"] = "overlay"
    args["layers"] = ["test-volumes"]

    TMPDIR = DIR + "/" + args["backend"]
    os.mkdir(TMPDIR)
    os.chown(TMPDIR, alice_uid, alice_gid)

    for path in [None, TMPDIR]:
        check_readonly(c, path, **args)
        check_place(c, path, **args)
        check_mounted(c, path, **args)
        check_destroy_linked_non_root(c, path, **args)

        AsAlice()
        c = porto_reconnect(c)
        copyup_quota(path)
        opaque_xattr(path)
        check_tune_space_limit(c, path, **args)
        check_tune_inode_limit(c, path, **args)
        AsRoot()
        c = porto_reconnect(c)

        with check_layers(c, path, cleanup=False, **args) as (r, v):
            r.Stop()
            r.SetProperty("command", "bash -c \"echo -n 1234 > /test_file2.txt\"")
            r.Start()
            r.Wait()
            v.Export(DIR + "/upper.tar")

        with open(DIR + "/upper.tar", "rb") as f:
            assert tarfile.open(fileobj=f).extractfile("test_file2.txt").read() == "1234".encode('utf-8')
        os.remove(DIR + "/upper.tar")

    os.rmdir(TMPDIR)

def backend_loop(c):
    args = {"backend": "loop"}

    TMPDIR = DIR + "/" + args["backend"]
    os.mkdir(TMPDIR)

    for path in [None, TMPDIR]:
        args["space_limit"] = "512M"
        check_readonly(c, path, **args)
        check_place(c, path, **args)
        check_mounted(c, path, **args)
        check_destroy_linked_non_root(c, path, **args)
        check_tune_space_limit(c, path, ("512M", "768", "536870912", "1024M", "805306368"), **args)
        args["space_limit"] = "1G"
        check_layers(c, path, **args)

    ExpectException(c.CreateVolume, porto.exceptions.InvalidValue, **args, fs_type="squashfs")

    args["space_limit"] = "512M"
    v = c.CreateVolume(**args)
    args["storage"] = os.path.abspath(v.path + "/../loop/loop.img")
    assert Catch(c.CreateVolume, **args) == porto.exceptions.Busy
    v.Unlink("/")

    f = open(DIR + "/loop.img", "w")
    f.truncate(512 * 1048576)
    f.close()

    subprocess.check_call(["mkfs.ext4", "-F", "-q", DIR + "/loop.img"])
    args["storage"] = os.path.abspath(DIR + "/loop.img")
    v = c.CreateVolume(**args)
    assert Catch(c.CreateVolume, **args) == porto.exceptions.Busy
    v.Unlink("/")

    subprocess.check_call(["mksquashfs", TMPDIR, DIR + "/loop.squash"])
    args["storage"] = os.path.abspath(DIR + "/loop.squash")
    ExpectException(c.CreateVolume, porto.exceptions.InvalidFilesystem,
                    **args, fs_type="ext4", read_only="true")
    ExpectException(c.CreateVolume, porto.exceptions.InvalidValue,
                    **args, fs_type="squashfs")
    v = c.CreateVolume(**args, fs_type="squashfs", read_only="true")
    assert Catch(c.CreateVolume, **args) == porto.exceptions.Busy
    v.Unlink("/")

    os.unlink(DIR + "/loop.img")
    os.rmdir(TMPDIR)

    with open(DIR + "/loop.squash", "wb") as f:
        f.write(b"broken")

    ExpectException(c.CreateVolume, porto.exceptions.InvalidFilesystem, **args, fs_type="squashfs", read_only="true")
    ExpectException(c.CreateVolume, porto.exceptions.InvalidFilesystem,
                    backend="squash", layers=[DIR + "/loop.squash"], read_only="true")

    with open(DIR + "/loop.squash", "wb") as f:
        pass

    ExpectException(c.CreateVolume, porto.exceptions.InvalidFilesystem, **args, fs_type="squashfs", read_only="true")
    ExpectException(c.CreateVolume, porto.exceptions.InvalidFilesystem,
                    backend="squash", layers=[DIR + "/loop.squash"], read_only="true")


def setup_loop_blocksize(fd, blocksize):
    for i in range(64):
        try:
            #define LOOP_SET_BLOCK_SIZE	0x4C09
            fcntl.ioctl(fd, 0x4C09, blocksize)
            return
        except OSerror as e:
            if e.errno != errno.EGAIN:
                raise


def backend_loop_blksize(conn):
    with contextlib.ExitStack() as es:
        tmpvol = conn.CreateVolume()
        es.callback(tmpvol.Unlink)
        es.callback(functools.partial, os.chdir, os.path.abspath('.'))
        os.chdir(tmpvol.path)

        with open("image", 'w') as f:
            f.truncate(1<<20)
            image_name = f.name

        subprocess.check_call(['mkfs.ext4', '-O', '^has_journal', '-b', '4096', image_name])

        # xenial version of losetup does not have --sector-size  and --direct-io options
        dev = subprocess.check_output(['losetup',
                                       '--show',
                                       '-f',
                                       image_name]).decode().strip()
        es.callback(lambda: subprocess.check_call(['losetup', '-d', dev]))

        with open(dev) as f:
            setup_loop_blocksize(f.fileno(), 4096)
            #defune LOOP_SET_DIRECT_IO	0x4C08
            fcntl.ioctl(f.fileno(), 0x4C08, 1)

        os.mkdir('root')
        subprocess.check_call(['mount', '-t', 'ext4', dev, 'root'])
        es.callback(lambda: subprocess.check_call(['umount', 'root']))

        dummy = conn.CreateVolume()
        es.callback(dummy.Unlink)

        overlay = conn.CreateVolume(backend='overlay', layers=[dummy.path], storage=os.path.abspath('root'))
        es.callback(overlay.Unlink)

        os.mkdir("dir")
        os.mknod("dir/foobar")

        squash_path = os.path.join(overlay.path, 'image.squash')
        subprocess.check_call(["mksquashfs", "dir", squash_path])
        vol = conn.CreateVolume(backend='loop', storage=squash_path, fs_type='squashfs', read_only='true')
        es.callback(vol.Unlink)


def backend_rbd():
    #Not implemented yet
    pass


def TestBody(c):
    ExpectEq(len(os.listdir(PLACE)), 1)
    ExpectEq(len(c.ListVolumes()), 1)

    open(DIR + "/test_file.txt", "w").write("1234567890")
    t = tarfile.open(name=DUMMYLAYER, mode="w")
    t.add(DIR + "/test_file.txt", arcname="test_file.txt")
    t.close()
    os.remove(DIR + "/test_file.txt")

    Catch(c.RemoveLayer, "test-volumes")
    c.ImportLayer("test-volumes", DUMMYLAYER)
    os.unlink(DUMMYLAYER)

    backend_plain(c)
    backend_bind(c)
    backend_bind_regular(c)
    backend_tmpfs(c)
    backend_quota(c)
    backend_native(c)
    backend_overlay(c)
    backend_loop(c)
    backend_loop_blksize(c)

    c.RemoveLayer("test-volumes")
    ExpectEq(len(c.ListVolumes()), 1)
    ExpectEq(len(os.listdir(PLACE)), 1)


c = porto.Connection(timeout=30)
try:
    with CreateVolume(c) as vol:
        DIR = vol.path
        DUMMYLAYER = os.path.join(vol.path, "test_layer.tar")
        TestBody(c)
finally:
    try:
        AsRoot()
        c = porto_reconnect(c)

        for l in ["test-volumes", DUMMYLAYER, "a_layer", "d_layer", "d_removed_layer"]:
            Catch(c.RemoveLayer, l)
    except Exception:
        pass

