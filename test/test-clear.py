#!/usr/bin/python -u

import os
import porto
import tarfile
import tempfile
import shutil
import time

from test_common import *

AsRoot()

NAME = "test-clear-storage"

WIDTHS = [255, 1, 255, 1, 255, 1, 12, 255, 1, 255, 1, 36, 1, 255, 1, 1, 1, 4, 1, 1, 4, 4] + [255] * 16

def CreateRecursive(path):
    dirfd1 = None
    dirfd = os.open(path, os.O_PATH)
    try:
        for i in WIDTHS:
            name = 'x' * i
            os.mkdir(name, dir_fd=dirfd)

            dirfd1 = os.open(name, os.O_PATH, dir_fd=dirfd)
            os.close(dirfd)
            dirfd, dirfd1 = dirfd1, None

        os.close(os.open("file.txt", os.O_CREAT|os.O_WRONLY, dir_fd=dirfd))
    finally:
        os.close(dirfd)
        if dirfd1 is not None:
            os.close(dirfd1)

c = porto.Connection(timeout=10)

try:
    c.RemoveStorage(NAME)
except porto.exceptions.VolumeNotFound:
    pass


with tempfile.TemporaryDirectory() as tmpdir, contextlib.ExitStack() as cleanup:
    storage_path = os.path.join(tmpdir, "storage.tar")
    with tarfile.open(storage_path, "w") as t, tempfile.NamedTemporaryFile() as tmpfile:
        tmpfile.write(b"foobar")
        tmpfile.flush()
        t.add(tmpdir, arcname="file.txt")

    c.ImportStorage(NAME, storage_path)
    v = cleanup.enter_context(CreateVolume(c, backend='bind', storage=NAME))

    assert os.stat(v.path) != None
    assert os.stat(os.path.join(v.path, "file.txt")) != None

    print("Preparing special directory tree of depth: {}, path len: {}  ... ".format(len(WIDTHS), sum(WIDTHS)))
    start_ts = time.time()
    CreateRecursive(v.path)
    stop_ts = time.time()
    print("Prepare took {} s".format(stop_ts - start_ts))

    v.Unlink()

    creation = stop_ts - start_ts

    print("Removing layer... ")
    start_ts = time.time()
    c.RemoveStorage(NAME)
    stop_ts = time.time()
    print("Removal took {} s".format(stop_ts - start_ts))

    storages = {x.name for x in c.ListStorages()}
    assert NAME not in storages, "{} must not be in {}".format(NAME, storages)
