import contextlib
import os
import subprocess

import porto
from test_common import CreateVolume, ExpectEq, portoctl


conn = porto.Connection(timeout=30)
contents = b'Hello, world!\n'
with open('hello_world.txt', 'wb') as f:
    f.write(contents)

archive = os.path.abspath('hello-world.tar.lz4')
subprocess.check_call(['tar', '--use-compress-program=lz4', '-cf', archive,
                       'hello_world.txt'])

with contextlib.ExitStack() as cleanup:
    layer = conn.ImportLayer('test-layer-lz4', archive)
    cleanup.callback(layer.Remove)

    with CreateVolume(conn, backend='overlay', layers=[layer, 'ubuntu-noble']) as volume:
        output = subprocess.check_output([portoctl, 'exec', 'test-layer-lz4',
                                          'root=' + volume.path, 'command=cat /hello_world.txt'],
                                         stdin=subprocess.DEVNULL)
        ExpectEq(output, contents)
