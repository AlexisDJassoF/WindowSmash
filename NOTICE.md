# Third-party notices

[← README](README.md)

## WindowSmash's own source

No project-wide license has been specified for the WindowSmash source outside the separately identified third-party material below.

## libvpx

[`runtime/gpu_vp9_tables.h`](runtime/gpu_vp9_tables.h) contains default VP9 probability tables derived from libvpx, as its header states. The upstream WebM Project license and additional patent grant are kept in [`third_party/libvpx/`](third_party/libvpx/):

- [BSD license and copyright notice](third_party/libvpx/LICENSE)
- [Additional IP rights grant](third_party/libvpx/PATENTS)

These notice files come from the upstream `webmproject/libvpx` repository at tag `v1.14.1`.

## libopus

The libopus 1.3.1 notice is kept at [`third_party/opus/COPYING`](third_party/opus/COPYING), for builds and runtime packages that statically link that codec.

## SDL3

Controller input can use [SDL3](https://www.libsdl.org/) (Simple DirectMedia Layer), copyright Sam Lantinga, under the [zlib license](https://github.com/libsdl-org/SDL/blob/main/LICENSE.txt).

SDL3 is an external dependency: this repository contains no SDL source or binaries. The runtime is compiled against the headers of the official SDL3 Windows development package (`deps/SDL3`, or `SSBU_SDL3_DIR`) and loads `SDL3.dll` from the executable's folder at run time. A package that includes `SDL3.dll` should include that package's `LICENSE.txt` too; the local bundler (`tools/bundle_ssbu.py`) adds it as `build/SDL3-LICENSE.txt`.

## Other external dependencies

These are external dependencies, not bundled implementations. Their own licenses apply when you obtain, link or distribute them.

| Dependency | Used for | How it's obtained |
| :--- | :--- | :--- |
| libopus | Music (Opus decoding) | Optional static libraries, named by `SSBU_OPUS_LIBS` at build time. |
| libvpx | Movies (VP9 decoding) | Optional, named by `SSBU_VPX_DIR` at build time. |
| SDL3 | Controllers other than Xbox pads | Optional, the official Windows development package. |
| PyCryptodome (`pycryptodomex`) | Reading game packages | `requirements.txt`. |
| Capstone, Unicorn | Disassembly and differential tests | `requirements-dev.txt`, optional. |

No codec binaries are checked into this repository.

## Game content and names

The Git source tree contains no Nintendo game binaries, translated game functions, assets, fonts, keys, firmware or personal saves. Runtime package assets contain the precompiled executable and relocated module images; the original game-data archives, firmware fonts, keys and saves remain separate inputs.

Super Smash Bros. Ultimate and associated names are the property of their respective owners. WindowSmash is an independent project and is not affiliated with Nintendo.
