#!/usr/bin/python

from test_common import *
import porto
import os

TEST_PATH_1 = "/tmp/test1"
TEST_PATH_2 = "/tmp/test2"

c = porto.Connection()

all_volumes = [
    TEST_PATH_1 + "/volume/1",
    TEST_PATH_1 + "/kek/1",
    TEST_PATH_1 + "/kek/2",
    TEST_PATH_1 + "/kek/3",
    TEST_PATH_1 + "/kek/lol/1",
    TEST_PATH_2 + "/volume/1",
    TEST_PATH_2 + "/kek/1",
    TEST_PATH_2 + "/kek/2",
    TEST_PATH_2 + "/kek/3",
    TEST_PATH_2 + "/kek/lol/1",
]

mask_and_needed_output = [
    ["/*/***", [                      # incorrect using of wildcard

    ]],

    [ "",
        all_volumes
    ],

    [ "***",
        all_volumes
    ],

    [ TEST_PATH_1 + "/volume/1", [
        TEST_PATH_1 + "/volume/1",
    ]],

    ["***/volume/1", [
        TEST_PATH_1 + "/volume/1",
        TEST_PATH_2 + "/volume/1",
    ]],

    [TEST_PATH_1 + "/***", [
        TEST_PATH_1 + "/volume/1",
        TEST_PATH_1 + "/kek/1",
        TEST_PATH_1 + "/kek/2",
        TEST_PATH_1 + "/kek/3",
        TEST_PATH_1 + "/kek/lol/1",
    ]],

    ["/*/test1/kek/*", [
        TEST_PATH_1 + "/kek/1",
        TEST_PATH_1 + "/kek/2",
        TEST_PATH_1 + "/kek/3",
    ]],
]


try:
    for volume in all_volumes:
        os.makedirs(volume)
        c.CreateVolume(path=volume)

    for [mask, needed_output] in mask_and_needed_output:
        needed_output = sorted(needed_output)
        output = sorted([str(v) for v in c.ListVolumes(mask)])
        ExpectEq(needed_output, output)
except Exception as ex:
    raise ex
finally:
    for volume in all_volumes:
        c.UnlinkVolume(volume)
        os.removedirs(volume)
