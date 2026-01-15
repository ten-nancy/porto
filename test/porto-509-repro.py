#!/usr/bin/python3
import sys
import os
import uuid
import subprocess

import porto

target = sys.argv[2]

conn = porto.Connection(timeout=30)
# Create new volume to deal with place policy
volume = conn.CreateVolume()
random = uuid.uuid4().hex
inner_place_path = os.path.join(volume.path, random)
outer_place_path = os.path.join(volume.path.replace('/porto/volume_', '/place/porto_volumes/'), 'native', random)

os.makedirs(inner_place_path)

def Catch(func, *args, **kwargs):
    try:
        func(*args, **kwargs)
    except:
        return sys.exc_info()[0]
    return None

def ExpectException(func, exc, *args, **kwargs):
    tmp = Catch(func, *args, **kwargs)
    assert tmp == exc, "method {} should throw {} not {}".format(func, exc, tmp)

if sys.argv[1] == 'layer':
    # Export Layer
    os.makedirs(os.path.join(inner_place_path, 'porto_layers'))
    os.symlink(target, os.path.join(inner_place_path, 'porto_layers', 'test'))
    ExpectException(conn.ReExportLayer, porto.exceptions.Permission,
                    layer="test", tarball="/layer.tar.gz", place=outer_place_path)
elif sys.argv[1]  == 'storage':
    # Export Storage
    os.makedirs(os.path.join(inner_place_path, 'porto_storage'))
    os.symlink(target, os.path.join(inner_place_path, 'porto_storage', 'test'))
    ExpectException(conn.ExportStorage, porto.exceptions.Permission,
                    name="test", tarball="/layer.tar.gz", place=outer_place_path)
