#!/usr/bin/python -u

import os
import porto
import time
from pathlib import Path
from test_common import *
import shutil
import tarfile

test_path = "/tmp/test-async-cleanup"

STORAGE_TYPES = ["porto_layers", "porto_storage", "porto_volumes"]

def create_tar():
    with open(os.path.join(test_path, "file.txt"), "w") as f:
        f.write("Oh shit, here we go again")

    with tarfile.open(name=os.path.join(test_path,  "layer.tar"), mode="w") as t:
        t.add(os.path.join(test_path, "file.txt"), arcname="file.txt")


def TriggerCleanup(conn):
    volume = conn.CreateVolume()
    volume.Unlink()


def PathOrSymlinkExists(path):
    return os.path.exists(path) or os.path.islink(path)


def CreateSymlink(target, link):
    if PathOrSymlinkExists(link):
        Path(link).unlink()

    Path(link).symlink_to(target)


def CreateFoldersAndSymlinks():
    for storage_type in STORAGE_TYPES:
        remove_dir = "/place/{}/_remove_dir".format(storage_type)

        Path(remove_dir).mkdir(exist_ok=True)

        tmp_dir = "/tmp/{}_tmp_dir".format(storage_type)
        tmp_dir_link = "/place/{}/tmp_dir_link".format(storage_type)

        Path(tmp_dir).mkdir(exist_ok=True)
        CreateSymlink(tmp_dir, tmp_dir_link)

        non_existing_dir = "/tmp/{}_non_existing_dir".format(storage_type)
        non_existing_dir_link = "/place/{}/non_existing_dir_link".format(storage_type)

        CreateSymlink(non_existing_dir, non_existing_dir_link)


def GetDeletedPaths():
    deleted_paths = []
    for storage_type in STORAGE_TYPES:
        remove_dir = "/place/{}/_remove_dir".format(storage_type)

        tmp_dir = "/tmp/{}_tmp_dir".format(storage_type)
        tmp_dir_link = "/place/{}/tmp_dir_link".format(storage_type)

        non_existing_dir_link = "/place/{}/non_existing_dir_link".format(storage_type)

        deleted_paths += [remove_dir, tmp_dir, tmp_dir_link, non_existing_dir_link]

    return deleted_paths


def DeleteTestPaths():
    for deleted_path in GetDeletedPaths():
        if os.path.isdir(deleted_path):
            os.rmdir(deleted_path)
        elif os.path.islink(deleted_path):
            os.unlink(deleted_path)


def TestAsyncCleanup():
    async_remove_watchdog_ms = 500
    wait_before_remove_s = 4 * async_remove_watchdog_ms / 1000

    ConfigurePortod("async-cleanup", """
daemon {
    docker_images_support: true
},
volumes {
    async_remove_watchdog_ms: %d
}
""" % (async_remove_watchdog_ms))

    conn = porto.Connection(timeout=10)

    CreateFoldersAndSymlinks()

    TriggerCleanup(conn)
    time.sleep(wait_before_remove_s)

    for deleted_path in GetDeletedPaths():
        assert not PathOrSymlinkExists(deleted_path), "Path '{}' shouldn't exist".format(deleted_path)

def TestSiblingBindImport():
    script_path =  os.path.join(test_path, "tar.sh")
    # This script checks memory cgroup of his process
    with open(script_path, 'w') as f:
        f.write("""
        #!/bin/bash
        sleep 3
        tar $@
        """)
    os.chmod(script_path, 0o755)

    ConfigurePortod("test-sibling-bind-import", """
    daemon {
        tar_path: "%s",
    }
    volumes {
        async_remove_watchdog_ms: %d
    }
    """ % (script_path, 5))
    conn = porto.Connection(timeout=10)
    volumes = []

    def createVolume(*args, **kwargs):
        vol = conn.CreateVolume(*args, **kwargs)
        volumes.append(vol)
        return vol

    try:
        storage = createVolume(backend='plain')

        vol1 = createVolume(backend='bind', storage=storage.path)
        vol2 = createVolume(backend='bind', storage=storage.path)
        vol3 = createVolume(backend='plain', place=vol1.path)
        vol4 = createVolume(backend='plain', place=vol2.path)
        create_tar()
        conn.ImportLayer("layer", os.path.join(test_path, "layer.tar"), place=vol1.path)
    finally:
        for vol in volumes[::-1]:
            conn.UnlinkVolume(vol.path)

try:
    TestAsyncCleanup()
except Exception as e:
    print("Unexpected error in test async cleanup: {}".format(e))
    sys.exit(1)
finally:
    DeleteTestPaths()
    ConfigurePortod('async-cleanup', "")

try:
    shutil.rmtree(test_path)
except:
    pass
os.mkdir(test_path)


try:
    TestSiblingBindImport()
finally:
    shutil.rmtree(test_path)
