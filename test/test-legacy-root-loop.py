import porto
import datetime
from test_common import *

conn = porto.Connection(timeout=30)

w = conn.CreateWeakContainer('w')

base = conn.CreateVolume(containers='w')

loop = conn.CreateVolume(backend='loop', space_limit='1G', layers=['ubuntu-noble'], storage=base.path, containers='w')
loop.Destroy()

a = conn.Run('a', root=base.path + '/loop.img')
a.Destroy()

a = conn.Run('a', root=base.path + '/loop.img', root_readonly=True)
a.Destroy()

base.Destroy()
