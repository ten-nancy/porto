import porto
from test_common import *
import platform
import subprocess
import shutil

REGISTRY = "mirror.gcr.io"

IMAGE_NAME = "alpine:3.16.2"
IMAGE_TAG = REGISTRY + "/library/alpine:3.16.2"
IMAGE_DIGEST = "9c6f0724472873bb50a2ae67a9e7adcb57673a183cea8b06eb778dca859181b5"
LAYER_NAME = "alpine"

K8S_IMAGE_TAG = "registry.k8s.io/pause:3.7"
K8S_IMAGE_DIGEST = "221177c6082a88ea4f6240ab2450d540955ac6f4d5454f0e15751b653ebda165"
K8S_IMAGE_ALT_TAG = REGISTRY + "/kndrvt/pause:latest"

UBUNTU_JAMMY_IMAGE_TAG = REGISTRY + "/library/ubuntu:jammy-20240125"
UBUNTU_JAMMY_IMAGE_DIGEST = "fd1d8f58e8aedc22ec0a3a7ce1a33de544a596eaa6cdb842f1af7c5e081d453f"

PLACE = ""

STORAGE_PATH = "/place/porto_docker/v1"

TARGET_ARCH = "amd64"

conn = porto.Connection(timeout=120)

ConfigurePortod('docker-images', """
daemon {
    docker_images_support: true
}
""")

def is_x86_64():
    machine = platform.machine()
    return machine.lower() in ('x86_64', 'amd64')

def check_storage_of_image(digest, tags):
    for tag in tags:
        path = "{}/tags/v2/{}".format(STORAGE_PATH, tag.replace(":", "/"))
        Expect(os.path.exists(path))

    path = "{}/images/{}/{}".format(STORAGE_PATH, digest[:2], digest)
    Expect(os.path.exists(path))


def check_image(image, digest, tags):
    ExpectEq(image.id, digest)
    ExpectEq(set(image.tags), set(tags))
    ExpectEq(len(image.digests), 1)
    ExpectEq(image.digests[0], "sha256:" + digest)
    check_storage_of_image(digest, tags)


def check_storage_is_empty():
    for x in ("tags", "images", "layers"):
        Expect(os.path.exists("{}/{}".format(STORAGE_PATH, x)))
        xs = os.listdir("{}/{}".format(STORAGE_PATH, x))
        Expect(not xs, message="{}: {}".format(x, "; ".join(xs)))


# prepare
print("Prepare place")
try:
    check_storage_is_empty()
except:
    # cleanup before test
    for x in ("tags", "images", "layers"):
        for y in os.listdir("{}/{}".format(STORAGE_PATH, x)):
            shutil.rmtree("{}/{}/{}".format(STORAGE_PATH, x, y))


# api
print("Check python api")
check_storage_is_empty()

image = conn.PullDockerImage(IMAGE_NAME, place=PLACE, platform=TARGET_ARCH)
check_image(image, IMAGE_DIGEST, [IMAGE_TAG])

for mask in (None, IMAGE_TAG[:-1]+"***", IMAGE_TAG):
    images = conn.ListDockerImages(mask=mask, place=PLACE)
    Expect(image.id in list(map(lambda x: x.id, images)))
    if mask:
        ExpectEq(len(images), 1)
        check_image(images[0], IMAGE_DIGEST, [IMAGE_TAG])

for name in (IMAGE_NAME, IMAGE_TAG, IMAGE_DIGEST, IMAGE_DIGEST[:12]):
    image = conn.DockerImageStatus(IMAGE_DIGEST[:12], place=PLACE)
    check_image(image, IMAGE_DIGEST, [IMAGE_TAG])

for name in (IMAGE_NAME, IMAGE_TAG, IMAGE_DIGEST, IMAGE_DIGEST[:12]):
    image = conn.PullDockerImage(IMAGE_NAME, place=PLACE, platform=TARGET_ARCH)
    conn.RemoveDockerImage(name, place=PLACE)
    ExpectException(conn.DockerImageStatus, porto.exceptions.DockerImageNotFound, name, place=PLACE)


# volume
print("Check volumes")
check_storage_is_empty()

image = conn.PullDockerImage(IMAGE_NAME, place=PLACE, platform=TARGET_ARCH)
volume = conn.CreateVolume(image=IMAGE_NAME, layers=[LAYER_NAME], place=PLACE)
ExpectEq(volume.GetProperty("image"), IMAGE_NAME)
Expect(LAYER_NAME not in volume.GetProperty("layers").split(";"))

conn.RemoveDockerImage(image.id, place=PLACE)
ExpectException(conn.DockerImageStatus, porto.exceptions.DockerImageNotFound, image.id, place=PLACE)
volume.Destroy()


# portoctl
print("Check portoctl commands")
check_storage_is_empty()

image = subprocess.check_output([portoctl, "docker-pull", "-P", PLACE, "-T", TARGET_ARCH, IMAGE_NAME]).decode("utf-8")[:-1]
ExpectEq(image, IMAGE_DIGEST)

for mask in ("", IMAGE_TAG[:-1]+"***", IMAGE_TAG):
    images = subprocess.check_output([portoctl, "docker-images", "-P", PLACE, mask]).decode("utf-8").split()
    Expect(image[:12] in images)
    Expect(IMAGE_TAG in images)
    if mask:
        # "ID" + "NAME" + <digest> + <tag> = 4
        ExpectEq(len(images), 4)
        ExpectEq(image[:12], images[2])
        ExpectEq(IMAGE_TAG, images[3])

for name in (IMAGE_NAME, IMAGE_TAG, IMAGE_DIGEST, IMAGE_DIGEST[:12]):
    image = subprocess.check_output([portoctl, "docker-pull", "-P", PLACE, "-T", TARGET_ARCH, IMAGE_NAME]).decode("utf-8")[:-1]
    stdout = subprocess.check_output([portoctl, "docker-rmi", "-P", PLACE, name]).decode("utf-8")
    ExpectEq(stdout, "")
    ExpectException(conn.DockerImageStatus, porto.exceptions.DockerImageNotFound, image, place=PLACE)


# k8s image
print("Check k8s pause image")
check_storage_is_empty()

image = conn.PullDockerImage(K8S_IMAGE_TAG, place=PLACE, platform=TARGET_ARCH)
check_image(image, K8S_IMAGE_DIGEST, [K8S_IMAGE_TAG])

for name in (K8S_IMAGE_TAG, K8S_IMAGE_DIGEST, K8S_IMAGE_DIGEST[:12]):
    image = conn.DockerImageStatus(name, place=PLACE)
    check_image(image, K8S_IMAGE_DIGEST, [K8S_IMAGE_TAG])

for name in (K8S_IMAGE_TAG, K8S_IMAGE_DIGEST, K8S_IMAGE_DIGEST[:12]):
    image = conn.PullDockerImage(K8S_IMAGE_TAG, place=PLACE, platform=TARGET_ARCH)
    conn.RemoveDockerImage(name, place=PLACE)
    ExpectException(conn.DockerImageStatus, porto.exceptions.DockerImageNotFound, name, place=PLACE)

# tag adding
print("Check tag adding")
check_storage_is_empty()

conn.PullDockerImage(K8S_IMAGE_TAG, place=PLACE, platform=TARGET_ARCH)
image = conn.PullDockerImage(K8S_IMAGE_ALT_TAG, place=PLACE, platform=TARGET_ARCH)
check_image(image, K8S_IMAGE_DIGEST, [K8S_IMAGE_TAG, K8S_IMAGE_ALT_TAG])

conn.RemoveDockerImage(K8S_IMAGE_TAG, place=PLACE)
image = conn.DockerImageStatus(K8S_IMAGE_ALT_TAG, place=PLACE)
check_image(image, K8S_IMAGE_DIGEST, [K8S_IMAGE_ALT_TAG])

conn.PullDockerImage(K8S_IMAGE_TAG, place=PLACE, platform=TARGET_ARCH)
image = conn.DockerImageStatus(K8S_IMAGE_TAG, place=PLACE)
check_image(image, K8S_IMAGE_DIGEST, [K8S_IMAGE_TAG, K8S_IMAGE_ALT_TAG])

conn.RemoveDockerImage(K8S_IMAGE_DIGEST, place=PLACE)
ExpectException(conn.DockerImageStatus, porto.exceptions.DockerImageNotFound, K8S_IMAGE_TAG, place=PLACE)
ExpectException(conn.DockerImageStatus, porto.exceptions.DockerImageNotFound, K8S_IMAGE_ALT_TAG, place=PLACE)
ExpectException(conn.DockerImageStatus, porto.exceptions.DockerImageNotFound, K8S_IMAGE_DIGEST, place=PLACE)
check_storage_is_empty()


# OCI mediatypes
print("Check OCI mediatypes")
check_storage_is_empty()

conn.PullDockerImage(UBUNTU_JAMMY_IMAGE_TAG, place=PLACE, platform=TARGET_ARCH)
image = conn.DockerImageStatus(UBUNTU_JAMMY_IMAGE_DIGEST, place=PLACE)
check_image(image, UBUNTU_JAMMY_IMAGE_DIGEST, [UBUNTU_JAMMY_IMAGE_TAG])
conn.RemoveDockerImage(UBUNTU_JAMMY_IMAGE_DIGEST, place=PLACE)

check_storage_is_empty()

# check empty target platform
if is_x86_64():
    print("Check empty target platform")
    image = conn.PullDockerImage(K8S_IMAGE_TAG, place=PLACE)
    check_image(image, K8S_IMAGE_DIGEST, [K8S_IMAGE_TAG])
    conn.RemoveDockerImage(K8S_IMAGE_TAG, place=PLACE)
    check_storage_is_empty()
