#!/usr/bin/env python3
"""Put linux-toolchain/ together on a Linux machine (Ubuntu, Debian or the like): everything
third-party the Linux build needs that is not an apt package of the build machine itself.

Usage: fetch_toolchain.py [--list]

It makes, in linux-toolchain/ next to this checkout (or the folder SSBU_LINUX_TOOLCHAIN names),
the layout tools/linux/toolchain.py and README-LINUX.md describe:
  sysroot/        Ubuntu 22.04 (jammy) amd64 packages, each .deb's files extracted with dpkg-deb
  dxvk-native/    DXVK's native Linux build
  dxvk-headers/   DXVK's include/native, with mingw-directx-headers in its directx/ folder
  src/vkd3d-*/    the vkd3d source
  src/SPIRV-Headers-*/
  src/Vulkan-Headers-*/
Every download is checked before it is used. The jammy packages come from one fixed moment of
snapshot.ubuntu.com (SNAPSHOT), so the sysroot is the same on every machine: the pocket's
InRelease is checked with gpgv against the Ubuntu archive keyring, the Packages index against
the hash the checked InRelease gives, and each .deb against the hash the index gives. The other
parts are checked against the SHA256 fixed below. A part that is already there, from the same
inputs, is left alone; a part is extracted next to its place and moved there when complete,
so a failure never leaves half of one. Downloads are kept in linux-toolchain/downloads.

Needs: Python 3.10+, dpkg-deb (dpkg) and gpgv (gpgv), both part of any Ubuntu. --list prints
what would be fetched."""
import hashlib
import lzma
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
# (as toolchain.py, which this does not import: that one wants the compilers, installed or not yet)
TOOLCHAIN = os.environ.get("SSBU_LINUX_TOOLCHAIN") or os.path.join(os.path.dirname(REPO), "linux-toolchain")
DOWNLOADS = os.path.join(TOOLCHAIN, "downloads")
MARKER = ".fetched"

# The sysroot: what the program is compiled and linked against. Linking against jammy's glibc 2.35
# keeps it runnable on Ubuntu 22.04, Debian 12 and anything newer. A later snapshot is a deliberate
# change: it updates every package below at once.
SNAPSHOT = "20261001T000000Z"
UBUNTU = f"https://snapshot.ubuntu.com/ubuntu/{SNAPSHOT}"
SUITES = ("jammy-updates", "jammy")   # a package's newest version first: an update's
COMPONENTS = ("main", "universe")
KEYRING = "/usr/share/keyrings/ubuntu-archive-keyring.gpg"
SYSROOT_PACKAGES = ("libc6", "libc6-dev", "linux-libc-dev", "libgcc-s1", "libgcc-11-dev", "libstdc++6", "libstdc++-11-dev",
                    "libsdl2-2.0-0", "libsdl2-dev", "libopus-dev", "libvpx-dev")

GITHUB = "https://github.com"
# (folder in linux-toolchain, url, sha256, the archive's folder to take, where it goes inside the folder)
RELEASES = (
    ("dxvk-native", f"{GITHUB}/doitsujin/dxvk/releases/download/v3.1.1/dxvk-native-3.1.1-steamrt-sniper.tar.gz",
     "506fa1ea5bb6ce9a89e11436f7ab5191bc0196e8a577e0cf0b16a45bbc9991b7", "", ""),
    ("dxvk-headers", f"{GITHUB}/doitsujin/dxvk/archive/refs/tags/v3.1.1.tar.gz",
     "ef4944134f6c39d0937f265391db2d08bca33cd3a2f0ee9133e1eb55f1cc657c", "dxvk-3.1.1/include/native", "include/native"),
    # (DXVK 3.1.1's include/native/directx submodule, at the commit that release uses)
    ("dxvk-headers", f"{GITHUB}/Joshua-Ashton/mingw-directx-headers/archive/9df86f2341616ef1888ae59919feaa6d4fad693d.tar.gz",
     "9c1b8bbfd2d6c758fac4d93c2808a46bc1eee429fe656b4f21b29e02615aa8aa",
     "mingw-directx-headers-9df86f2341616ef1888ae59919feaa6d4fad693d", "include/native/directx"),
    ("src/vkd3d-2.1", "https://dl.winehq.org/vkd3d/source/vkd3d-2.1.tar.xz",
     "7510146aff2adfb4ae07ab890701a607e5ff7c66e57100cfc9f630ec92eeda6a", "vkd3d-2.1", ""),
    ("src/SPIRV-Headers-vulkan-sdk-1.4.363.0", f"{GITHUB}/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz",
     "5b3a0d099f5c7649e29c0938476c462e71d66ad89a0bde42fa1ba3eb476956c9", "SPIRV-Headers-vulkan-sdk-1.4.363.0", ""),
    # (the Vulkan renderer's API, of the same Vulkan SDK as SPIRV-Headers; libvulkan itself is loaded at run time)
    ("src/Vulkan-Headers-vulkan-sdk-1.4.363.0", f"{GITHUB}/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz",
     "4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e", "Vulkan-Headers-vulkan-sdk-1.4.363.0", ""),
)


class Failed(Exception):
    pass


def log(text):
    print(text, flush=True)


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def download(url, sha256, name=None):
    """The file at url, in downloads/, checked against sha256 (None: not checked here - the caller
    checks it, as with an InRelease). A kept download that matches is not fetched again; one that
    does not match is deleted."""
    os.makedirs(DOWNLOADS, exist_ok=True)
    path = os.path.join(DOWNLOADS, name or url.rstrip("/").rsplit("/", 1)[-1])
    if sha256 and os.path.isfile(path) and sha256_of(path) == sha256:
        return path
    log(f"  downloading {url}")
    request = urllib.request.Request(url, headers={"User-Agent": "WindowSmash fetch_toolchain"})
    partial = path + ".part"
    try:
        with urllib.request.urlopen(request, timeout=120) as response, open(partial, "wb") as f:
            shutil.copyfileobj(response, f, 1 << 20)
    except OSError as e:
        raise Failed(f"{url}: {e}")
    if sha256 and sha256_of(partial) != sha256:
        os.remove(partial)
        raise Failed(f"{url}: not the expected file (SHA256 differs): nothing was extracted")
    os.replace(partial, path)
    return path


def read_marker(folder):
    try:
        with open(os.path.join(folder, MARKER), encoding="utf-8") as f:
            return f.read()
    except OSError:
        return None


def install(folder, identity, fill):
    """Make linux-toolchain/<folder> by fill(staging folder) unless it is there from the same
    inputs (its marker says identity); the staging folder replaces it only once fill is done."""
    target = os.path.join(TOOLCHAIN, folder)
    if read_marker(target) == identity:
        log(f"{folder}: up to date")
        return False
    parent = os.path.dirname(target)
    os.makedirs(parent, exist_ok=True)
    staging = tempfile.mkdtemp(prefix=os.path.basename(target) + ".", suffix=".partial", dir=parent)
    try:
        fill(staging)
        with open(os.path.join(staging, MARKER), "w", encoding="utf-8") as f:
            f.write(identity)
        if os.path.lexists(target):
            shutil.rmtree(target)
        os.replace(staging, target)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    log(f"{folder}: done")
    return True


# ---------------------------------------------------------------- the sysroot

def verified_release(suite):
    """The signed content of a pocket's InRelease, once gpgv has checked it (only the signed part:
    whatever else the file holds is not read)."""
    if not os.path.isfile(KEYRING):
        raise Failed(f"{KEYRING} is missing: install ubuntu-keyring")
    # (a snapshot's InRelease never changes: a kept one is used again, and checked again)
    path = os.path.join(DOWNLOADS, f"{SNAPSHOT}-{suite}-InRelease")
    if not os.path.isfile(path):
        download(f"{UBUNTU}/dists/{suite}/InRelease", None, os.path.basename(path))
    out = path + ".verified"
    r = subprocess.run(["gpgv", "--keyring", KEYRING, "--output", out, path], capture_output=True, text=True,
                       env={**os.environ, "LC_ALL": "C"})
    if r.returncode:
        os.remove(path)
        raise Failed(f"{suite}'s InRelease is not signed by the Ubuntu archive:\n{r.stderr.strip()}")
    with open(out, encoding="utf-8") as f:
        return f.read()


def release_sha256(release, name):
    """The SHA256 a checked Release gives for one of its files (e.g. main/binary-amd64/Packages.xz)."""
    section = release.split("\nSHA256:\n", 1)[-1] if "\nSHA256:\n" in release else ""
    for line in section.splitlines():
        if not line.startswith(" "):
            break
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return parts[0]
    raise Failed(f"the Release file has no SHA256 for {name}")


def parse_packages(text):
    """{package: {field: value}} from a Packages index (the first stanza of a name wins)."""
    found = {}
    for stanza in text.split("\n\n"):
        fields = dict(re.findall(r"^([\w-]+): (.*)$", stanza, re.M))
        if "Package" in fields:
            found.setdefault(fields["Package"], fields)
    return found


def resolve_packages():
    """[(package, version, url, sha256)] for SYSROOT_PACKAGES: each from the first pocket that has it."""
    indexes = []
    for suite in SUITES:
        release = verified_release(suite)
        for component in COMPONENTS:
            name = f"{component}/binary-amd64/Packages.xz"
            path = download(f"{UBUNTU}/dists/{suite}/{name}", release_sha256(release, name),
                            f"{SNAPSHOT}-{suite}-{component}-Packages.xz")
            with lzma.open(path, "rt", encoding="utf-8") as f:
                indexes.append(parse_packages(f.read()))
    chosen = []
    for package in SYSROOT_PACKAGES:
        fields = next((index[package] for index in indexes if package in index), None)
        if not fields:
            raise Failed(f"{package} is in none of {', '.join(SUITES)} ({', '.join(COMPONENTS)})")
        chosen.append((package, fields["Version"], f"{UBUNTU}/{fields['Filename']}", fields["SHA256"]))
    return chosen


def keep_links_inside(root):
    """A package's absolute symbolic links (/usr/lib/... -> /lib/...) point into the build
    machine's own system once extracted: point them at the same place in the sysroot instead."""
    for folder, dirs, files in os.walk(root):
        for name in dirs + files:
            path = os.path.join(folder, name)
            if os.path.islink(path) and os.readlink(path).startswith("/"):
                target = os.path.join(root, os.readlink(path).lstrip("/"))
                os.remove(path)
                os.symlink(os.path.relpath(target, folder), path)


def fetch_sysroot(packages):
    identity = f"snapshot {SNAPSHOT}\n" + "".join(f"{p} {v}\n" for p, v, _u, _s in packages)

    def fill(staging):
        for package, version, url, sha256 in packages:
            deb = download(url, sha256)
            r = subprocess.run(["dpkg-deb", "-x", deb, staging], capture_output=True, text=True)
            if r.returncode:
                raise Failed(f"{package} {version}: dpkg-deb could not extract it:\n{r.stderr.strip()}")
        keep_links_inside(staging)
    return install("sysroot", identity, fill)


# ---------------------------------------------------------------- the releases

def extract(archive, inner, dest):
    """The archive's folder inner ('' for all of it) into dest, refusing anything that would land
    outside dest."""
    with tarfile.open(archive) as tar:
        prefix = inner.rstrip("/") + "/" if inner else ""
        members = []
        for m in tar.getmembers():
            if prefix and not m.name.startswith(prefix):
                continue
            name = m.name[len(prefix):]
            if not name:
                continue
            if name.startswith("/") or ".." in name.split("/"):
                raise Failed(f"{archive}: unsafe path {m.name}")
            m.name = name
            if m.islnk():   # (a hard link names another member: the same prefix comes off it)
                m.linkname = m.linkname[len(prefix):]
            members.append(m)
        if not members:
            raise Failed(f"{archive}: no {inner or 'files'} in it")
        os.makedirs(dest, exist_ok=True)
        kwargs = {"filter": "data"} if hasattr(tarfile, "data_filter") else {}
        tar.extractall(dest, members, **kwargs)


def archive_name(url):
    """The download's name: a GitHub source archive's carries its repository (two of them can both
    be vulkan-sdk-1.4.363.0.tar.gz); anything else keeps its own."""
    m = re.match(r"https://github\.com/[^/]+/([^/]+)/archive/(?:refs/tags/)?(.+)$", url)
    return f"{m.group(1)}-{m.group(2)}" if m else None


def fetch_releases():
    changed = False
    for folder in dict.fromkeys(r[0] for r in RELEASES):
        parts = [r for r in RELEASES if r[0] == folder]
        identity = "".join(f"{url} {sha256}\n" for _f, url, sha256, _i, _w in parts)

        def fill(staging, parts=parts):
            for _f, url, sha256, inner, where in parts:
                extract(download(url, sha256, archive_name(url)), inner, os.path.join(staging, where))
        changed |= install(folder, identity, fill)
    return changed


def main():
    if not sys.platform.startswith("linux"):
        raise SystemExit("fetch_toolchain.py makes linux-toolchain/ on a Linux machine; on Windows, README-LINUX.md says "
                         "where each part comes from")
    missing = [tool for tool in ("dpkg-deb", "gpgv") if not shutil.which(tool)]
    if missing:
        raise SystemExit(f"{', '.join(missing)} not found: sudo apt install dpkg gpgv")
    log(f"linux-toolchain: {TOOLCHAIN}")
    try:
        packages = resolve_packages()
        if "--list" in sys.argv:
            for package, version, url, _sha in packages:
                log(f"  {package} {version}  {url}")
            for folder, url, _sha, _i, _w in RELEASES:
                log(f"  {folder}  {url}")
            return 0
        fetch_sysroot(packages)
        fetch_releases()
    except Failed as e:
        log(f"\nFAILED: {e}")
        return 1
    log("\nlinux-toolchain is ready. Next: python3 tools/linux/build_vkd3d.py linux")
    return 0


if __name__ == "__main__":
    sys.exit(main())
