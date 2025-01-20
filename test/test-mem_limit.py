#!/usr/bin/python

import porto
import os
import types
from test_common import *

SIZE = 2048 * 1048576
EPS = 16 * 1048576

(kmaj, kmin) = GetKernelVersion()
OLD_KERNEL = kmaj < 4 or (kmaj == 4 and kmin < 3)

DURATION = 60000 #ms

NAME = os.path.basename(__file__)
USE_CGROUP2 = GetUseCgroup2()
MEMORY_MAX_KNOB = "memory.max" if USE_CGROUP2 else "memory.max_usage_in_bytes"
MEMORY_ANON_MAX_KNOB = "memory.anon.max_usage"

if USE_CGROUP2:
    print "\nuse {} hierarchy\n".format("cgroup2" if USE_CGROUP2 else "cgroup1")

def test_memory_limit_high(conn):
    ct = conn.Run("memory_high", cpu_limit='1c')

    ExpectEq(ct.GetProperty('memory.limit_in_bytes'), ct.GetProperty('memory.high_limit_in_bytes'))

    ct.SetProperty('memory_limit', '10485760')
    ExpectEq(int(ct.GetProperty('memory.high_limit_in_bytes')), 10223616)

    ct.SetProperty('cpu_limit', '2c')
    ExpectEq(int(ct.GetProperty('memory.high_limit_in_bytes')), 9961472)

    ct.SetProperty('memory_limit', '41943040')
    ExpectEq(int(ct.GetProperty('memory.high_limit_in_bytes')), 41418752)

    ct1 = conn.Run("memory_high/cpu", cpu_limit='1c')
    ExpectEq(int(ct.GetProperty('memory.high_limit_in_bytes')), 41418752)

    ct1.SetProperty('cpu_limit', '2c')
    ExpectEq(int(ct.GetProperty('memory.high_limit_in_bytes')), 41418752)

    ct.SetProperty('memory_limit', '0')
    ExpectEq(ct.GetProperty('memory.limit_in_bytes'), ct.GetProperty('memory.high_limit_in_bytes'))


def CT_NAME(suffix):
    global NAME
    return NAME + "-" + suffix

def Prepare(ct, anon=0, total=0, use_anon=0, use_file=0, meta=False, to_wait=False, weak=True):
    if OLD_KERNEL:
        ct.SetProperty("user", "root")
        ct.SetProperty("group", "root")
        ct.SetProperty("capabilities", "IPC_LOCK;DAC_OVERRIDE")
    else:
        ct.SetProperty("capabilities", "IPC_LOCK")
        ct.SetProperty("capabilities_ambient", "IPC_LOCK")

    ct.SetProperty("ulimit", "memlock: unlimited")

    ct.UseAnon = use_anon
    ct.UseFile = use_file
    ct.Meta = meta

    if total > 0:
        ct.SetProperty("memory_limit", total + EPS)
        ct.Total = total + EPS
    else:
        ct.SetProperty("memory_limit", 0)

    if not USE_CGROUP2:
        if anon > 0:
            ct.SetProperty("anon_limit", anon + EPS)
            ct.Anon = anon + EPS
            ct.Total = max(ct.Total, ct.Anon)
        else:
            ct.SetProperty("anon_limit", 0)

    if not meta:
        ct.Vol = ct.conn.CreateVolume(None, [], space_limit=str(SIZE))
        ct.Vol.Link(ct.name)
        ct.Vol.Unlink("/")
        ct.SetProperty("command", "{}/mem_touch {} {} {}/test.mapped{}"\
                       .format(os.getcwd(), use_anon, use_file, ct.Vol.path,\
                               " wait" if to_wait else ""))
    if not weak:
        ct.SetProperty("weak", "false")

    return ct

def CheckOOM(ct, expect_oom):
    assert ct.Wait(DURATION * 2) == ct.name, "container runtime twice exceeded"\
                                             " expected duration {}".format(DURATION)

    print "{} max total usage: {}, anon usage: {}".format(ct,
           ct.GetProperty(MEMORY_MAX_KNOB).rstrip(),\
           ct.GetProperty(MEMORY_ANON_MAX_KNOB).rstrip() if not USE_CGROUP2 else "not supported")

    if expect_oom:
        print "exit_code: {}".format(ct.GetProperty("exit_code"))
        if ct.GetProperty("exit_code") == "2":
            return

        ExpectProp(ct, "oom_killed", True)
    else:
        ExpectProp(ct, "exit_status", "0")
        if not USE_CGROUP2 and ct.Anon > 0:
            ExpectPropLe(ct, MEMORY_ANON_MAX_KNOB, ct.Anon)
        if ct.Total > 0:
            ExpectPropLe(ct, MEMORY_MAX_KNOB, ct.Total)

    ct.Stop()
    if ct.Vol is not None:
        ct.Vol.Unlink(ct.name)

def WaitUsage(ct):
    wait_duration = 0

    if ct.UseAnon == 0 and ct.UseFile == 0:
        return

    while True:
        if not USE_CGROUP2 and ct.UseAnon > 0 and\
           int(ct.GetProperty(MEMORY_ANON_MAX_KNOB)) >= ct.UseAnon:
            break

        value = ct.GetProperty(MEMORY_MAX_KNOB)
        if ct.UseFile > 0 and (value == "max\n" or int(value) >= ct.UseFile):
            break

        assert ct.Wait(500) == "", "container {} died prior to achieving desired usage file: {} anon: {}"\
                                   .format(ct.name, ct.UseAnon, ct.UseFile)
        wait_duration += 500
        assert wait_duration < 2 * DURATION, "container wait twice exceeded "\
                                             "expected duration {}".format(DURATION)

def CheckAlive(ct):
    if ct.Meta:
        ExpectProp(ct, "state", "meta")
    else:
        ExpectProp(ct, "state", "running")

    ct.Stop()
    if ct.Vol is not None:
        ct.Vol.Unlink(ct.name)

def StartIt(ct):
    ct.conn.Start(ct.name)
    return ct

def Alloc(conn, suffix):
    ct = conn.CreateWeakContainer(CT_NAME(suffix))
    ct.Prepare = types.MethodType(Prepare, ct)
    ct.CheckOOM = types.MethodType(CheckOOM, ct)
    ct.CheckAlive = types.MethodType(CheckAlive, ct)
    ct.WaitUsage = types.MethodType(WaitUsage, ct)
    ct.Start = types.MethodType(StartIt, ct)
    ct.Total = 0
    ct.Anon = 0
    ct.UseAnon = 0
    ct.UseFile = 0
    ct.Meta = False
    ct.Vol = None
    return ct

conn = porto.Connection(timeout=30)
conn.Alloc = types.MethodType(Alloc, conn)

print "\nMemory limit test, SIZE: {}, EPS: {}\n".format(SIZE, EPS)

print "\nCheck limit can be achieved\n"

conn.Alloc("achieve_anon").Prepare(anon=SIZE, total=0, use_anon=SIZE, use_file=0)\
                          .Start()\
                          .CheckOOM(False)

conn.Alloc("achieve_file").Prepare(anon=0, total=SIZE, use_anon=0, use_file=SIZE)\
                          .Start()\
                          .CheckOOM(False)

conn.Alloc("achieve_mixed").Prepare(anon=SIZE / 3, total=SIZE,\
                                    use_anon=SIZE / 3, use_file=2 * SIZE / 3)\
                           .Start()\
                           .CheckOOM(False)

print "\nCheck OOM triggering\n"

conn.Alloc("oom_file").Prepare(anon=0, total=SIZE, use_anon=0, use_file=SIZE * 2)\
                      .Start()\
                      .CheckOOM(True)

if not USE_CGROUP2:
    conn.Alloc("oom_anon").Prepare(anon=SIZE, total=0, use_anon=SIZE * 2, use_file=0)\
                          .Start()\
                          .CheckOOM(True)

conn.Alloc("oom_anon_with_file").Prepare(anon=SIZE / 2, total=SIZE,\
                                         use_anon=SIZE, use_file=SIZE / 2)\
                                .Start()\
                                .CheckOOM(True)

if USE_CGROUP2:
    print "\nSkip hierarchical check\n"
    exit(0)

print "\nCheck hierarchical\n"

ct = conn.Alloc("meta").Prepare(anon=SIZE / 2, total=SIZE, meta=True)

ct1 = conn.Alloc("meta/ct1").Prepare(to_wait=True, anon=SIZE / 4, total=SIZE / 2,\
                                use_anon = SIZE / 4, use_file = SIZE / 4)

ct2 = conn.Alloc("meta/ct2").Prepare(to_wait=True, anon=SIZE / 4, total=SIZE / 2,\
                                use_anon = SIZE / 4, use_file = SIZE / 4)

#every container lives in its limits

ct1.Start(); ct2.Start()
ct1.WaitUsage(); ct2.WaitUsage()
ct2.CheckAlive(); ct1.CheckAlive(); ct.CheckAlive()

#oom isolated in ct2

ct1.Prepare(to_wait=True, use_anon = SIZE / 4, use_file = SIZE / 4)
ct2.Prepare(anon=SIZE / 5, use_anon=SIZE / 4)
ct1.Start(); ct1.WaitUsage();
ct2.Start();
ct2.CheckOOM(True); ct1.CheckAlive(); ct.CheckAlive()

#oom in ct2 kills ct and ct1

ct1.Prepare(to_wait=True, use_anon = SIZE / 4, use_file = SIZE / 4)
ct2.Prepare(use_anon=SIZE)
ct1.Start(); ct1.WaitUsage();
ct2.Start()
ct2.CheckOOM(True); ct1.CheckOOM(True); ct.CheckOOM(True)

print "\nCheck memory.high\n"
test_memory_limit_high(conn)
