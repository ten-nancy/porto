#!/usr/bin/python -u

import os
import porto
import time
from pathlib import Path
from test_common import *

STORAGE_TYPES = ["porto_layers", "porto_storage", "porto_volumes"]


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

try:
    TestAsyncCleanup()
except Exception as e:
    print("Unexpected error in test async cleanup: {}".format(e))
    sys.exit(1)
finally:
    DeleteTestPaths()
    ConfigurePortod('async-cleanup', "")
