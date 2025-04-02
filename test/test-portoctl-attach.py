import subprocess
import shlex
import tempfile
from test_common import *

with tempfile.TemporaryDirectory() as tmp:
    fname = os.path.join(tmp, 'script.sh')
    with open(fname, 'w', 0o777) as f:
        subcmd = '{portoctl} attach self/bar $$; {portoctl} get self absolute_name'.format(portoctl=portoctl)
        f.write('''#!/bin/bash
        {portoctl} create self/bar
        {portoctl} set self/bar isolate false
        {portoctl} start self/bar
        {portoctl} get self absolute_name
        bash -c {subcmd}
        '''.format(portoctl=portoctl, subcmd=shlex.quote(subcmd)))
        os.fchmod(f.fileno(), 0o777)

    cmd = [portoctl, 'exec', 'foo', "command=bash -c {}".format(fname)]
    output = subprocess.check_output(cmd, stdin=subprocess.PIPE).decode()
    ExpectEq(output, '/porto/foo\n/porto/foo/bar\n')
