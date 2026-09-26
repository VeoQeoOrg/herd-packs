# herd-packs

Packages for [Cervus](https://github.com/VeoQeo/Cervus), installed with `herd`.

Cervus is a from-scratch operating system. It is not Linux, so ordinary Linux
binaries do not run on it — every program has to be compiled for Cervus. This
repository is where those compiled programs come from.

Two things live here:

- **`recipes/<name>/recipe`** — how to build a program: where its source comes
  from, its checksum, any patches, and the build commands. Upstream source is
  never committed here, only the URL and the hash.
- **The `index` release** — the built `.tar.gz` packages plus a signed `INDEX`
  describing them. This is what `herd` downloads.
- **`tools/accept`** — how a package someone sent with `herd add` is checked
  and published.

---

## For users: installing a package

On a Cervus machine:

```sh
herd update            # download and verify the package list
herd search nasm       # find something
herd install nasm      # install it, and anything it needs
nasm -v
herd list              # what is installed
herd remove nasm       # take it away again
```

If you want to watch the download, `herd --progress=pacman install nasm`.
Styles are `bar`, `pacman`, `hash`, `dots`, `percent`, `none`.

### Pointing herd at this repository

A stock Cervus image already has this configured. If yours does not, or you run
your own repository, two files do it:

```sh
echo 'repo=https://github.com/VeoQeoOrg/herd-packs/releases/download/index' > /etc/herd.conf
echo 'c9ae3a5c10fa55b9392a0b44b4a0c9186685f1f654f975a7e775b7663260204d' > /etc/herd.pub
```

`/etc/herd.pub` is the public key this repository's index is signed with. `herd`
**refuses** an index whose signature does not match it, so packages cannot be
swapped out underneath you by whoever is serving the files. Each package is then
checked against the sha256 in that signed index.

---

## Sending your own package, straight from Cervus

Built something that runs on Cervus? Send it to the repository from the Cervus
machine itself:

```sh
herd add ./hello                 # a program or a #! script
herd add hello-1.0.tar.gz        # an archive laid out like / (usr/bin/..., usr/share/...)
herd add ./stage                 # a directory laid out the same way
```

`herd add` looks at what you gave it, then asks a few questions — Enter keeps
the suggestion in brackets:

| Question    | What goes there                                                     |
|-------------|---------------------------------------------------------------------|
| Name        | what people type in `herd install`: lowercase, digits, `+ - . _`    |
| Version     | e.g. `1.0`, `2.3.1`                                                 |
| Summary     | one line: what it is                                                |
| License     | an SPDX name: `MIT`, `GPL-3.0-or-later`, `Apache-2.0`, ...          |
| Homepage    | where the source lives (optional)                                   |
| Description | anything the reviewer should know (optional)                        |
| Depends     | filled in for you from the libraries and `#!` interpreters it uses  |
| Installs as | for a single file: where it goes, `/usr/bin/NAME` by default        |

It builds the `.tar.gz` and its manifest, shows them, and asks:

```
[s]ubmit for review, [w]rite the files here, [q]uit:
```

**Submit** opens a pull request on GitHub under your account, with the package
in `submissions/NAME/VERSION/`, and prints its link. The package is then
**under review**: a maintainer checks it and publishes it, and from that moment
everyone can `herd update && herd install NAME`. **Write** saves the two files
in the current directory instead, if you would rather send them some other way.

The first time you submit, herd asks for a GitHub token. Make a *classic* token
with only the `public_repo` scope at
<https://github.com/settings/tokens/new?scopes=public_repo&description=herd>
and paste it; herd keeps it in `~/.config/herd/token` (readable only by you)
and uses it for nothing but the submission. `$HERD_GITHUB_TOKEN` overrides the
file. herd forks this repository for you if you cannot push to it.

herd refuses Linux programs — they do not run on Cervus — and warns about
libraries the program needs that nothing on your system provides; ship those
in the package (give a directory or an archive) or it will not start elsewhere.
Packages over 40 MB cannot be sent this way; write the files and open the pull
request by hand, linking to where they can be downloaded.

---

## For maintainers: adding a package

You need a Linux machine. Everything below happens there, not on Cervus.

### 1. Build the cross compiler, once

Cervus programs are built with a compiler that targets Cervus rather than
Linux. Your system `gcc` will not do: it produces Linux binaries. Build the
cross toolchain from the Cervus tree:

```sh
git clone https://github.com/VeoQeo/Cervus
cd Cervus
sh builder/build_cross_toolchain.sh      # downloads and builds gcc + binutils
```

This takes a while and lands in `usr/cross/tools`. Put it on your `PATH` and
check it:

```sh
export PATH=$PWD/usr/cross/tools/bin:$PATH
x86_64-cervus-gcc -dumpmachine           # must print: x86_64-cervus
```

If it prints `x86_64-pc-linux-gnu` you are still using the system compiler.

### 2. Build the program

Unpack the upstream source and build it with `--host=x86_64-cervus`, installing
into a staging directory rather than onto your own machine:

```sh
export CC=x86_64-cervus-gcc AR=x86_64-cervus-ar RANLIB=x86_64-cervus-ranlib

./configure --host=x86_64-cervus --prefix=/usr
make
make DESTDIR=/tmp/stage install
```

`DESTDIR` is the important part. `/tmp/stage` now looks like the root of a
Cervus system: `/tmp/stage/usr/bin/nasm` becomes `/usr/bin/nasm` once
installed. Nothing outside it is packaged.

Strip the binaries — debug information can be most of the download:

```sh
x86_64-cervus-strip /tmp/stage/usr/bin/*
```

**Meson projects.** Much of the graphics stack (Wayland, libinput, Weston)
builds with Meson. `recipes/meson-cross.ini` describes the Cervus target; it
needs one value, `staging`, the directory where the packages this one depends on
have been unpacked, so that `pkg-config` and the compiler find their headers and
libraries. Recipes write it into a second cross file:

```sh
printf "[constants]\nstaging = '%s'\n" "$STAGING" > constants.ini
meson setup build --cross-file ../meson-cross.ini --cross-file constants.ini \
    --prefix=/usr --libdir=lib --buildtype=release
ninja -C build
DESTDIR=/tmp/stage meson install -C build --no-rebuild
```

Libraries are built shared. A program that loads them links as a
position-independent executable (`-pie`, which the Cervus compiler turns into a
dynamically linked program using `/lib/ld-cervus.elf`), and a program that
`dlopen`s plugins must export the whole C library to them
(`-Wl,--whole-archive -lcervus_pic -Wl,--no-whole-archive`), as Weston's patch
does.

**Two things commonly go wrong.**

*`configure` says `OS 'cervus' not recognized`.* The program ships its own
`config.sub`, which has a list of operating systems it knows, and Cervus is not
in it. Add it, and keep the change as a patch in your recipe — see
`recipes/nasm/patches/` for exactly this.

*The compiler rejects a function you know exists in Cervus.* gcc keeps a frozen
copy of some headers from when the toolchain was built, and it shadows the real
ones. Delete them:

```sh
rm -f usr/cross/tools/lib/gcc/x86_64-cervus/*/include-fixed/std*.h
```

If the function genuinely is missing from Cervus, that is a bug in Cervus, not
in the program — report it, or fix it there and rebuild.

### 3. Make the package

A package is a gzipped tar of the staging directory:

```sh
mkdir -p ~/out
cd /tmp/stage
tar -czf ~/out/nasm-2.16.03-x86_64.tar.gz .
```

### 4. Describe it

Every package has a manifest. Write `~/out/nasm-2.16.03-x86_64.manifest`:

```
name: nasm
version: 2.16.03
arch: x86_64
size: 470619
sha256: 25c9e7afb55353e18e6aa289e1908f68f57fd584bd938c4229dd32d89db07f54
file: nasm-2.16.03-x86_64.tar.gz
depends: libc
summary: the Netwide Assembler, an x86 assembler
license: BSD-2-Clause
```

`size` and `sha256` must match the tarball exactly — `herd` refuses it
otherwise:

```sh
wc -c < ~/out/nasm-2.16.03-x86_64.tar.gz
sha256sum ~/out/nasm-2.16.03-x86_64.tar.gz
```

`depends` lists other package names, separated by spaces. `libc` means the base
system and is always satisfied.

### 5. Send a pull request

Commit only the recipe and its patches — never the tarball:

```
recipes/nasm/recipe
recipes/nasm/patches/0001-config.sub-recognise-cervus.patch
```

Open a pull request. Attach the built tarball and its manifest to the PR, or say
where they can be fetched from, so a maintainer can reproduce and publish them.

---

## For the repository owner: publishing

Only someone with the private key can publish, because the index is signed.

### Making the key, once

```sh
mkdir -p ~/cervus-keys && chmod 700 ~/cervus-keys
openssl genpkey -algorithm ed25519 -out ~/cervus-keys/herd.pem
chmod 600 ~/cervus-keys/herd.pem
openssl pkey -in ~/cervus-keys/herd.pem -pubout -outform DER | tail -c 32 | xxd -p -c32
```

The hex it prints is what goes in `/etc/herd.pub` on every Cervus install.
**Back up `herd.pem` somewhere safe.** If it leaks, anyone can publish packages
that every Cervus machine will trust. If you lose it, you cannot publish updates
and every user has to be given a new `/etc/herd.pub` by hand — there is no
revocation.

### Publishing a release

Collect every manifest into one index, separated by blank lines, sign it, and
upload:

```sh
cd ~/out
awk 'FNR==1 && NR!=1 {print ""} {print}' *.manifest > INDEX
openssl pkeyutl -sign -inkey ~/cervus-keys/herd.pem -rawin -in INDEX -out INDEX.sig

gh release upload index *.tar.gz INDEX INDEX.sig \
   --clobber --repo VeoQeoOrg/herd-packs
```

Nothing else is needed: `herd` fetches `INDEX` and `INDEX.sig` from that release
and takes it from there. Re-sign and re-upload the index every time a package
changes, or the checksums will no longer match.

### Reviewing submissions from `herd add`

```sh
tools/accept --list          # submissions waiting for review
tools/accept 42              # check pull request #42 and publish it
```

`tools/accept` needs `gh` (logged in), `openssl`, and the key at
`~/cervus-keys/herd.pem` (or `$HERD_KEY`). It refuses a pull request that
changes anything outside `submissions/`, a manifest whose name, version, size
or sha256 do not match the tarball, an archive with absolute or `..` paths, and
a package that would overwrite files of another package. It shows the manifest
and the file list and asks before publishing.

Publishing takes the index that is live now — after checking its signature —
replaces or adds the accepted records, signs it with your key **on your
machine**, uploads the package, `INDEX` and `INDEX.sig`, and closes the pull
request with a note telling the author it is out. The key never leaves your
machine and nothing is merged into git: packages live in the `index` release,
not in the repository. A copy lands in `out/` too, so the next `./nb upload`
from the Cervus tree keeps it.

Look at what the program does before accepting it, the same as any pull
request: the signature only proves that you published it.

---

## A note on where this is going

Today packages are cross-compiled on Linux, the way Alpine did for years. The
goal is for Cervus to build them itself: `configure` and `make` already run
natively on Cervus, so the remaining piece is a compiler that runs there too.
When that lands a recipe will build on the machine it installs to, and the
`--host=x86_64-cervus` step, along with patches like the `config.sub` one,
stops being necessary.

---

## License

Apache License 2.0 — see [LICENSE](LICENSE).
