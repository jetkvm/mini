"""A release has the configuration of the committed defaults and the UI values of the committed env files,
whatever the checkout built before."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

TOOLS = Path(__file__).resolve().parents[1]
REPOSITORY = TOOLS.parent
sys.path.insert(0, str(TOOLS))

import mini_image  # noqa: E402
import release_config  # noqa: E402

VERSION = '0.5.1-dev202610090000'
SKUS = {'ethernet': 'jetkvm-mini-ethernet', 'wifi': 'jetkvm-mini-wireless'}
TEST_CA = b'JetKVM Mini e2e mock cloud test CA'

# STAND_IN_CALLS names a file to which each stand-in appends its name and
# arguments, one call per line.
# git answers a clean tree at a commit without a release tag; with
# GIT_HEAD_ON, HEAD is on only the refs that it lists. With GIT_FETCH_FAILS=1
# each fetch exits with an error. tag -l lists the tags
# of GIT_TAGS, and log --oneline prints one change followed by the range it
# was asked for. rclone keeps a
# bucket in a directory: lsf lists its folders, copy puts the files of the
# stage there, largest first, and check compares the two. RCLONE_STOPS_AT
# names a file at which a copy stops with an error, and RCLONE_LOSES a file
# that a copy leaves out without one. npm writes a UI bundle. docker
# runs the command after the image name on the host. idf.py stands in for
# ESP-IDF and the project's CMakeLists.txt:
#   - the project configuration file is -DSDKCONFIG, or sdkconfig in the
#     project directory without it;
#   - the options come from sdkconfig.defaults (or the -DSDKCONFIG_DEFAULTS
#     list in its place), the board variant's file and, with a mock cloud URL,
#     sdkconfig.mockcloud.defaults; a value in an existing configuration file
#     wins over them, as in ESP-IDF;
#   - the configure step records what it used in CMakeCache.txt,
#     project_description.json and config/sdkconfig.json;
#   - the image holds its version, its SKU, the configuration and the CMake
#     options, the fingerprint of the release key, and for a mock cloud
#     build the name of the e2e test CA (with the custom certificate bundle)
#     and the fingerprint of the test release key.
SHIM = r'''#!/usr/bin/env python3
import json,os,struct,subprocess,sys
from pathlib import Path
name=Path(sys.argv[0]).name;args=sys.argv[1:];root=Path(os.environ['CONTROL_ROOT'])
if os.environ.get('STAND_IN_CALLS'):
    with open(os.environ['STAND_IN_CALLS'],'a') as calls: calls.write(' '.join([name,*args])+'\n')
def options(path):
    found={}
    for line in path.read_text().splitlines():
        if line.startswith('CONFIG_') and '=' in line:
            key,value=line[7:].split('=',1)
            found[key]={'y':True,'n':False}.get(value,value.strip('"'))
    return found
if name=='git':
    if args[:1]==['fetch'] and os.environ.get('GIT_FETCH_FAILS')=='1': sys.exit(1)
    if '--verify' in args: sys.exit(1)
    if '--is-ancestor' in args and 'GIT_HEAD_ON' in os.environ:
        sys.exit(0 if args[-1] in os.environ['GIT_HEAD_ON'].split() else 1)
    if '--git-common-dir' in args: print(root/'.git')
    elif 'rev-parse' in args: print('abcdef1')
    elif args[:2]==['tag','-l']:
        for tag in os.environ.get('GIT_TAGS','').split(): print(tag)
    elif args[:2]==['log','--oneline']: print('abcdef1 a change',args[-1])
elif name=='rclone':
    import fnmatch,shutil
    if args[0]=='lsf':
        print(''.join(d.name+'/\n' for d in sorted(Path(args[-1]).glob('*/'))),end='')
        sys.exit(0)
    source,bucket=Path(args[-2]),Path(args[-1])
    pattern=args[args.index('--include')+1] if '--include' in args else '*'
    files=[f for f in source.rglob('*') if f.is_file() and fnmatch.fnmatch(f.name,pattern)]
    for file in sorted(files,key=lambda f:(-f.stat().st_size,f)):
        there=bucket/file.relative_to(source)
        if args[0]=='check':
            if not there.is_file() or there.read_bytes()!=file.read_bytes(): sys.exit(1)
        elif args[0]=='copy':
            if file.name==os.environ.get('RCLONE_STOPS_AT'): sys.exit(1)
            if file.name==os.environ.get('RCLONE_LOSES'): continue
            there.parent.mkdir(parents=True,exist_ok=True);shutil.copy(file,there)
    sys.exit(0 if args[0] in ('check','copy') else 1)
elif name=='npm':
    out=Path(args[args.index('--outDir')+1]);out.mkdir(parents=True,exist_ok=True)
    (out/'index.html').write_text('ui')
elif name=='docker':
    i=1;cwd=None
    while args[i].startswith('-'):
        if args[i]=='-w': cwd=args[i+1]
        i+=1 if args[i]=='--rm' else 2
    sys.exit(subprocess.run(args[i+1:],cwd=cwd).returncode)
elif name=='idf.py':
    import re
    def fingerprint(file,name): return re.search(name+r' "([0-9A-F]{40})"',(Path.cwd()/'components/jetkvm_ota'/file).read_text()).group(1).encode()
    project=Path.cwd();build=Path(args[args.index('-B')+1]);build.mkdir(parents=True,exist_ok=True)
    defs=dict(a[2:].split('=',1) for a in args if a.startswith('-D'))
    variant=defs['JETKVM_BOARD_VARIANT'];mock=defs.get('JETKVM_MOCK_CLOUD_JWKS_URL','')
    listed=[Path(item) for item in defs.get('SDKCONFIG_DEFAULTS','').split(';') if item]
    defaults=(listed or [project/'sdkconfig.defaults'])+[project/f'sdkconfig.{variant}.defaults']
    if mock: defaults.append(project/'sdkconfig.mockcloud.defaults')
    config={}
    for path in defaults: config.update(options(path))
    config_file=project/defs.get('SDKCONFIG','sdkconfig')
    if config_file.exists(): config.update(json.loads(config_file.read_text()))
    text=json.dumps(config,sort_keys=True)
    if not config_file.exists() or config_file.read_text()!=text: config_file.write_text(text)
    (build/'config').mkdir(exist_ok=True)
    (build/'config/sdkconfig.json').write_text(text)
    (build/'CMakeCache.txt').write_text(''.join(f'{k}:STRING={v}\n' for k,v in defs.items()))
    version=defs['PROJECT_VER']+('.mockcloud' if mock else '')
    (build/'project_description.json').write_text(json.dumps({'project_version':version,
        'config_file':str(config_file),'config_defaults':';'.join(map(str,defaults))}))
    if 'build' in args:
        sku={'ethernet':'jetkvm-mini-ethernet','wifi':'jetkvm-mini-wireless'}[variant]
        head=bytearray(b'\xff'*352)
        struct.pack_into('<I',head,32,0xABCD5432);head[48:80]=version.encode().ljust(32,b'\0')
        struct.pack_into('<I',head,288,0x4B534B4A);head[292:324]=sku.encode().ljust(32,b'\0')
        ca=b'JetKVM Mini e2e mock cloud test CA' if config.get('MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE') else b''
        ca+=b'\n'+fingerprint('include/jetkvm_ota_signature.h','JETKVM_OTA_RELEASE_KEY_FINGERPRINT')
        if mock: ca+=b'\n'+fingerprint('jetkvm_ota_test_key.h','JETKVM_OTA_TEST_RELEASE_KEY_FINGERPRINT')
        image=bytes(head)+text.encode()+b'\n'+json.dumps(defs,sort_keys=True).encode()+b'\n'+ca
        (build/'jetkvm-mini.bin').write_bytes(image)
        (build/'flash_args').write_text('0x1000 jetkvm-mini.bin\n')
        (build/'jetkvm-mini-full.bin').write_bytes(b'\xff'*0x1000+image)
'''


def checkout(test, stand_ins):
    """A temporary checkout with the release scripts, and an environment with the named stand-ins on PATH."""
    temp = tempfile.TemporaryDirectory()
    test.addCleanup(temp.cleanup)
    root = Path(temp.name) / 'checkout'
    (root / 'tools').mkdir(parents=True)
    for script in ('release_r2.sh', 'release_check.py', 'release_config.py', 'mini_image.py'):
        shutil.copy(TOOLS / script, root / 'tools' / script)
    for file in [REPOSITORY / 'partitions.csv', *REPOSITORY.glob('sdkconfig.*defaults')]:
        shutil.copy(file, root / file.name)
    for file in ('include/jetkvm_ota_signature.h', 'jetkvm_ota_test_key.h'):
        (root / 'components/jetkvm_ota' / file).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REPOSITORY / 'components/jetkvm_ota' / file, root / 'components/jetkvm_ota' / file)
    (root / 'frontend/jetkvm-ui/node_modules').mkdir(parents=True)
    bins = Path(temp.name) / 'bin'
    bins.mkdir()
    for name in stand_ins:
        file = bins / name
        file.write_text(SHIM)
        file.chmod(0o755)
    env = {name: value for name, value in os.environ.items() if not name.startswith('VITE_')}
    env.update(CONTROL_ROOT=str(root), R2_PATH=str(Path(temp.name) / 'r2'),
               CLOUD_APP_R2_PATH=str(Path(temp.name) / 'cloud-app'),
               PATH=str(bins) + os.pathsep + os.environ['PATH'])
    return root, env


def release(root, env, *options, version=VERSION, answers='', dry_run=True):
    return subprocess.run(['bash', str(root / 'tools/release_r2.sh'), '--version', version,
                           *(['--dry-run'] if dry_run else []), *options],
                          env=env, capture_output=True, text=True, timeout=60, input=answers)


class ReleaseBuildTests(unittest.TestCase):
    def setUp(self):
        self.root, self.env = checkout(self, ('git', 'rclone', 'npm', 'docker', 'idf.py'))
        (self.root / '.git').mkdir()

    def idf(self, build, variant, *options, targets=('reconfigure', 'build')):
        subprocess.run(['idf.py', '-B', build, f'-DJETKVM_BOARD_VARIANT={variant}', f'-DPROJECT_VER={VERSION}',
                        *options, *targets], cwd=self.root, env=self.env, check=True, timeout=20)

    def bench_build_in_the_checkout(self):
        # A Wi-Fi image for the e2e mock cloud, configured in the checkout's sdkconfig.
        self.idf('build-bench', 'wifi', '-DJETKVM_MOCK_CLOUD_JWKS_URL=http://bench:8791/oidc/jwks',
                 '-DJETKVM_WIFI_SETUP_LOG_CODE=1')
        self.assertTrue(json.loads((self.root / 'sdkconfig').read_text())['MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE'])

    def release(self, *options):
        return release(self.root, self.env, *options)

    def staged(self):
        """The configuration in each staged image, by board variant."""
        configs = {}
        for variant, sku in SKUS.items():
            image = (self.root / 'build-release-stage' / VERSION / 'skus' / sku / 'jetkvm-mini.bin').read_bytes()
            self.assertNotIn(TEST_CA, image, variant)
            configs[variant] = json.loads(image[mini_image.HEAD_BYTES:].split(b'\n')[0])
        return configs

    def test_a_release_after_a_bench_build_has_the_configuration_of_a_clean_checkout(self):
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        clean = self.staged()
        self.assertTrue(clean['wifi']['ESP_HOSTED_ENABLED'])
        self.assertFalse(clean['ethernet']['ESP_HOSTED_ENABLED'])
        self.bench_build_in_the_checkout()
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.staged(), clean)

    def test_a_configuration_file_left_in_a_release_build_directory_is_not_used(self):
        self.idf('build-release-ethernet', 'ethernet', '-DSDKCONFIG=build-release-ethernet/sdkconfig',
                 '-DJETKVM_MOCK_CLOUD_JWKS_URL=http://bench:8791/oidc/jwks')
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE', self.staged()['ethernet'])

    def test_a_reused_build_directory_configured_from_the_checkout_sdkconfig_is_refused(self):
        self.bench_build_in_the_checkout()
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        # The same version and board, configured again without a file of its own.
        self.idf('build-release-wifi', 'wifi', '-DJETKVM_WIFI_SETUP_LOG_CODE=0', '-DJETKVM_MOCK_CLOUD_JWKS_URL=',
                 targets=('reconfigure',))
        result = self.release('--skip-build')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE=y', result.stderr)
        self.assertIn(f"SDKCONFIG is '', not {self.root}/build-release-wifi/sdkconfig", result.stderr)

    def usb_debugging_defaults(self):
        """The defaults of a bench image from the Dockerfile with USB_DEBUGGING=1."""
        extra = self.root.parent / 'usb_debugging.defaults'
        extra.write_text('CONFIG_JETKVM_USB_DEBUGGING_DEFAULT_ON=y\n')
        return f'-DSDKCONFIG_DEFAULTS={self.root}/sdkconfig.defaults;{extra}'

    def test_a_release_starts_with_usb_debugging_off_after_a_bench_build_with_it_on(self):
        self.idf('build-bench', 'wifi', self.usb_debugging_defaults())
        self.assertTrue(json.loads((self.root / 'sdkconfig').read_text())['JETKVM_USB_DEBUGGING_DEFAULT_ON'])
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        for variant, config in self.staged().items():
            self.assertNotIn('JETKVM_USB_DEBUGGING_DEFAULT_ON', config, variant)

    def test_a_release_build_directory_configured_with_usb_debugging_on_is_refused(self):
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        build = self.root / 'build-release-wifi'
        self.idf(str(build), 'wifi', f'-DSDKCONFIG={build}/sdkconfig', '-DJETKVM_WIFI_SETUP_LOG_CODE=0',
                 '-DJETKVM_MOCK_CLOUD_JWKS_URL=', self.usb_debugging_defaults(), targets=('reconfigure',))
        result = self.release('--skip-build')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('CONFIG_JETKVM_USB_DEBUGGING_DEFAULT_ON=y is set, which only a bench build may have',
                      result.stderr)

    def test_a_reused_build_directory_with_the_bench_factory_mac_is_refused(self):
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        build = self.root / 'build-release-ethernet'
        config = json.loads((build / 'sdkconfig').read_text())
        (build / 'sdkconfig').write_text(json.dumps({**config, 'JETKVM_BENCH_CUSTOM_MAC': True}))
        # Configured again with the release command; the images stay as the release built them.
        self.idf(str(build), 'ethernet', f'-DSDKCONFIG={build}/sdkconfig', '-DJETKVM_WIFI_SETUP_LOG_CODE=0',
                 '-DJETKVM_MOCK_CLOUD_JWKS_URL=', targets=('reconfigure',))
        result = self.release('--skip-build')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('CONFIG_JETKVM_BENCH_CUSTOM_MAC=y is set, which only a bench build may have', result.stderr)

    def test_a_reused_build_directory_is_staged_as_the_release_built_it(self):
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        built = self.staged()
        result = self.release('--skip-build')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.staged(), built)

    def test_a_reused_build_directory_that_was_built_again_with_another_option_is_refused(self):
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        # The release command with one more CMake option; the configuration files stay the same.
        build = self.root / 'build-release-wifi'
        self.idf(str(build), 'wifi', f'-DSDKCONFIG={build}/sdkconfig', '-DJETKVM_WIFI_SETUP_LOG_CODE=0',
                 '-DJETKVM_MOCK_CLOUD_JWKS_URL=', '-DJETKVM_TF_RAW_BOOT_SELFTEST=1')
        result = self.release('--skip-build')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f'{build} does not hold the images that a release build recorded', result.stderr)

    def test_a_reused_ui_bundle_is_the_one_a_release_built(self):
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.release('--skip-ui-build')
        self.assertEqual(result.returncode, 0, result.stderr)
        # The bundle of another build, for example one made while an env file was there.
        bundle = self.root / 'frontend/jetkvm-ui/dist-p4'
        (bundle / 'index.html').write_text('ui with VITE_CLOUD_API=https://env-file.invalid')
        result = self.release('--skip-ui-build')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f'{bundle} is not the bundle that a release build recorded', result.stderr)

    def test_a_prerelease_lists_the_changes_since_the_previous_stable_tag(self):
        self.env['GIT_TAGS'] = 'release/0.4.9 release/0.5.0 release/0.5.1-dev202601010000'
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('changes since release/0.5.0:\n  abcdef1 a change release/0.5.0..HEAD\n', result.stdout)

    def test_a_release_without_an_earlier_tag_lists_no_changes(self):
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('no earlier release tag; no list of changes\n', result.stdout)
        self.assertNotIn('changes since', result.stdout)

    def test_a_release_fetches_the_release_tags_before_it_reads_them(self):
        calls = self.root.parent / 'calls'
        calls.write_text('')
        self.env['STAND_IN_CALLS'] = str(calls)
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = calls.read_text().splitlines()
        fetch = 'git fetch --quiet origin refs/tags/release/*:refs/tags/release/*'
        self.assertIn(fetch, calls)
        # Both the check for an existing tag and the list of changes read the fetched tags.
        for reader in ('git rev-parse --quiet --verify refs/tags/release/', 'git tag -l'):
            reads = [i for i, call in enumerate(calls) if call.startswith(reader)]
            self.assertTrue(reads, (reader, calls))
            self.assertLess(calls.index(fetch), reads[0], reader)

    def test_a_failing_tag_fetch_stops_the_release_before_any_build(self):
        self.env['GIT_FETCH_FAILS'] = '1'
        result = self.release()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('could not fetch the release tags from origin', result.stderr)
        self.assertFalse((self.root / 'build-release-ethernet').exists(), 'the firmware was built')


STABLE = '0.5.1'
TEST_RELEASE_KEY = re.search(r'JETKVM_OTA_TEST_RELEASE_KEY_FINGERPRINT "([0-9A-F]{40})"',
                             (REPOSITORY / 'components/jetkvm_ota/jetkvm_ota_test_key.h').read_text()).group(1)


@unittest.skipUnless(shutil.which('gpg'), 'needs the gpg command')
class ReleaseSignatureTests(unittest.TestCase):
    """A stable release is signed with a subkey of the release key that the firmware names. GnuPG is real,
    with throwaway keys; the release key of the temporary checkout is the throwaway one."""

    def setUp(self):
        self.root, self.env = checkout(self, ('git', 'rclone', 'npm', 'docker', 'idf.py'))
        (self.root / '.git').mkdir()
        home = self.root.parent / 'gnupg'
        home.mkdir(mode=0o700)
        self.env['GNUPGHOME'] = str(home)
        self.addCleanup(subprocess.run, ['gpgconf', '--kill', 'all'], env=self.env, capture_output=True)
        self.release_key, self.signing_key = self.key('release', subkey=True)
        self.other_key, _ = self.key('other')
        header = self.root / 'components/jetkvm_ota/include/jetkvm_ota_signature.h'
        header.write_text(re.sub(r'(JETKVM_OTA_RELEASE_KEY_FINGERPRINT) "[0-9A-F]{40}"', rf'\1 "{self.release_key}"',
                                 header.read_text()))

    def gpg(self, *args):
        return subprocess.run(['gpg', '--batch', '--pinentry-mode', 'loopback', '--passphrase', '', *args],
                              env=self.env, capture_output=True, text=True, timeout=60)

    def key(self, name, subkey=False):
        """A throwaway Ed25519 key: (primary fingerprint, signing subkey fingerprint or None). GnuPG makes it
        with a clock that is one hour behind, so a subkey that a test adds has a later creation time, which
        GnuPG keeps in whole seconds."""
        made = ('--faked-system-time', str(int(time.time()) - 3600))
        generated = self.gpg(*made, '--quick-gen-key', f'release test {name}', 'ed25519',
                             'cert' if subkey else 'sign', 'never')
        self.assertEqual(generated.returncode, 0, f'GnuPG made no key: {generated.stderr}')
        listed = self.gpg('--list-keys', '--with-colons', f'release test {name}').stdout
        primary = re.findall(r'^fpr:+([0-9A-F]{40}):', listed, re.M)[0]
        if not subkey:
            return primary, None
        return primary, self.add_subkey(primary, 'ed25519', *made)

    def newer_subkey(self, algorithm):
        """Adds a signing subkey to the release key, with the present time, and returns its fingerprint."""
        # GnuPG makes an Ed448 key, which is a version 5 key, only with its own compliance mode.
        return self.add_subkey(self.release_key, algorithm, '--compliance=gnupg')

    def add_subkey(self, primary, algorithm, *options):
        """Adds a signing subkey to primary with the extra GnuPG options and returns its fingerprint."""
        def fingerprints():
            listed = self.gpg('--list-keys', '--with-colons', primary).stdout
            return re.findall(r'^fpr:+([0-9A-F]+):', listed, re.M)
        before = fingerprints()
        for _ in range(8):
            added = self.gpg(*options, '--quick-add-key', primary, algorithm, 'sign', 'never')
            if len(fingerprints()) == len(before) + 1:
                return fingerprints()[-1]
            # GnuPG 2.4.4 and 2.4.7 do not find the secret key of about 1 new Ed448 key in 140 for the
            # signature that the subkey makes over the primary key, and then add no subkey.
            if 'failed for backsig: No secret key' not in added.stderr:
                break
        self.fail(f'GnuPG added no {algorithm} subkey: {added.stderr}')

    def signers(self, version):
        """The fingerprint of the key that made each staged signature, as GnuPG verifies it."""
        stage = self.root / 'build-release-stage' / version
        found = set()
        for file in self.staged(version):
            if file.endswith('.sig'):
                status = self.gpg('--status-fd', '1', '--verify', str(stage / file), str(stage / file)[:-4]).stdout
                found.update(re.findall(r'VALIDSIG ([0-9A-F]+) ', status))
        return found

    def staged(self, version):
        return sorted(str(file.relative_to(self.root / 'build-release-stage' / version))
                      for file in (self.root / 'build-release-stage' / version).rglob('*') if file.is_file())

    def test_a_stable_release_has_a_signature_beside_each_image_that_gnupg_verifies(self):
        # An operator's configuration that asks for ASCII armor does not change the form.
        (Path(self.env['GNUPGHOME']) / 'gpg.conf').write_text('armor\n')
        result = release(self.root, self.env, '--signing-key', self.signing_key, version=STABLE, answers='y\n' * 4)
        self.assertEqual(result.returncode, 0, result.stderr)
        files = [f'skus/{sku}/{image}{suffix}' for sku in SKUS.values()
                 for image in ('jetkvm-mini.bin', 'jetkvm-mini-full.bin') for suffix in ('', '.sha256', '.sig')]
        self.assertEqual(self.staged(STABLE), sorted(files))
        stage = self.root / 'build-release-stage' / STABLE
        for file in files:
            if not file.endswith('.sig'):
                continue
            signature = stage / file
            # Binary, as the Mini reads it: a signature packet, not ASCII armor.
            self.assertIn(signature.read_bytes()[0], (0x88, 0x89, 0xC2), file)
            status = self.gpg('--status-fd', '1', '--verify', str(signature), str(signature)[:-4]).stdout
            self.assertRegex(status, rf'VALIDSIG {self.signing_key} .* {self.release_key}\n', file)
        self.assertIn(f'each signature verifies against the release key {self.release_key}', result.stdout)

    def test_the_signature_is_of_the_named_subkey_when_the_release_key_has_a_newer_one(self):
        # GnuPG takes a fingerprint without "!" as the name of the whole key and signs with the signing
        # subkey that was created last.
        self.newer_subkey('ed25519')
        for named in (self.signing_key, self.signing_key + '!'):
            with self.subTest(named=named):
                result = release(self.root, self.env, '--signing-key', named, version=STABLE, answers='y\n' * 4)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.signers(STABLE), {self.signing_key})

    def test_a_prerelease_is_not_signed_and_needs_no_key(self):
        result = release(self.root, self.env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([file for file in self.staged(VERSION) if file.endswith('.sig')], [])

    def test_a_stable_release_without_a_signing_key_stops_before_any_build(self):
        for options, reason in (((), 'set --signing-key or SIGNING_KEY_FPR'),
                                (('--signing-key', '0' * 40), 'is not in the local GPG keyring')):
            with self.subTest(options=options):
                result = release(self.root, self.env, *options, version=STABLE)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(reason, result.stderr)
                self.assertFalse((self.root / 'build-release-ethernet').exists(), 'the firmware was built')

    def branch_release(self, head_on, *options, dry_run=True):
        """A stable release with HEAD on the refs of head_on; returns the result and the stand-ins' calls."""
        calls = self.root.parent / 'calls'
        calls.write_text('')
        result = release(self.root, dict(self.env, GIT_HEAD_ON=head_on, STAND_IN_CALLS=str(calls)),
                         '--signing-key', self.signing_key, *options, version=STABLE, answers='y\n' * 5,
                         dry_run=dry_run)
        return result, calls.read_text().splitlines()

    def test_a_stable_release_from_origin_main_names_no_branch(self):
        result, calls = self.branch_release('origin/main', dry_run=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('git fetch --quiet origin main', calls)
        self.assertIn('>> release 0.5.1 from abcdef1\n', result.stdout)
        self.assertIn('tagged release/0.5.1 at abcdef1 and pushed the tag', result.stdout)
        self.assertNotIn('WARNING', result.stderr)
        self.assertFalse([call for call in calls if 'build-branch' in call])

    def test_a_stable_release_off_origin_main_is_refused_without_the_release_branch(self):
        result, _ = self.branch_release('origin/release-branch/0.5.19')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('stable 0.5.1 must build from a commit on origin/main; HEAD abcdef1 is not', result.stderr)
        self.assertFalse((self.root / 'build-release-ethernet').exists(), 'the firmware was built')

    def test_a_stable_release_from_the_release_branch_names_the_branch_and_records_it(self):
        result, calls = self.branch_release('origin/release-branch/0.5.19', '--release-branch',
                                            'release-branch/0.5.19', dry_run=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        # Into the tracking ref that the ancestor check reads, also in a clone of one branch.
        self.assertIn('git fetch --quiet origin '
                      '+refs/heads/release-branch/0.5.19:refs/remotes/origin/release-branch/0.5.19', calls)
        self.assertIn('git merge-base --is-ancestor HEAD origin/release-branch/0.5.19', calls)
        self.assertNotIn('git fetch --quiet origin main', calls)
        self.assertIn('Releasing from branch release-branch/0.5.19 (commit abcdef1) instead of main.\n', result.stdout)
        self.assertNotIn('WARNING', result.stdout + result.stderr)
        self.assertIn('>> release 0.5.1 from abcdef1 on branch release-branch/0.5.19\n', result.stdout)
        self.assertIn('staged 0.5.1 from abcdef1 on branch release-branch/0.5.19:', result.stdout)
        self.assertIn('tagged release/0.5.1 at abcdef1 on branch release-branch/0.5.19 and pushed the tag', result.stdout)
        ui_upload = [call for call in calls if call.startswith('rclone copy') and 'x-amz-meta-jetkvm-version' in call]
        self.assertEqual(len(ui_upload), 1, calls)
        self.assertIn('x-amz-meta-jetkvm-build-branch: release-branch/0.5.19', ui_upload[0])

    def test_a_stable_release_lists_the_changes_since_the_highest_stable_tag_below_it(self):
        tags = 'release/0.5.0 release/0.4.9 release/0.5.2 release/0.5.1-dev202601010000'
        result = release(self.root, dict(self.env, GIT_TAGS=tags), '--signing-key', self.signing_key,
                         version=STABLE, answers='y\n' * 4)
        self.assertEqual(result.returncode, 0, result.stderr)
        listed = 'changes since release/0.5.0:\n  abcdef1 a change release/0.5.0..HEAD\n'
        self.assertIn(listed, result.stdout)
        self.assertLess(result.stdout.index(listed), result.stdout.index('>> building the web UI'))

    def test_a_release_branch_that_does_not_hold_head_is_refused(self):
        result, calls = self.branch_release('origin/main', '--release-branch', 'release-branch/0.5.19')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('stable 0.5.1 with --release-branch release-branch/0.5.19 must build from a commit on '
                      'origin/release-branch/0.5.19; HEAD abcdef1 is not', result.stderr)
        self.assertNotIn('git fetch --quiet origin main', calls)
        self.assertFalse((self.root / 'build-release-ethernet').exists(), 'the firmware was built')

    def test_the_release_branch_is_refused_for_main_and_for_a_prerelease(self):
        for branch, version, reason in (
                ('main', STABLE, "--release-branch needs the name of a branch other than main: 'main'"),
                ('', STABLE, "--release-branch needs the name of a branch other than main: ''"),
                ('release-branch/0.5.19', VERSION, '--release-branch is for a stable version')):
            with self.subTest(branch=branch, version=version):
                result = release(self.root, dict(self.env, GIT_HEAD_ON='origin/main'), '--signing-key',
                                 self.signing_key, '--release-branch', branch, version=version)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(reason, result.stderr)
                self.assertFalse((self.root / 'build-release-ethernet').exists(), 'the firmware was built')

    def test_a_signature_of_a_key_outside_the_release_key_is_refused(self):
        result = release(self.root, self.env, '--signing-key', self.other_key, version=STABLE, answers='y\n' * 4)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f'is not signed with the JetKVM release key {self.release_key}', result.stderr)

    def test_an_answer_other_than_y_signs_nothing(self):
        result = release(self.root, self.env, '--signing-key', self.signing_key, version=STABLE, answers='n\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('signing cancelled', result.stderr)
        self.assertEqual([file for file in self.staged(STABLE) if file.endswith('.sig')], [])

    def test_a_signature_that_a_mini_does_not_read_is_refused(self):
        # Each of these verifies with GnuPG against the release key.
        home = Path(self.env['GNUPGHOME'])
        subkeys = {algorithm: self.newer_subkey(algorithm) for algorithm in ('rsa2048', 'ed448')}
        for key, configuration, reason in (
                (subkeys['ed448'], 'compliance gnupg\n', 'is an OpenPGP version 5 signature; a Mini reads version 4'),
                (subkeys['rsa2048'], '', 'is made with a key of algorithm 1; a Mini reads Ed25519 (22)'),
                (self.signing_key, 'digest-algo SHA224\n', 'has hash algorithm 11; a Mini reads SHA-256'),
                (self.signing_key, 'textmode\n', 'has signature type 0x01; a Mini reads the signature of a binary')):
            with self.subTest(reason=reason):
                (home / 'gpg.conf').write_text(configuration)
                result = release(self.root, self.env, '--signing-key', key, version=STABLE, answers='y\n' * 4)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f'.bin.sig {reason}', result.stderr)
                self.assertNotIn('dry run: nothing uploaded', result.stdout)

    def bucket(self):
        bucket = Path(self.env['R2_PATH']) / STABLE
        return sorted(str(file.relative_to(bucket)) for file in bucket.rglob('*') if file.is_file())

    def upload(self, **stand_in):
        return release(self.root, dict(self.env, **stand_in), '--signing-key', self.signing_key, version=STABLE,
                       answers='y\n' * 5, dry_run=False)

    def test_an_upload_puts_each_staged_file_into_the_bucket(self):
        result = self.upload()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.bucket(), self.staged(STABLE))
        self.assertEqual(len([file for file in self.bucket() if file.endswith('.sig')]), 4)

    def test_an_upload_that_stops_at_an_image_leaves_no_image_without_its_signature(self):
        # The stand-in copies the largest files first, so one copy of the whole stage has the recovery image
        # in the bucket before any signature.
        result = self.upload(RCLONE_STOPS_AT='jetkvm-mini.bin')
        self.assertNotEqual(result.returncode, 0)
        images = [file for file in self.bucket() if file.endswith('.bin')]
        self.assertEqual(len(images), 2, self.bucket())
        for image in images:
            self.assertIn(image + '.sig', self.bucket())
        self.assertNotIn('tagged release/', result.stdout)

    def test_no_image_is_uploaded_when_a_signature_is_not_in_the_bucket(self):
        # The copy of the signatures reports success and one file is not there.
        result = self.upload(RCLONE_LOSES='jetkvm-mini.bin.sig')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f'the signatures of {STABLE} are not all at', result.stderr)
        self.assertEqual([file for file in self.bucket() if not file.endswith('.sig')], [])
        self.assertEqual(len(self.bucket()), 2)

    def test_an_image_that_names_the_test_release_key_is_refused(self):
        result = release(self.root, self.env)
        self.assertEqual(result.returncode, 0, result.stderr)
        # The images of a release build with the test key's fingerprint in them, recorded as that build's.
        for variant in SKUS:
            build = self.root / f'build-release-{variant}'
            image = (build / 'jetkvm-mini.bin').read_bytes() + b'\n' + TEST_RELEASE_KEY.encode()
            (build / 'jetkvm-mini.bin').write_bytes(image)
            (build / 'jetkvm-mini-full.bin').write_bytes(b'\xff' * 0x1000 + image)
            sums = subprocess.run(['sha256sum', 'jetkvm-mini.bin', 'jetkvm-mini-full.bin'], cwd=build,
                                  capture_output=True, text=True, check=True).stdout
            (build / 'release-images.sha256').write_text(sums)
        result = release(self.root, self.env, '--skip-build')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('image names the test release key; it is a bench e2e build', result.stderr)


class ReleaseUiEnvironmentTests(unittest.TestCase):
    """The preflight in a git checkout with this repository's ignore rules; only git is not a stand-in."""

    def setUp(self):
        self.root, self.env = checkout(self, ('rclone', 'npm', 'docker', 'idf.py'))
        self.ui = self.root / 'frontend/jetkvm-ui'
        for file in ('.gitignore', 'frontend/jetkvm-ui/.gitignore', 'frontend/jetkvm-ui/.env.cloud-production'):
            shutil.copy(REPOSITORY / file, self.root / file)
        # Only the checkout's own ignore rules apply.
        self.env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull)
        for command in (['init', '-q'], ['add', '-A'],
                        ['-c', 'user.name=test', '-c', 'user.email=test@example.invalid', 'commit', '-q', '-m', 'release']):
            subprocess.run(['git', *command], cwd=self.root, env=self.env, check=True, capture_output=True)
        # An empty origin, from which the release fetches the release tags.
        origin = self.root.parent / 'origin.git'
        subprocess.run(['git', 'init', '-q', '--bare', str(origin)], env=self.env, check=True, capture_output=True)
        subprocess.run(['git', 'remote', 'add', 'origin', str(origin)], cwd=self.root, env=self.env, check=True,
                       capture_output=True)

    def refused(self, env, *lines):
        """Run the release; it must stop before the first build and print the lines."""
        result = release(self.root, env)
        self.assertNotEqual(result.returncode, 0)
        for line in lines:
            self.assertIn(line, result.stderr)
        self.assertFalse((self.ui / 'dist-p4').exists(), 'the UI was built')
        self.assertFalse((self.root / 'build-release-ethernet').exists(), 'the firmware was built')

    def test_an_env_file_that_git_ignores_stops_the_release_before_any_build(self):
        # .env is ignored by a rule outside the repository's files; the others by frontend/jetkvm-ui/.gitignore.
        (self.root / '.git/info').mkdir(exist_ok=True)
        (self.root / '.git/info/exclude').write_text('.env\n.envrc\n')
        for name in ('.env.local', '.env.device.local', '.env.cloud-production.local', '.env'):
            with self.subTest(file=name):
                (self.ui / name).write_text('VITE_CLOUD_API=https://env-file.invalid\n')
                try:
                    self.refused(self.env, f'   frontend/jetkvm-ui/{name}\n', 'git ignores these Vite env files')
                finally:
                    (self.ui / name).unlink()
        # Without them the same checkout is released, with its committed .env.cloud-production and with an
        # ignored file that Vite does not read.
        (self.ui / '.envrc').write_text('export VITE_CLOUD_API=https://env-file.invalid\n')
        result = release(self.root, self.env)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_a_vite_variable_in_the_environment_stops_the_release_before_any_build(self):
        self.refused(dict(self.env, VITE_CLOUD_API='https://shell.invalid'),
                     '   VITE_CLOUD_API\n', 'the environment sets these Vite variables')


class ReleaseConfigTests(unittest.TestCase):
    """release_config.py against the defaults files of this repository."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name) / 'build-release-wifi'
        (self.build / 'config').mkdir(parents=True)

    def differences(self, cache=None, description=None, config=None):
        """The differences of a Wi-Fi release build directory with the given changes."""
        values = {'SDKCONFIG': f'{self.build}/sdkconfig', 'JETKVM_BOARD_VARIANT': 'wifi',
                  'JETKVM_MOCK_CLOUD_JWKS_URL': '', 'JETKVM_WIFI_SETUP_LOG_CODE': '0', **(cache or {})}
        types = {'SDKCONFIG': 'UNINITIALIZED'}
        (self.build / 'CMakeCache.txt').write_text(
            '# This is the CMakeCache file.\n//JetKVM hardware variant (ethernet or wifi)\n' +
            ''.join(f'{name}:{types.get(name, "STRING")}={value}\n' for name, value in values.items()
                    if value is not None))
        (self.build / 'project_description.json').write_text(json.dumps({
            'config_file': f'{self.build}/sdkconfig.jetkvm-lwip-g256-t256-w1310720',
            'config_defaults': f'{REPOSITORY}/sdkconfig.defaults;{REPOSITORY}/sdkconfig.wifi.defaults',
            **(description or {})}))
        (self.build / 'config/sdkconfig.json').write_text(json.dumps({
            'ESP_HOSTED_ENABLED': True, 'MBEDTLS_CERTIFICATE_BUNDLE': True,
            'MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE': False,
            'JETKVM_VIRTUAL_MEDIA_HTTP_DIAGNOSTIC_DISCARD': False,
            'JETKVM_USB_DEBUGGING_DEFAULT_ON': False, **(config or {})}))
        return release_config.differences(REPOSITORY, self.build, 'wifi', ['ethernet', 'wifi'])

    def test_a_release_configuration_has_no_differences(self):
        self.assertEqual(self.differences(), [])

    def test_an_option_of_a_bench_defaults_file_is_named_with_its_file(self):
        for option, file in (('MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE', 'sdkconfig.mockcloud.defaults'),):
            with self.subTest(option=option):
                self.assertEqual(self.differences(config={option: True}),
                                 [f'CONFIG_{option}=y is set, which only a bench build may have ({file})'])

    def test_the_path_of_the_e2e_test_ca_is_named(self):
        path = 'frontend/jetkvm-ui/e2e/mini/mock-cloud-ca.pem'
        self.assertEqual(self.differences(config={'MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE_PATH': path}),
                         [f'CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE_PATH="{path}" is set, '
                          'which only a bench build may have (sdkconfig.mockcloud.defaults)'])

    def test_a_bench_option_without_a_defaults_file_is_named(self):
        self.assertEqual(self.differences(config={'JETKVM_VIRTUAL_MEDIA_HTTP_DIAGNOSTIC_DISCARD': True}),
                         ['CONFIG_JETKVM_VIRTUAL_MEDIA_HTTP_DIAGNOSTIC_DISCARD=y is set, '
                          'which only a bench build may have'])

    def test_usb_debugging_on_by_default_is_named(self):
        self.assertEqual(self.differences(config={'JETKVM_USB_DEBUGGING_DEFAULT_ON': True}),
                         ['CONFIG_JETKVM_USB_DEBUGGING_DEFAULT_ON=y is set, which only a bench build may have'])

    def test_the_bench_factory_mac_is_named(self):
        self.assertEqual(self.differences(config={'JETKVM_BENCH_CUSTOM_MAC': True}),
                         ['CONFIG_JETKVM_BENCH_CUSTOM_MAC=y is set, which only a bench build may have'])

    def test_no_committed_file_turns_on_a_bench_only_option(self):
        # A release is configured from the committed defaults files and the
        # Kconfig defaults, so neither may turn on an option of BENCH_ONLY.
        kconfig = '\n'.join(path.read_text() for path in (REPOSITORY / 'components').glob('*/Kconfig*'))
        for name in release_config.BENCH_ONLY:
            with self.subTest(option=name):
                entry = re.search(rf'^config {name}\n(.*?)(?=^\S|\Z)', kconfig, re.M | re.S)
                self.assertIsNotNone(entry)
                self.assertNotRegex(entry.group(1), r'(?m)^\s+default (?!n$)')
                for path in REPOSITORY.glob('sdkconfig*defaults'):
                    self.assertNotIn(f'CONFIG_{name}=', path.read_text(), path.name)

    def test_an_option_of_the_other_board_is_not_a_bench_option(self):
        # sdkconfig.ethernet.defaults sets this one; a Wi-Fi release may have any value for it.
        self.assertEqual(self.differences(config={'LWIP_TCPIP_CORE_LOCKING': True}), [])

    def test_a_configuration_from_the_checkout_sdkconfig_is_refused(self):
        self.assertEqual(self.differences(cache={'SDKCONFIG': None}),
                         [f"SDKCONFIG is '', not {self.build}/sdkconfig"])
        self.assertEqual(self.differences(description={'config_file': f'{REPOSITORY}/sdkconfig'}),
                         [f'the configuration file is {REPOSITORY}/sdkconfig, outside {self.build}'])

    def test_bench_values_of_the_cmake_options_are_refused(self):
        self.assertEqual(self.differences(cache={'JETKVM_MOCK_CLOUD_JWKS_URL': 'http://bench:8791/oidc/jwks'}),
                         ["JETKVM_MOCK_CLOUD_JWKS_URL is 'http://bench:8791/oidc/jwks', "
                          'which only a bench build may have'])
        self.assertEqual(self.differences(cache={'JETKVM_WIFI_SETUP_LOG_CODE': '1'}),
                         ["JETKVM_WIFI_SETUP_LOG_CODE is '1', which only a bench build may have"])

    def test_another_defaults_file_or_board_is_refused(self):
        defaults = f'{REPOSITORY}/sdkconfig.defaults;{REPOSITORY}/sdkconfig.wifi.defaults'
        self.assertEqual(
            self.differences(description={'config_defaults': f'{defaults};{REPOSITORY}/sdkconfig.mockcloud.defaults'}),
            [f'the defaults are {defaults.replace(";", ", ")}, {REPOSITORY}/sdkconfig.mockcloud.defaults; '
             f'a release uses {defaults.replace(";", " and ")}'])
        self.assertEqual(self.differences(cache={'JETKVM_BOARD_VARIANT': 'ethernet'}),
                         ["JETKVM_BOARD_VARIANT is 'ethernet', not wifi"])

    def test_a_directory_without_a_configuration_is_refused(self):
        self.differences()
        (self.build / 'config/sdkconfig.json').unlink()
        reasons = release_config.differences(REPOSITORY, self.build, 'wifi', ['ethernet', 'wifi'])
        self.assertEqual(len(reasons), 1)
        self.assertIn(f'cannot read the configuration of {self.build}', reasons[0])


if __name__ == '__main__':
    unittest.main()
