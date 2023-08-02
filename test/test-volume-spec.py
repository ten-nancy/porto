#!/usr/bin/python3

from test_common import *
import porto
import os

volume_path = "/tmp/volume_path"
share_path = "path"
share_origin_path = "/tmp/origin_path"

c = porto.Connection(timeout=30)

if not os.path.exists(volume_path):
    os.mkdir(volume_path)

if not os.path.exists(share_origin_path):
    os.mkdir(share_origin_path)

try:
    volume = c.NewVolume({
        "path": volume_path,
        "backend": "native",
        "shares": [{
            "path": share_path,
            "origin_path": share_origin_path
        }]
    })

    ExpectEq(volume["path"], volume_path)
    ExpectEq(volume["backend"], "native")

    spec = c.GetVolume(volume_path)

    ExpectEq(spec["path"], volume_path)
    ExpectEq(spec["backend"], "native")

finally:
    c.DestroyVolume(volume_path)
    os.rmdir(volume_path)
    os.rmdir(share_origin_path)
