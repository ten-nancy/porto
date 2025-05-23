#!/usr/bin/python3

import os
import time
import errno
import pwd
import grp
import sys
import os
import subprocess
import shutil
import tarfile
import uuid

import porto
from test_common import *

#stdin/stderr/stdout privilege escalation

def run_streams(r):
    r.SetProperty("command", "echo -n 654321")
    r.SetProperty("stdout_path", "/tmp/porto-tests/root-secret")
    assert Catch(r.Start) == porto.exceptions.InvalidValue

    r.SetProperty("command", "tee /proc/self/fd/2")
    r.SetProperty("stdin_path", "/tmp/porto-tests/porto-alice-stdin")
    r.SetProperty("stderr_path", "/tmp/porto-tests/root-secret")
    assert Catch(r.Start) == porto.exceptions.InvalidValue

    r.SetProperty("command", "cat")
    r.SetProperty("stdin_path", "/tmp/porto-tests/root-secret")
    r.SetProperty("stdout_path", "")
    r.SetProperty("stderr_path", "")
    assert Catch(r.Start) == porto.exceptions.InvalidValue


def std_streams_escalation():
    Catch(os.remove,"/tmp/porto-tests/root-secret")
    Catch(os.remove,"/tmp/porto-tests/porto-alice-stdin")

    f = open("/tmp/porto-tests/root-secret", "w")
    f.write("0123456789")
    os.fchmod(f.fileno(), 0o600)
    f.close()

    AsAlice()

    f = open("/tmp/porto-tests/porto-alice-stdin", "w+")
    f.write("123456")
    f.close()

    c = porto.Connection(timeout=30)
    r = c.Create("test")

    #run under user
    run_streams(r)

    AsRoot()

    c = porto.Connection(timeout=30)
    r = c.Find("test")

    #run under root
    run_streams(r)

    AsRoot()
    c.Destroy("test")
    os.remove("/tmp/porto-tests/root-secret")
    os.remove("/tmp/porto-tests/porto-alice-stdin")

#child escapes parent namespace (leaving chroot)

def ns_escape_container():
    w = int(sys.argv[2])
    to_kill = int(sys.argv[3])
    try:
        f = open("/tmp/porto-tests/root-secret", "r")
        print(f.read())
        f.close()
        print("FAIL",)
    except IOError as e:
        if e[0] == errno.ENOENT:
            print("OK",)
            sys.stdout.flush()
            if w > 0:
                time.sleep(w)

            if to_kill:
                pid = os.getpid()
                os.kill(pid, 9)
        else:
            print("FAIL",)
    except:
        print("FAIL",)

    sys.stdout.flush()

def ns_escape(v):
    try:
        os.remove("/tmp/porto-tests/root-secret")
    except:
        pass

    f = open("/tmp/porto-tests/root-secret","w")
    f.write("123456")
    f.close()
    os.chmod("/tmp/porto-tests/root-secret", 0o600)

    AsAlice()

    c = porto.Connection(timeout=30)

    r = c.Create("parent")
    r.SetProperty("root", v.path)
    r.SetProperty("command", "python3 /porto/test/test-security.py ns_escape_container 2 1")
    r.SetProperty("porto_namespace", "parent")

    r = c.Create("parent/child")
    #FIXME:
    #porto r.SetProperty("command","cat /porto/test/test-security.py") shows file contents, but
    #c.SetProperty("parent/child","command", "python /porto/test/test-security.py ns_escape_container 10 0") fails (file not found)
    r.SetProperty("command", "sleep 10")
    r.SetProperty("respawn", "true")
    r.SetProperty("max_respawns", "1")
    r.SetProperty("root_readonly","true")
    r.SetProperty("porto_namespace", "parent/child")
    r.Start()
    time.sleep(5)

    assert c.GetProperty("parent","state") == "dead" and c.GetProperty("parent/child", "state") == "dead"
    output = c.Get(["parent","parent/child"], ["stdout"])
    assert output["parent"]["stdout"] == "OK"

    #And now vice versa...

    c.Stop("parent")

    c.SetProperty("parent", "command", "python3 /porto/test/test-security.py ns_escape_container 3 0")
    #Actually, on fail because of ns escape
    #we won't even find our python test, but anyway...
    r.SetProperty("command", "python3 /porto/test/test-security.py ns_escape_container 0 1")

    r.Start()

    c.Wait(["parent"])
    output = c.Get(["parent","parent/child"], ["stdout"])
    assert output["parent"]["stdout"] == "OK\n"
    assert output["parent/child"]["stdout"] == "OKOK"

    AsRoot()

    c.Destroy("parent")
    os.unlink("/tmp/porto-tests/root-secret")

#binds privilege escalation

def read_shadow():
    f = open("/tmp/shadow", "r")
    print(f.read())
    f.close()


def append_sudoers():
    f = open("/tmp/sudoers", "a")
    print("Opened sudoers for append...")
    sys.stdout.flush()
    #f.write("\tmax7255 (ALL) NOPASSWD: ALL")
    f.close()


def append_passwd():
    f = open("/tmp/passwd", "a")
    #f.write("joker:x:1980:1980:::/bin/false")
    print("Opened passwd for append...")
    sys.stdout.flush()
    f.close()


def binds_escalation(v):
    c = porto.Connection(timeout=30)

    AsAlice()
    c = porto.Connection(timeout=30)
    r = c.Create("bind_file")
    r.SetProperty("root", v.path)
    r.SetProperty("bind", "/etc/shadow /tmp/shadow ro")
    r.SetProperty("command", "python3 /porto/test/test-security.py read_shadow")
    assert Catch(r.Start) == porto.exceptions.PermissionError

    r.SetProperty("bind", "/etc/passwd /tmp/passwd rw")
    r.SetProperty("command", "python3 /porto/test/test-security.py append_passwd")
    assert Catch(r.Start) == porto.exceptions.PermissionError

    r.SetProperty("bind", "/etc/sudoers /tmp/sudoers rw")
    r.SetProperty("command", "python3 /porto/test/test-security.py append_sudoers")
    assert Catch(r.Start) == porto.exceptions.PermissionError

    r.SetProperty("bind", "/sbin /tmp/lol rw")
    r.SetProperty("command", "/tmp/lol/hwclock")
    assert Catch(r.Start) == porto.exceptions.PermissionError

    r.Destroy()
    AsRoot()

    os.mkdir("/tmp/porto-tests/dir1")
    os.chmod("/tmp/porto-tests/dir1", 0o777)
    os.mkdir("/tmp/porto-tests/mount1")
    os.chmod("/tmp/porto-tests/mount1", 0o555)
    os.mkdir("/tmp/porto-tests/dir-bob")
    os.chmod("/tmp/porto-tests/dir-bob", 0o700)
    os.chown("/tmp/porto-tests/dir-bob", bob_uid, bob_gid)

    AsAlice()

    f = open("/tmp/porto-tests/dir1/file", "w+")
    f.write("123456")
    f.close()

    c = porto.Connection(timeout=30)
    r = c.Create("test")
    r.SetProperty("bind", "/tmp/porto-tests/dir1 /tmp/porto-tests/mount1/mount2 rw")
    r.SetProperty("command", "dd if=/dev/zero of=/tmp/porto-tests/mount1/mount2/file bs=32 count=1")

    assert Catch(r.Start) == porto.exceptions.PermissionError

    r.SetProperty("bind", "/tmp/porto-tests/dir-bob /tmp/porto-tests/mount1/mount2 rw")

    assert Catch(r.Start) == porto.exceptions.PermissionError

    c.Destroy("test")
    AsRoot()

#privilege escalation for requests from inside the porto container w virt_mode=="os"

def internal_escalation_container():
    print('internal_escalation_container')
    c = porto.Connection(timeout=30)
    r = c.Create("test_cont2", weak=False)


def internal_escalation(v):
    c = porto.Connection(timeout=120)

    AsAlice()
    c = porto.Connection(timeout=30)
    r = c.Create("test_cont1")
    r.SetProperty("porto_namespace", "")
    r.SetProperty("virt_mode", "app")
    r.SetProperty("root", v.path)
    r.SetProperty("env", "PYTHON3PATH=/porto/src/api/python;PORTO_TEST_NO_RESTART=1;")
    r.SetProperty("command", "python3 /porto/test/test-security.py internal_escalation_container")

    r.Start()
    r.Wait()

    print(r['stdout'])
    print(r['stderr'])

    ExpectEq(r['exit_status'], '0')
    ExpectEq(c.GetProperty("test_cont2", "user"), "porto-alice")

    r.Destroy()
    c.Destroy("test_cont2")
    AsRoot()

#porto_namespace escape

def porto_namespace_escape_container():
    c = porto.Connection(timeout=30)
    c.SetProperty("self", "porto_namespace", "")


def porto_namespace_escape(v):
    AsAlice()

    c = porto.Connection(timeout=30)
    r = c.Create("test")
    r.SetProperty("porto_namespace", "test")
    r.SetProperty("root", v.path)
    r.SetProperty("env", "PORTO_TEST_NO_RESTART=1;")
    r.SetProperty("command", "python3 /porto/test/test-security.py porto_namespace_escape_container")
    r.Start()
    r.Wait()

    assert r.GetProperty("porto_namespace") == "test"
    assert r.GetData("exit_status") != "0"

    r.Destroy()

    AsRoot()

#layers privilege escalation/escape

def layer_escalation_container():
    #We can use e.g. /etc down there...
    os.symlink("/tmp/porto-tests", "porto-tests")

    t = tarfile.open(name="/layer0.tar", mode="w")
    t.add("porto-tests")
    t.close()

    os.remove("porto-tests")
    os.mkdir("porto-tests")

    #And we can place /etc/sudoers here...
    f = open("porto-tests/evil_file", "w")
    f.write("pwned")
    f.close()

    t = tarfile.open(name="/layer1.tar", mode="w")
    t.add("porto-tests/evil_file")
    t.close()

    c = porto.Connection(timeout=30)

    #We have persist layers here in porto, let's create clean layer for test
    try:
        c.RemoveLayer("test-layer")
    except:
        pass

    l = c.ImportLayer("test-layer", "/layer0.tar")
    l.Merge("/layer1.tar")

    c.RemoveLayer("test-layer")

    os.remove("porto-tests/evil_file")
    os.rmdir("porto-tests")
    os.remove("layer0.tar")
    os.remove("layer1.tar")


def layer_escalation_volume_container():
    os.mkdir("layer")
    f = open("layer/good_file", "w")
    f.write("pwned")

    vol_path = sys.argv[2]
    c = porto.Connection(timeout=30)
    subprocess.check_call(["/portobin/portoctl", "vcreate", "/layer",
                           "path={}/../../../../tmp/porto-tests".format(vol_path),
                           "layers=/layer"])


def layer_escalation(v):
    AsAlice()
    c = porto.Connection(timeout=30)
    r = c.Create("test")
    r.SetProperty("root", v.path)
    r.SetProperty("env", "PORTO_TEST_NO_RESTART=1")
    r.SetProperty("command", "python3 /porto/test/test-security.py layer_escalation_container")

    r.Start()
    r.Wait()
    print(r['stderr'])
    ExpectEq(r['exit_code'], '0')

    assert Catch(open, "/tmp/porto-tests/evil_file", "r") == IOError

    assert Catch(c.RemoveLayer, "../../../..") == porto.exceptions.InvalidValue
    assert Catch(c.ImportLayer, "../etc", "/tmp") == porto.exceptions.InvalidValue

    r.Destroy()

    AsRoot()

    f = open("/tmp/porto-tests/good_file", "w")
    f.write("I am a duck")
    f.close()

    AsAlice()
    c = porto.Connection(timeout=30)
    r = c.Create("test")


    r.SetProperty("root", v.path)
    r.SetProperty("env", "PORTO_TEST_NO_RESTART=1;")
    r.SetProperty("command", "python3 /porto/test/test-security.py layer_escalation_volume_container " + v.path)
    r.SetProperty("stdout_path","/tmp/stdout")
    r.SetProperty("stderr_path","/tmp/stderr")
    r.SetProperty("bind", "{} /portobin ro".format(portobin))

    r.Start()
    r.Wait()

    assert open("/tmp/porto-tests/good_file", "r").read() == "I am a duck"

    r.Destroy()

    AsRoot()


if len(sys.argv) > 1:
    exec(sys.argv[1]+"()")
    exit(0)

def setup_volume(conn, vol):
    ct = None
    path = os.path.join(vol.path, "porto")
    os.mkdir(path)

    portosrc_vol = conn.CreateVolume(
        path=path,
        backend='overlay',
        layers=[portosrc],
    )
    try:
        cmd = 'apt-get update; apt-get install -y apt-utils protobuf-compiler python3-setuptools; cd /porto/src/api/python/; python3 ./setup.py install'
        ct = c.Run(name='tmp', root=vol.path, command="bash -c '{}'".format(cmd), wait=30)
        ExpectEq(ct['state'], 'dead')
        ExpectEq(ct['exit_status'], '0')
    finally:
        if ct:
            ct.Destroy()


AsRoot()

c = porto.Connection(timeout=120)

v = c.CreateVolume(path=None, layers=["jammy"])
setup_volume(c, v)

try:
    shutil.rmtree("/tmp/porto-tests")
except:
    pass

os.mkdir("/tmp/porto-tests")
os.chmod("/tmp/porto-tests", 0o777)

std_streams_escalation()
binds_escalation(v)
internal_escalation(v)
porto_namespace_escape(v)
layer_escalation(v)
ns_escape(v)

shutil.rmtree("/tmp/porto-tests")
v.Unlink()

# check PORTO-509

random = uuid.uuid4().hex
v = c.CreateVolume(path=None, layers=["ubuntu-precise"])
v2 = c.CreateVolume(path=None)

with open(os.path.join(v2.path, random), 'w') as _:
    pass

shutil.copyfile(os.path.join(os.path.dirname(sys.argv[0]), 'porto-509-repro.py'), os.path.join(v.path, 'repr.py'))

chroot_portoctl = os.path.join(v.path, 'portoctl')
shutil.copyfile(portoctl, chroot_portoctl)
os.chmod(chroot_portoctl, 755)

a = c.Run('abc', wait=10, weak=True, root=v.path, command='python /repr.py layer {}'.format(v2.path))
ExpectNe(a['exit_code'], '0')
ExpectNe(a['stderr'].find('exit status 11'), -1)
a.Destroy()

a = c.Run('abc', wait=10, weak=True, root=v.path, command='python /repr.py storage {}'.format(v2.path))
ExpectNe(a['exit_code'], '0')
ExpectNe(a['stderr'].find('exit status 11'), -1)
a.Destroy()

# FOR REPRODUCE
#output = subprocess.check_output(['tar', '-tvf', "{}/layer.tar.gz".format(v.path)])
#ExpectEq(output.find(random), -1)

v.Unlink()
v2.Unlink()

# test client containers permission
def CheckCtPermission(name, is_sucess):
    b = c.Run(name, wait=3, command=portoctl + ' set abc thread_limit 1000')
    ExpectEq(b['exit_code'] == '0', is_sucess)
    b.Destroy()

a = c.Run('abc', wait=0)

b = c.Run('abc/d', wait=3, command=portoctl + ' set abc owner_containers "bcd;cde/fgh"')
ExpectEq(b['exit_code'], '0')
b.Destroy()

CheckCtPermission('abc/d', False)
CheckCtPermission('bcd', True)

CheckCtPermission('bcd', True)
bcd = c.Run('bcd', wait=0)
CheckCtPermission('bcd/e', False)
bcd.Destroy()

CheckCtPermission('cde', True)
cde = c.Run('cde', wait=0)
CheckCtPermission('cde/fgh', True)
CheckCtPermission('cde/fg', False)
CheckCtPermission('cde/fghh', False)
cde.Destroy()

b = c.Run('bcd', wait=3, command=portoctl + ' set abc owner_containers "self;abc"')
ExpectEq(b['exit_code'], '0')
b.Destroy()

ExpectEq(c.GetProperty('abc','owner_containers'), 'bcd;abc')

a.Destroy()
