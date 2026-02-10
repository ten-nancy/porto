#!/usr/bin/python -u

import contextlib
import os
import porto
import sys

from test_common import *

c = porto.Connection(timeout=30)

v = c.CreateVolume()
a = c.Run("a", weak=False)
v.Link(a)
os.unlink("/run/porto/kvs/" + a['id'])
ReloadPortod()
ExpectEq(Catch(c.Find, "a"), porto.exceptions.ContainerDoesNotExist)
c.FindVolume(v.path)
v.Unlink()

# test junk cleanup on volume creation
warns = int(c.GetData('/', 'porto_stat[warnings]'))
v_path_components = v.path.split('/')
v_id = int(v_path_components[-2])
v_junk = '/'.join(v_path_components[:-2] + [str(v_id + 1), 'volume'])
os.makedirs(v_junk)

v1 = c.CreateVolume()
ExpectEq(v1.path, v_junk)
v1.Unlink()
warns += 1
ExpectEq(int(c.GetData('/', 'porto_stat[warnings]')), warns)

# check that ownership transfer completed successfully and we do not receive warning after reload
ct_c = c.Create("c")
v_bc = c.CreateVolume(layers=["ubuntu-noble"], owner_container='c')

ct_b = c.Create("b")
ct_b.SetProperty("command", "sleep 10000")
v_bc.Link(ct_b)

ct_b.Start()
ct_c.Destroy()

ReloadPortod()
ct_b.Destroy()
v_bc.Unlink()

ExpectEq(int(c.GetData('/', 'porto_stat[warnings]')), warns)

def test_self_dependency_cycle(conn, cleanup):
    vol = cleanup.enter_context(CreateVolume(conn))
    for x in ["foo", "bar"]:
        p = os.path.join(vol.path, x)
        os.mkdir(p)
        cleanup.enter_context(CreateVolume(conn, path=p, storage=p, backend="native"))

    ReloadPortod()
    vol.Unlink()


with contextlib.ExitStack() as cleanup:
    test_self_dependency_cycle(c, cleanup)

# TODO: remove after fixing self-dependency on restore
warns += 2

ExpectEq(int(c.GetData('/', 'porto_stat[warnings]')), warns)
