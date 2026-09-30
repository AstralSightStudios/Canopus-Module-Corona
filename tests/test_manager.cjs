/* Unified host-test entry point for Manager-side Vela modules. */
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const tests = [
  'test_file.cjs',
  'test_interconnect.cjs',
  'test_resource_storage.cjs',
  'test_reload_signal.cjs',
  'test_resource_order.cjs',
  'test_resource_overrides.cjs',
  'test_resource_catalog_async.cjs',
  'test_reload_native_copy.cjs',
  'test_mix_match_pages.cjs',
  'test_manager_layout.cjs'
];

for (const test of tests) {
  const result = spawnSync(process.execPath, [path.join(__dirname, test)], {
    stdio: 'inherit',
    cwd: path.resolve(__dirname, '..')
  });
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status === null ? 1 : result.status);
}

async function testPortableDestinations() {
  const assert = require('node:assert/strict');
  const fs = require('node:fs');
  const os = require('node:os');
  const temporary = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'manager-portable-')));
  const root = path.resolve(__dirname, '..');
  try {
    const compiled = spawnSync(path.join(root, 'manager/node_modules/.bin/tsc'), [
      ...['resource-pack', 'resource-activation', 'resource-storage', 'interconnect'].map(
        name => path.join(root, `manager/src/ts/${name}.ts`)),
      '--outDir', temporary, '--module', 'commonjs', '--target', 'es2018',
      '--lib', 'es2018,dom', '--skipLibCheck', '--allowJs'
    ], { stdio: 'inherit' });
    assert.equal(compiled.status, 0);
    const boundary = path.join(temporary, 'import.js');
    require.cache[boundary] = { id: boundary, filename: boundary, loaded: true, exports: {} };
    const { parseResourcePackManifest: parse, serializeResourcePackMappings: serialize } =
      require(path.join(temporary, 'resource-pack.js'));
    const { planActiveMappings: plan } = require(path.join(temporary, 'resource-activation.js'));
    const { removeInstalledTheme: remove } = require(path.join(temporary, 'resource-storage.js'));
    const { validateThemeRelativePath: validFile } = require(path.join(temporary, 'interconnect.js'));
    const { safeThemeDestination: validDestination } = require(path.join(temporary, 'resource-path.js'));
    const manifest = (themeId, destination = 'icons/', source = '/resource/icons/') => parse(JSON.stringify({
      format: 'canopus-resource-pack', formatVersion: 1, themeId, name: themeId,
      mappings: [{ source, destination }]
    }));
    const top = manifest('top');
    const low = manifest('low');
    const plain = plan([{ themeId: 'top', manifest: top, files: null }], 'portable');
    assert.equal(plain.mappings, '/resource/icons/\tthemes/top/icons/\n');
    assert.equal(serialize(top), plain.mappings);
    // Identical TSV expands under either supported device root, not a sender root.
    for (const nativeRoot of ['/data/files/ng.lst.corona/', '/data/quickapp/files/ng.lst.corona/']) {
      const destination = plain.mappings.trim().split('\t')[1];
      assert.equal(nativeRoot + destination, nativeRoot + 'themes/top/icons/');
      assert.ok(Buffer.byteLength(nativeRoot + destination) <= 255);
    }
    const asset = relativePath => ({ relativePath, sizeBytes: 1 });
    const themes = [
      { themeId: 'top', manifest: top, files: [asset('icons/a.bin')] },
      { themeId: 'low', manifest: low, files: [asset('icons/a.bin'), asset('icons/b.bin')] }
    ];
    const overlay = plan(themes, 'portable', {
      '/resource/icons/a.bin': 'low', '/resource/icons/b.bin': '@system'
    });
    assert.equal(overlay.mappings,
      '/resource/icons/\tthemes/.active-portable/r0/\n' +
      '/resource/icons/a.bin\tthemes/low/icons/a.bin\n' +
      '/resource/icons/b.bin\t@system\n');
    assert.equal(overlay.copies.length, 2);
    assert.ok(overlay.copies.every(copy => copy.destinationUri.startsWith(
      'internal://files/themes/.active-portable/r0/')));
    assert.ok(!overlay.mappings.includes('/data/'));

    for (const unsafe of ['../escape', './a', 'a/../b', 'a//b', '/absolute',
      'internal://files/a', 'a\\b', 'a\tb', 'a\nb', 'a\0b', 'a:b']) {
      assert.throws(() => manifest('top', unsafe, '/resource/a'));
      assert.equal(validFile(unsafe, 'top'), false);
      assert.equal(validDestination(`themes/top/${unsafe}`), false);
    }
    assert.equal(validDestination('/data/quickapp/files/ng.lst.corona/themes/top/a'), false);
    assert.throws(() => manifest('top', '/data/quickapp/files/ng.lst.corona/themes/top/a', '/resource/a'));
    assert.throws(() => plan([{ themeId: 'top', files: null, manifest: {
      ...top, mappings: [{ source: '/resource/a', destination: '/absolute' }]
    } }], 'portable'));
    const maxFile = 255 - 35 - Buffer.byteLength('themes/top/');
    for (const destination of ['x'.repeat(maxFile), '中'.repeat(Math.floor(maxFile / 3)) +
        'x'.repeat(maxFile % 3)]) {
      assert.equal(validFile(destination, 'top'), true);
      assert.equal(validFile(destination + 'x', 'top'), false);
      assert.equal(Buffer.byteLength(serialize(manifest('top', destination, '/resource/a'))
        .trim().split('\t')[1]) + 35, 255);
      assert.throws(() => manifest('top', destination + 'x', '/resource/a'));
    }
    const generation = 'g'.repeat(32);
    const maxOverlaySuffix = 255 - 35 - Buffer.byteLength(`themes/.active-${generation}/r0/`);
    const overlayThemes = suffix => themes.map(theme => ({ ...theme,
      files: [asset(`icons/${suffix}`)] }));
    assert.equal(plan(overlayThemes('x'.repeat(maxOverlaySuffix)), generation).copies.length, 1);
    assert.throws(() => plan(overlayThemes('x'.repeat(maxOverlaySuffix + 1)), generation));
    assert.throws(() => plan(themes, 'portable', {
      [`/resource/icons/${'x'.repeat(maxFile)}`]: 'low'
    }));

    const removed = [];
    const texts = new Map();
    const api = {
      async readOptionalText(uri) { return texts.get(uri) ?? null; },
      async writeText(uri, text) { texts.set(uri, text); },
      async removeDirectory(uri) { removed.push(uri); },
      isFileNotFound() { return false; }
    };
    const uri = 'internal://files/mappings.tsv';
    texts.set(uri, '/resource/a\tthemes/top/a\r\n');
    await assert.rejects(remove('top', api), error => error.resourceStorageReason === 'active-theme');
    assert.deepEqual(removed, []);
    texts.set(uri, '/resource/a\tthemes/top2/a\n/resource/b\t@system\n');
    await remove('top', api);
    assert.deepEqual(removed, ['internal://files/themes/top/']);
    texts.set(uri, '/resource/a\t/data/quickapp/files/ng.lst.corona/themes/top/a\n');
    await remove('top', api); // Absolute TSV destinations have no legacy interpretation.
    assert.equal(removed.length, 2);
    console.log('Portable relative TSV, overlays, overrides, deletion, traversal and byte-limit tests passed.');
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}

testPortableDestinations().then(() => console.log('All Manager host tests passed.')).catch(error => {
  console.error(error);
  process.exitCode = 1;
});
