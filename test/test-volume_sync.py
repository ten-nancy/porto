#!/usr/bin/python3

import time
import threading
import porto
import subprocess
from test_common import *
import random

conn = porto.Connection(timeout=30)

# TODO: this test cannot reproduce race properly
for i in range(10):
    t = threading.Thread(target=conn.CreateVolume, kwargs={'layers': ['ubuntu-noble'], 'backend': 'plain'})
    t.start()

    time.sleep(0.5 + random.randint(-4, 4) * 0.05)
    try:
        conn.UnlinkVolume('/place/porto_volumes/{}/volume'.format(i), '***')
    except (porto.exceptions.VolumeNotFound, porto.exceptions.VolumeNotReady):
        pass

    t.join()
