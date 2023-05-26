#!/usr/bin/python -u

import os
import porto
import sys
import multiprocessing
from test_common import *

def CreateAndUnlinkVolume(id):
    conn = porto.Connection(timeout=30)

    runs = 25
    for i in range(runs):
        volume = conn.CreateVolume()
        volume.Unlink()


def CheckRaceInLogs(porto_tmp_log):
    with open(porto_tmp_log, 'r') as porto_log:
        for line in porto_log:
            if "Remove junk" in line:
                print("Some files were removed on Cleanup. Probably it's race during the removing of a folder in UnlinkVolume and Cleanup")
                print("Line with remove junk log: {}".format(line))
                print("Logs saved in '{}'.".format(porto_tmp_log))

                sys.exit(1)


def TestVolumeRemoveRace():
    porto_tmp_log = '/tmp/porto_test_volume_remove_race.log'
    with open(porto_tmp_log, 'w') as porto_log:
        RestartPortodWithStdlog(porto_log)

        procs_count = 64
        procs = []
        for id in range(procs_count):
            proc = multiprocessing.Process(target=CreateAndUnlinkVolume, args=(id,))
            proc.start()
            procs += [proc]

        for proc in procs:
            proc.join()

        RestartPortod()

    CheckRaceInLogs(porto_tmp_log)
    os.remove(porto_tmp_log)

try:
    TestVolumeRemoveRace()
except Exception as e:
    print("Unexpected error in test volume remove race: {}".format(e))
    sys.exit(1)
