from test_common import *

import porto

import glob
import contextlib
import os
import subprocess
import tarfile
import tempfile
from uuid import uuid4


def main(cleanup):
    conn = porto.Connection(timeout=30)
    name = 'test-storage-removal-' + str(uuid4())
    with tempfile.TemporaryDirectory() as tmpdir:
        path = os.path.join(tmpdir, 'foo')
        open(path, 'w').close()
        tarpath = os.path.join(tmpdir, 'foo.tar')
        with tarfile.open(tarpath, mode='w') as t:
            t.add(path, arcname=name)
        conn.ImportStorage(name, tarpath)

        try:
            subprocess.check_call(['mount', '--bind', '/dev/null', '/place/porto_storage/{}/{}'.format(name, name)])

            ExpectException(conn.RemoveStorage, porto.exceptions.Unknown, name=name)
            ExpectException(conn.RemoveStorage, porto.exceptions.Unknown, name=name)
            ExpectEq({'name'} & {x.name for x in conn.ListStorages()}, set())
            ExpectException(conn.ImportStorage, porto.exceptions.Busy, name, tarpath)
        finally:
            for path in glob.glob('/place/porto_storage/*{}/{}'.format(name, name)):
                subprocess.call(['umount', path])
            conn.RemoveStorage(name=name)


if __name__ == '__main__':
    with contextlib.ExitStack() as cleanup:
        main(cleanup)
