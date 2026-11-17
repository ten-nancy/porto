#!/usr/bin/python -u

import os
import porto
import shlex
import types

from test_common import *

NAME = os.path.basename(__file__)

(f1, f2) = (0, 0)

def CT_NAME(suffix):
    global NAME
    return NAME + "-" + suffix

def SetPipe(ct):
    (ct.fd1, ct.fd2) = os.pipe()
    ct.SetProperty("stdin_path", "/dev/fd/{}".format(ct.fd1))

def Check(ct, value):
    os.close(ct.fd1)
    os.close(ct.fd2)
    ct.Wait()
    ExpectEq(ct["stderr"], "")
    ExpectEq(ct["exit_status"], "0")
    ExpectEq(ct.GetProperty("stdout"), value)
    ct.Stop()


CMD0 = '''
import porto
import sys
import os

c = porto.Connection(timeout=30);
try:
        name = c.LocateProcess({}).name;
except porto.exceptions.InvalidValue as e:
        name = "failed"
sys.stdout.write(name)
'''.strip()
CMD1 = f'read; python3 -c {shlex.quote(CMD0)}'
CMD = f'bash -c {shlex.quote(CMD1)}'

conn = porto.Connection(timeout=30)

Catch(conn.Destroy, CT_NAME("a"))
Catch(conn.Destroy, CT_NAME("c"))

ct = conn.Create(CT_NAME("a"))
ct.Prepare = types.MethodType(SetPipe, ct)
ct.Check = types.MethodType(Check, ct)
ct.SetProperty("env", "PYTHONPATH={}".format(
    os.getenv("PYTHONPATH", os.path.join(portosrc, "src/api/python"))))

ct.Prepare()
ct.SetProperty("command", CMD.format(2))
ct.SetProperty("porto_namespace", CT_NAME("a"))
ct.Start()
ct.Check("self")

ct.Prepare()
ct.SetProperty("porto_namespace", CT_NAME("a/"))
ct.Start()
ct.Check("self")

ct.Prepare()
ct.SetProperty("porto_namespace", "")
ct.SetProperty("command", CMD.format(2))
ct.Start()
ct.Check("self")

ct.Prepare()
ct2 = conn.Create(CT_NAME("a/b"))
ct2.SetProperty("command", "sleep 1000")
ct.SetProperty("command", CMD.format(4))
ct2.Start()
ct.Check(CT_NAME("a/b"))

ct.Prepare()
ct2.SetProperty("isolate", False)
ct.SetProperty("command", CMD.format(3))
ct2.Start()
ct.Check(CT_NAME("a/b"))

ct.Prepare()
ct2.SetProperty("isolate", True)
ct.SetProperty("enable_porto", "isolate")
ct.SetProperty("command", CMD.format(4))
ct2.Start()
ct.Check("b")

ct.Prepare()
ct2.Destroy()
ct2 = conn.Create(CT_NAME("c"))
ct2.SetProperty("command", "sleep 1000")
ct2.Start()
ct.SetProperty("command", CMD.format(1024))
ct.Start()
ct.Check("failed")

ct.Prepare()
ct3 = conn.Create(CT_NAME("c/c"))
ct3.SetProperty("command", "sleep 1000")
ct3.Start()
ct.SetProperty("command", CMD.format(1024))
ct.Start()
ct.Check("failed")

ct.Destroy()
ct2.Destroy()
