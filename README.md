# herd-packs

Package repository for [Cervus](https://github.com/VeoQeo/Cervus), served to
`herd` on the target machine.

- `recipes/<name>/recipe` — where the upstream source lives, its checksum, and
  how to build it. No upstream source and no binaries are committed here.
- `tools/sign.c` — signs the index with the repository's Ed25519 key. Build it
  against the Cervus tree: see the header of the file.

Built packages and the signed `INDEX` are attached to the release tagged
`index`, which is what clients fetch:

    repo=https://github.com/VeoQeoOrg/herd-packs/releases/download/index

A client verifies `INDEX` against `/etc/herd.pub` and refuses it otherwise;
each package is then trusted by matching its sha256 against that signed index.
