# Attribution and licensing

No project-wide license has been specified for the WindowSmash source outside the separately identified third-party material below.

## libvpx

[`runtime/gpu_vp9_tables.h`](runtime/gpu_vp9_tables.h) contains default VP9 probability tables derived from libvpx. The source header identifies this attribution. The upstream WebM Project license and additional patent grant are preserved under [`third_party/libvpx/`](third_party/libvpx/):

- [BSD license and copyright notice](third_party/libvpx/LICENSE)
- [Additional IP rights grant](third_party/libvpx/PATENTS)

The notice files are from the upstream `webmproject/libvpx` repository at tag `v1.14.1`.

## Optional external dependencies

The libopus 1.3.1 notice is preserved at [`third_party/opus/COPYING`](third_party/opus/COPYING) for builds and runtime releases that statically link that codec.

libopus, libvpx, PyCryptodome, Capstone, and Unicorn are external dependencies, not bundled implementations. Their own licenses apply when you obtain, link, or distribute them. The optional native codecs are selected through build environment variables; no codec binaries are checked into this repository.

## Game content and names

The Git source tree contains no Nintendo game binaries, translated game functions, assets, fonts, keys, firmware, or personal saves. Runtime release assets contain the precompiled executable and relocated module images; original game-data archives, firmware fonts, keys, and saves remain separate inputs. Super Smash Bros. Ultimate and associated names are the property of their respective owners. WindowSmash is an independent project and is not affiliated with Nintendo.
