#!/usr/bin/python3

import time
import threading
import porto
import subprocess
from test_common import *
import random

def vunlink():
    c = porto.Connection(timeout=30)
    try:
        c.CreateVolume(layers=['ubuntu-precise'], backend='plain')
    except Exception as e:
        return e

for i in range(10):
    t = threading.Thread(target=vunlink)
    t.start()

    time.sleep(0.5 + random.randint(-4, 4) * 0.05)
    c = porto.Connection(timeout=30)
    try:
        c.UnlinkVolume('/place/porto_volumes/%d/volume' % i, '***')

    except (porto.exceptions.VolumeNotFound, porto.exceptions.VolumeNotReady):
        pass

    except Exception as e:
        raise e

    t.join()
