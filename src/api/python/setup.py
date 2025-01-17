from setuptools import setup
import re
import os
import sys

def version():
    if os.path.exists("PKG-INFO"):
        with open("PKG-INFO") as f:
            return re.search(r'\nVersion: (\S+)\n', f.read()).group(1)
    if os.path.exists("../../../debian/changelog"):
        with open("../../../debian/changelog") as f:
            return re.search(r'.*\((.*)\).*', f.readline()).group(1)
    return "0.0.0"

def readme():
    with open('README.rst') as f:
        return f.read()

if __name__ == '__main__':

    for x in ['rpc', 'seccomp']:
        try:
            import subprocess
            subprocess.check_call(['protoc', '--python_out=porto', '--proto_path=../..', '{}.proto'.format(x)])
        except Exception as e:
            sys.stderr.write("Cannot compile {}.proto: {}\n".format(x, e))

        if not os.path.exists("porto/{}_pb2.py".format(x)):
            sys.stderr.write("Compiled {}.proto not found\n".format(x))
            sys.exit(-1)

    subprocess.check_call(['sed', '-i', 's/^import seccomp_pb2/from . import seccomp_pb2/', 'porto/rpc_pb2.py'])

    setup(name='portopy',
          version=version(),
          description='Python API for porto',
          long_description=readme(),
          url='https://github.com/ten-nancy/porto',
          author_email='porto@yandex-team.ru',
          maintainer_email='porto@yandex-team.ru',
          license='GNU LGPL v3 License',
          packages=['porto'],
          package_data={'porto': ['rpc.proto', 'seccomp.proto']},
          install_requires=['protobuf'],
          zip_safe=False)
