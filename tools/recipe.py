import hashlib, os, shutil, subprocess, sys, tarfile, time

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
R = TOP + '/recipes'
OUT = TOP + '/out'
WORK = os.path.abspath(os.environ.get('HERD_WORK', os.path.expanduser('~/.cache/herd-build')))
B = WORK + '/ports'
DL = WORK + '/dl'
STAGE = WORK + '/proot'
CERVUS = os.path.abspath(os.environ.get('CERVUS', TOP + '/../Cervus'))
CROSS = CERVUS + '/usr/cross/tools/bin'


def parse(path):
    f, build, inb = {}, [], False
    for line in open(path):
        line = line.rstrip('\n')
        if inb:
            if line.startswith('  ') or line == '':
                build.append(line[2:])
                continue
            inb = False
        if line.startswith('build: |'):
            inb = True
            continue
        if ':' in line:
            k, v = line.split(':', 1)
            f[k.strip()] = v.strip()
    f['build'] = '\n'.join(build).rstrip() + '\n'
    return f


def run(cmd, cwd, env, log):
    with open(log, 'a') as lf:
        p = subprocess.run(['sh', '-ec', cmd], cwd=cwd, env=env, stdout=lf, stderr=subprocess.STDOUT)
    if p.returncode:
        os.system('tail -40 ' + log)
        sys.exit('recipe: step failed, see ' + log)


def fetch(f, work):
    src = f['source']
    os.makedirs(DL, exist_ok=True)
    if f.get('revision') and not src.endswith(('.tar.gz', '.tar.xz', '.tar.bz2', '.tgz', '.zip')):
        d = work + '/src'
        subprocess.check_call(['git', 'clone', '-q', '--depth', '1', '--branch', f['revision'], src, d])
        return d
    fn = DL + '/' + src.rsplit('/', 1)[1]
    if not os.path.exists(fn):
        subprocess.check_call(['curl', '-sfL', '--retry', '6', '--retry-all-errors', '-o', fn + '.part', src])
        os.rename(fn + '.part', fn)
    got = hashlib.sha256(open(fn, 'rb').read()).hexdigest()
    if f.get('sha256') and got != f['sha256']:
        sys.exit('recipe: sha256 mismatch for %s: %s' % (fn, got))
    x = work + '/x'
    os.makedirs(x)
    if fn.endswith('.zip'):
        import zipfile
        with zipfile.ZipFile(fn) as z:
            for info in z.infolist():
                out = z.extract(info, x)
                mode = info.external_attr >> 16
                if mode & 0o777:
                    os.chmod(out, mode & 0o777)
    else:
        subprocess.check_call(['tar', '-xf', fn, '-C', x])
    ent = os.listdir(x)
    return x + '/' + ent[0] if len(ent) == 1 and os.path.isdir(x + '/' + ent[0]) else x


def hardlinks_to_symlinks(root):
    seen = {}
    for d, _, files in os.walk(root):
        for n in sorted(files):
            p = os.path.join(d, n)
            st = os.lstat(p)
            if not os.path.isfile(p) or os.path.islink(p) or st.st_nlink < 2:
                continue
            key = (st.st_dev, st.st_ino)
            if key not in seen:
                seen[key] = p
                continue
            os.unlink(p)
            os.symlink(os.path.relpath(seen[key], d), p)


def strip(root):
    for d, _, files in os.walk(root):
        for n in files:
            p = os.path.join(d, n)
            if os.path.islink(p) or n.endswith(('.a', '.o')):
                continue
            with open(p, 'rb') as fh:
                if fh.read(4) != b'\x7fELF':
                    continue
            subprocess.call([CROSS + '/x86_64-cervus-strip', '--strip-unneeded', p],
                            stderr=subprocess.DEVNULL)


def pack(f, pkg):
    name, ver = f['name'], f['version']
    file = '%s-%s-x86_64.tar.gz' % (name, ver)
    for o in os.listdir(OUT):
        if o.endswith('.manifest'):
            txt = open(OUT + '/' + o).read()
            if ('\nname: %s\n' % name) in '\n' + txt and o != file.replace('.tar.gz', '.manifest'):
                os.unlink(OUT + '/' + o)
                t = OUT + '/' + o.replace('.manifest', '.tar.gz')
                if os.path.exists(t):
                    os.unlink(t)
    subprocess.check_call(['tar', '-C', pkg, '--owner=0', '--group=0', '--numeric-owner',
                           '-czf', OUT + '/' + file, '.'])
    data = open(OUT + '/' + file, 'rb').read()
    m = ['name: ' + name, 'version: ' + ver, 'arch: x86_64', 'size: %d' % len(data),
         'sha256: ' + hashlib.sha256(data).hexdigest(), 'file: ' + file,
         'depends: ' + f.get('depends', 'libc'), 'abi: 1',
         'built: ' + time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
         'summary: ' + f['summary'], 'license: ' + f['license']]
    open(OUT + '/' + file.replace('.tar.gz', '.manifest'), 'w').write('\n'.join(m) + '\n')
    print('%s %d bytes' % (file, len(data)))


def main():
    name = sys.argv[1]
    f = parse('%s/%s/recipe' % (R, name))
    work = B + '/' + name
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    os.makedirs(OUT, exist_ok=True)
    log = work + '/build.log'
    src = fetch(f, work)
    for p in f.get('patches', '').split():
        subprocess.check_call(['patch', '-s', '-p1', '-i', '%s/%s/patches/%s' % (R, name, p)], cwd=src)
    pkg = work + '/pkg'
    os.makedirs(pkg)
    os.makedirs(STAGE, exist_ok=True)
    env = dict(os.environ)
    if not os.path.exists(CROSS + '/x86_64-cervus-gcc'):
        sys.exit('recipe: no cross compiler in %s (set CERVUS to the Cervus checkout and run builder/build_cross_toolchain.sh)' % CROSS)
    env.update(PATH=CROSS + ':' + env['PATH'],
               RECIPEDIR='%s/%s' % (R, name), PKGDIR=pkg, STAGING=STAGE, DEPDIR=STAGE,
               WORKDIR=work, HERD_CACHE=DL, PKG_CONFIG_LIBDIR=STAGE + '/usr/lib/pkgconfig:' + STAGE + '/usr/share/pkgconfig',
               PKG_CONFIG_SYSROOT_DIR=STAGE)
    t0 = time.time()
    run(f['build'], src, env, log)
    hardlinks_to_symlinks(pkg)
    strip(pkg)
    subprocess.check_call(['cp', '-a', pkg + '/.', STAGE + '/'])
    if '--no-pack' not in sys.argv:
        pack(f, pkg)
    print('built %s in %ds' % (name, time.time() - t0))


main()
