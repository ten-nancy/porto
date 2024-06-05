#!/usr/bin/python -u

import os
import porto
import sys
from test_common import *

c = porto.Connection()

v = c.CreateVolume()
a = c.Run("a", weak=False)
v.Link(a)
os.unlink("/run/porto/kvs/" + a['id'])
ReloadPortod()
ExpectEq(Catch(c.Find, "a"), porto.exceptions.ContainerDoesNotExist)
c.FindVolume(v.path)
v.Unlink()

# test junk cleanup on volume creation
v_path_components = v.path.split('/')
v_id = int(v_path_components[-2])
v_junk = '/'.join(v_path_components[:-2] + [str(v_id + 1), 'volume'])
os.makedirs(v_junk)

v1 = c.CreateVolume()
ExpectEq(v1.path, v_junk)
v1.Unlink()
assert int(c.GetData('/', 'porto_stat[warnings]')) == 1

# check that ownership transfer completed successfully and we do not receive warning after reload
ct_c = c.Create("c")
v_bc = c.CreateVolume(layers=["ubuntu-precise"], owner_container='c')

ct_b = c.Create("b")
ct_b.SetProperty("command", "sleep 10000")
v_bc.Link(ct_b)

ct_b.Start()
ct_c.Destroy()

ReloadPortod()
ct_b.Destroy()
v_bc.Unlink()

assert int(c.GetData('/', 'porto_stat[warnings]')) == 1
