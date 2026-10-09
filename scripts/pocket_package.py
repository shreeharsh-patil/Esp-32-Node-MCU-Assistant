"""Generate manifests, build evidence, licenses and a source/release ZIP."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import zipfile
from pocket_image_check import inspect

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / 'pocket-release'


def backend_files():
    """Explicit deploy-source inventory: never traverse dependencies or secrets."""
    names = ['package.json', 'package-lock.json', 'vercel.json', 'dev.js',
             '.gitignore', '.env.example', 'README.md', 'DASHBOARD.md', 'VALIDATION.md', 'VALIDATION.json']
    paths = [ROOT / 'backend' / name for name in names]
    for directory in ['src', 'tests', 'scripts']:
        paths.extend((ROOT / 'backend' / directory).glob('*.js'))
    paths.extend(path for path in (ROOT / 'backend/public').glob('*') if path.suffix in ('.html', '.css', '.js'))
    paths.extend((ROOT / 'backend/preview').glob('dashboard-*.jpg'))
    return sorted(path for path in paths if path.is_file())


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def archive_bytes(relative_name, path):
    """Keep identifying restore details local; anonymize device IDs in shared logs."""
    if relative_name == 'docs/pocket/DEVICE_BACKUP.log':
        return None
    if relative_name == 'docs/pocket/DEVICE_PROBE.json':
        probe = json.loads(path.read_text(encoding='utf-8-sig'))
        probe.pop('backup_file', None)
        probe.pop('backup_sha256', None)
        return (json.dumps(probe, indent=2) + '\n').encode('utf-8')
    if relative_name == 'docs/pocket/DEVICE_BACKUP.md':
        content = path.read_text(encoding='utf-8')
        content = re.sub(r'`[A-Za-z]:\\[^`]*device-backups\\[^`]*`', '`[private local backup path]`', content, flags=re.IGNORECASE)
        content = re.sub(r'`[0-9a-f]{64}`', '`[private local backup hash]`', content, flags=re.IGNORECASE)
        return content.encode('utf-8')
    if relative_name.lower().endswith('.log') and relative_name.startswith('docs/pocket/'):
        return re.sub(rb'UUID=[0-9a-fA-F-]{36}', b'UUID=<redacted>', path.read_bytes())
    return path.read_bytes()


def main():
    OUTPUT.mkdir(exist_ok=True)
    variants = {}
    for kind, name in [('conversation', 'pocket-xiaozhi-esp32.bin'),
                       ('diagnostics', 'pocket-xiaozhi-esp32-diagnostics.bin')]:
        report = inspect(OUTPUT / name)
        if not report['compatible_with_esp32_wroom32']:
            raise SystemExit(f'Invalid {kind} firmware')
        config = (OUTPUT / kind / 'sdkconfig.txt').read_text(encoding='utf-8')
        if ('CONFIG_POCKET_HARDWARE_DIAGNOSTICS=y' in config) != (kind == 'diagnostics'):
            raise SystemExit('Wrong variant configuration')
        if ('CONFIG_POCKET_CUSTOM_BACKEND=y' in config.splitlines()) != (kind == 'conversation'):
            raise SystemExit('Wrong custom backend configuration')
        variants[kind] = {'binary': name, 'offset': 0, 'bytes': report['bytes'],
                          'sha256': digest(OUTPUT / name), 'images': report['images'],
                          'size': json.loads((OUTPUT / kind / 'size.json').read_text())}
    upstream = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    idf_path = Path(os.environ.get('IDF_PATH', 'D:/Espressif/esp-idf-v6.1'))
    sdk_commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=idf_path, text=True).strip()
    tests = (ROOT / 'pocket-host-tests.log').read_text(encoding='utf-8', errors='replace')
    if '\nOK\n' not in tests.replace('\r\n', '\n'):
        raise SystemExit('Host test success report is missing')
    import re
    matches = re.findall(r'Ran (\d+) tests in', tests)
    test_count = int(matches[-1])
    regression = (ROOT / 'pocket-regression-build.log').read_text(encoding='utf-8', errors='replace')
    if 'Project build complete.' not in regression or 'FAILED:' in regression:
        raise SystemExit('Representative upstream ESP32 regression build did not pass')
    device_path = ROOT / 'docs/pocket/DEVICE_PROBE.json'
    device_probe = json.loads(device_path.read_text(encoding='utf-8-sig')) if device_path.exists() else None
    if device_probe:
        device_probe.pop('backup_file', None)
        device_probe.pop('backup_sha256', None)
    hardware_path = ROOT / 'docs/pocket/DEVICE_DIAGNOSTICS_RESULTS.json'
    hardware_results = json.loads(hardware_path.read_text(encoding='utf-8-sig')) if hardware_path.exists() else None
    ui_path = ROOT / 'docs/pocket/UI_VALIDATION.json'
    ui_results = json.loads(ui_path.read_text(encoding='utf-8-sig')) if ui_path.exists() else None
    hands_free = json.loads((ROOT / 'docs/pocket/HANDS_FREE_VALIDATION.json').read_text(encoding='utf-8'))
    backend_results = json.loads((ROOT / 'backend/VALIDATION.json').read_text(encoding='utf-8'))
    if backend_results.get('tests_passed', 0) < 30 or backend_results.get('tests_failed') != 0:
        raise SystemExit('Backend test success report is missing')
    evidence = {'board': 'custom-esp32-nodemcu-pocket-ai', 'target': 'esp32',
                'version': variants['conversation']['images'][-1]['version'],
                'upstream_commit': upstream, 'esp_idf_version': '6.1', 'esp_idf_commit': sdk_commit,
                'flash_layout_bytes': 0x400000, 'app_partition_bytes': 0x3f0000,
                'psram_enabled': False, 'firmware_ota_enabled': False,
                'host_tests_passed': test_count, 'regression_board': 'bread-compact-esp32-lcd',
                'hardware_tests_A_to_N': ('See hardware_test_results; cloud and extended acceptance pending'
                                        if hardware_results else 'PENDING: firmware not flashed or tested on physical board'),
                'voice_backend': 'User-owned Vercel Express/WebSocket backend with Gemini Live',
                'real_provider_and_conversation': 'PENDING: production URL/key and physical acceptance required',
                'peak_runtime_ram': 'PENDING: cannot be inferred from linker memory usage',
                'browser_installer': 'PENDING: no browser/serial end-to-end test',
                'physical_device_probe': device_probe, 'hardware_test_results': hardware_results,
                'companion_ui_validation': ui_results,
                'hands_free_validation': hands_free, 'backend_validation': backend_results,
                'variants': variants}
    (OUTPUT / 'VALIDATION.json').write_text(json.dumps(evidence, indent=2)+'\n', encoding='utf-8')
    layout = {row['name']: row for row in variants['conversation']['size']['layout']}
    device_text = (f"Read-only physical probe PASS: {device_probe['chip']} "
                   f"({device_probe['chip_revision']}) on {device_probe['serial_port']}, "
                   f"{device_probe['detected_flash_bytes']:,} bytes detected flash. "
                   "Firmware test evidence is listed separately below.\n" if device_probe else
                   "Actual physical chip/flash capacity verification is PENDING.\n")
    hardware_text = ('Actual diagnostic firmware tests/results: ' +
                     '; '.join(f"{name}: {status}" for name, status in hardware_results['tests'].items()) +
                     '\n\nMeasured offline minimum free internal heap: ' +
                     str(hardware_results['heap_measurements']['minimum_internal_free_bytes']) +
                     ' bytes. See docs/pocket/DEVICE_DIAGNOSTICS_RESULTS.json and the raw device logs.\n'
                     if hardware_results else 'Physical firmware tests A-N are PENDING.\n')
    installation_note = (
        'The current conversation image was flashed on COM4 with boot/table/app hash verification; '
        'NVS was preserved. Startup and the updated face were confirmed. Backend deployment '
        'and real speech/playback acceptance remain pending.'
        if ui_results and ui_results.get('hardware', {}).get('installation', '').startswith('PASS')
        else 'No new image was flashed during this implementation: deployment URL/private keys '
        'and subsequent hardware testing are still required.')
    text = f'''# Release validation

Official XiaoZhi source commit `{upstream}`. ESP-IDF 6.1 commit `{sdk_commit}`.

Executed software checks:
- Both uniquely named custom ESP32 variants compiled through the canonical build script.
- {test_count} host tests passed, including upstream tests and native audio/protocol bounds tests.
- Existing upstream `bread-compact-esp32-lcd` regression variant compiled with default feature policies.
- ESP32 boot/app image checksums and appended SHA-256 validated for both custom variants.
- Generated flash table, chip IDs, SDK metadata, no-PSRAM/OTA settings and release hashes validated.
- Source archive excludes local credentials, Git internals, SDK/tools and incompatible reference firmware.
- {backend_results['tests_passed']} Node backend tests passed with real Opus codecs, dashboard storage and injected test providers.
- Comma-separated Gemini key rotation and fixed per-session credentials passed locally.
- Custom backend authentication, packet bounds, audio pacing and half-duplex restart passed locally.

Conversation application binary: {(OUTPUT/'conversation/xiaozhi.bin').stat().st_size:,} bytes.
Merged image at 0x0: {variants['conversation']['bytes']:,} bytes.
Application partition: 4,128,768 bytes within 4 MB flash.
Static DRAM: {layout['DRAM']['used']:,} bytes. IRAM: {layout['IRAM']['used']:,} bytes.
Linker DRAM remaining: {layout['DRAM']['free']:,} bytes; this is **not measured runtime free heap**.

{device_text}
{hardware_text}
Current companion UI validation: see `docs/pocket/UI_VALIDATION.json` and
`COMPANION_UI.md`. Earlier portrait observations do not validate the new landscape
orientation. Native LVGL screenshots use simulated state/audio/setup data.

Full cloud conversation peak RAM, long-duration stability, acoustic acceptance,
speaker power measurement and real custom-backend conversation remain incomplete.
The new conversation image enables your Vercel backend and hands-free listening.
{installation_note} Read `backend/README.md`.
The optional browser installer is supplied but has no browser/serial end-to-end result.
Read `docs/pocket/HARDWARE_TESTS.md` for the actual acceptance procedure.

The upstream Linux-oriented test suite has Windows cwd cleanup and WSL-launcher
issues. `python -X utf8 scripts/pocket_host_tests.py` runs its assertions unchanged
with temporary cwd cleanup and explicit Git Bash selection. UTF-8 avoids Windows
default code-page errors. Negative-test fixture error messages in the log are expected.

The build reports retain upstream CMake dependency warnings and FATFS Kconfig notes.
These do not represent physical hardware evidence. OTA firmware writes are disabled;
there is no second app slot or signed-update/rollback claim.
'''
    (OUTPUT / 'VALIDATION.md').write_text(text, encoding='utf-8')
    shutil.copy2(ROOT / 'pocket-host-tests.log', OUTPUT / 'host-tests.log')
    shutil.copy2(ROOT / 'pocket-regression-build.log', OUTPUT / 'regression-build.log')
    shutil.copy2(ROOT / 'pocket-backend-tests.log', OUTPUT / 'backend-tests.log')
    manifest = {'name': 'Shreeharsh Assistant ESP32 NodeMCU', 'version': evidence['version'],
                'new_install_prompt_erase': True, 'new_install_improv_wait_time': 0,
                'builds': [{'chipFamily': 'ESP32', 'parts': [{'path': 'pocket-xiaozhi-esp32.bin', 'offset': 0}]}]}
    (OUTPUT / 'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    diagnostic_manifest = dict(manifest)
    diagnostic_manifest['name'] += ' Diagnostics'
    diagnostic_manifest['builds'] = [{'chipFamily': 'ESP32', 'parts': [
        {'path': 'pocket-xiaozhi-esp32-diagnostics.bin', 'offset': 0}]}]
    (OUTPUT / 'manifest-diagnostics.json').write_text(json.dumps(diagnostic_manifest, indent=2)+'\n', encoding='utf-8')
    (OUTPUT / 'index.html').write_text('''<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Shreeharsh Assistant · ESP32 NodeMCU</title>
<style>body{background:#090f1b;color:#e1e8f2;font:17px system-ui;max-width:660px;margin:60px auto;padding:24px}h1{color:#71dccd}section{background:#142237;padding:24px;margin:24px 0;border-radius:16px}button{background:#71dccd;border:0;padding:14px 22px;border-radius:10px;font-weight:bold;cursor:pointer}a{color:#71dccd}p{line-height:1.6}small{color:#aabbd0}</style>
<script type="module" src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module"></script>
<h1>Shreeharsh Assistant</h1><p>Classic ESP32 NodeMCU · ST7789 · INMP441 · MAX98357A</p>
<section><h2>Conversation firmware</h2><p>Hands-free voice with your own Vercel/Gemini backend. Deploy and configure the backend first; then enter its HTTPS address, device token and Wi-Fi details through the hotspot shown on the display.</p>
<esp-web-install-button manifest="manifest.json"><button slot="activate">Install conversation firmware</button></esp-web-install-button></section>
<section><h2>Hardware diagnostics</h2><p>Test the display, mic, speaker and Opus loopback over serial before connecting to the cloud.</p>
<esp-web-install-button manifest="manifest-diagnostics.json"><button slot="activate">Install diagnostic firmware</button></esp-web-install-button></section>
<p>Release BOOT before reset or flashing. Verify your board is classic ESP32 with at least 4 MB flash. These merged images clear stored settings; Wi-Fi and backend setup are required again. Choose full erase only for an initial migration. Use the guarded USB flasher to preserve existing settings.</p>
<p><a href="VALIDATION.md">Build validation and pending hardware tests</a> · <a href="SHA256SUMS.txt">Checksums</a></p>
<small>Use a Web Serial browser on localhost or HTTPS. See the validation report for actual hardware results. Real provider conversation and this browser installer remain unverified.</small></html>''', encoding='utf-8')
    licenses = OUTPUT / 'licenses'
    license_inventory = []
    for base, prefix in [(ROOT / 'managed_components', 'managed_components'), (idf_path, 'esp-idf')]:
        for path in base.rglob('*'):
            if not path.is_file() or '.git' in path.parts:
                continue
            lower = path.name.lower()
            if not (lower.startswith(('license', 'copying', 'notice')) or lower in ('ofl.txt', 'ftl.txt')):
                continue
            if path.stat().st_size > 1024 * 1024:
                continue
            relative = Path(prefix) / path.relative_to(base)
            target = licenses / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            license_inventory.append(relative.as_posix())
    (OUTPUT / 'LICENSE_INVENTORY.txt').write_text('\n'.join(sorted(license_inventory))+'\n', encoding='utf-8')
    (OUTPUT / 'source-changes.patch').write_bytes(subprocess.check_output(['git', 'diff', '--binary'], cwd=ROOT, stderr=subprocess.DEVNULL))
    backend_package = OUTPUT / 'Shreeharsh-Assistant-Vercel-backend.zip'
    with zipfile.ZipFile(backend_package, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path in backend_files():
            archive.write(path, 'backend/' + path.relative_to(ROOT / 'backend').as_posix())
        archive.write(ROOT / 'LICENSE', 'backend/LICENSE')
    with zipfile.ZipFile(backend_package) as archive:
        if archive.testzip() is not None:
            raise SystemExit('Backend ZIP CRC check failed')
    checksums = []
    for path in sorted(OUTPUT.rglob('*')):
        if path.is_file() and path.suffix.lower() != '.zip' and path.name not in ('SHA256SUMS.txt', 'PACKAGE_SHA256.txt'):
            checksums.append(f'{digest(path)}  {path.relative_to(OUTPUT).as_posix()}')
    (OUTPUT / 'SHA256SUMS.txt').write_text('\n'.join(checksums)+'\n', encoding='utf-8')
    source_names = set(subprocess.check_output(['git', 'ls-files'], cwd=ROOT, text=True).splitlines())
    for directory in ['main/boards/custom-esp32-nodemcu-pocket-ai', 'docs/pocket', 'cmake',
                      'scripts/pocket_ui_sim']:
        source_names.update(p.relative_to(ROOT).as_posix() for p in (ROOT/directory).rglob('*') if p.is_file())
    source_names.update(p.relative_to(ROOT).as_posix() for p in (ROOT/'scripts').glob('pocket_*.py'))
    source_names.update({'partitions/custom-pocket-4m.csv',
                         'main/protocols/websocket_audio_frame.h',
                         'scripts/tests/test_pocket_board.py', 'scripts/tests/pocket_native_test.cpp'})
    source_names.update(p.relative_to(ROOT).as_posix() for p in (ROOT/'scripts/tests').glob('pocket_*.cpp'))
    source_names.update(path.relative_to(ROOT).as_posix() for path in backend_files())
    package = OUTPUT / 'XiaoZhi-Pocket-ESP32-source-and-firmware.zip'
    with zipfile.ZipFile(package, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name in sorted(source_names):
            path = ROOT/name
            if path.is_file() and path.suffix.lower() not in ('.pem', '.key', '.pyc', '.bin') and (not path.name.startswith('.env') or name == 'backend/.env.example'):
                content = archive_bytes(name, path)
                if content is not None:
                    archive.writestr('XiaoZhi-Pocket-ESP32/'+name, content)
        # The generated lock pins component versions for a clean rebuild.
        archive.write(OUTPUT/'conversation/dependencies.lock', 'XiaoZhi-Pocket-ESP32/dependencies.lock')
        for path in sorted(OUTPUT.rglob('*')):
            if path.is_file() and path.suffix.lower() != '.zip' and path.name != 'PACKAGE_SHA256.txt':
                archive.write(path, 'XiaoZhi-Pocket-ESP32/pocket-release/'+path.relative_to(OUTPUT).as_posix())
    with zipfile.ZipFile(package) as archive:
        if archive.testzip() is not None:
            raise SystemExit('Source/release ZIP CRC check failed')
    (OUTPUT/'PACKAGE_SHA256.txt').write_text(
        f'{digest(package)}  {package.name}\n{digest(backend_package)}  {backend_package.name}\n', encoding='utf-8')
    print(f'Packaged {package.name}: {package.stat().st_size:,} bytes; {test_count} host tests; both ESP32 images verified')


if __name__ == '__main__':
    main()
