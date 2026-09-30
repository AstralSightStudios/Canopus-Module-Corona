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
    const { parseResourcePackManifest: parse, serializeResourcePackMappings: serialize,
      validateResourcePackFiles: validateFiles } = require(path.join(temporary, 'resource-pack.js'));
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
    for (const source of [`/${'x'.repeat(254)}`, `/${'中'.repeat(84)}xx`]) {
      assert.equal(Buffer.byteLength(source), 255);
      const pack = manifest('top', 'a.bin', source);
      assert.equal(plan([{ themeId: 'top', manifest: pack, files: null }], 'portable').mappings,
        serialize(pack));
      assert.throws(() => manifest('top', 'a.bin', source + 'x'), /source.*255/);
      assert.throws(() => plan([{ themeId: 'top', files: null, manifest: {
        ...pack, mappings: [{ source: source + 'x', destination: 'a.bin' }]
      } }], 'portable'), /路径无效/);
    }

    const packObject = mappings => ({
      format: 'canopus-resource-pack', formatVersion: 1, themeId: 'top', name: 'top', mappings
    });
    const parseMappings = mappings => parse(JSON.stringify(packObject(mappings)));
    const capacityMappings = Array.from({ length: 256 }, (_, index) => ({
      source: `/resource/${index}.bin`, destination: 'shared.bin'
    }));
    const capacityPack = parseMappings(capacityMappings);
    assert.equal(capacityPack.mappings.length, 256);
    assert.equal(serialize(capacityPack).split('\n').filter(Boolean).length, 256);
    assert.doesNotThrow(() => validateFiles(capacityPack, ['canora.json', 'shared.bin']),
      '256 source aliases may share one transmitted resource file');
    assert.equal(plan([{ themeId: 'top', manifest: capacityPack, files: null }], 'portable').mappings,
      serialize(capacityPack));
    assert.throws(() => parseMappings([...capacityMappings, {
      source: '/resource/256.bin', destination: 'shared.bin'
    }]), /最多 256 条/);

    // Rule capacity and TSV byte budget are independent, including multibyte sources.
    for (const multibyte of [false, true]) {
      const sourceBytes = 128 - Buffer.byteLength('\tthemes/top/shared.bin\n');
      const budgetMappings = capacityMappings.map((mapping, index) => {
        const prefix = `/resource/${String(index).padStart(3, '0')}/`;
        const remaining = sourceBytes - Buffer.byteLength(prefix);
        return { ...mapping, source: prefix + (multibyte
          ? '中'.repeat(Math.floor(remaining / 3)) + 'x'.repeat(remaining % 3)
          : 'x'.repeat(remaining)) };
      });
      const budgetPack = parseMappings(budgetMappings);
      const tsv = serialize(budgetPack);
      assert.equal(Buffer.byteLength(tsv), 32 * 1024);
      if (multibyte) assert.ok(tsv.length < 32 * 1024);
      assert.equal(plan([{ themeId: 'top', manifest: budgetPack, files: null }], 'portable').mappings,
        tsv, 'exactly 32 KiB remains valid with 256 rules');
      const oversized = budgetMappings.map((mapping, index) =>
        index === 0 ? { ...mapping, source: mapping.source + 'x' } : mapping);
      assert.equal(Buffer.byteLength(serialize({ ...budgetPack, mappings: oversized })), 32 * 1024 + 1);
      assert.throws(() => parseMappings(oversized), /超过 32 KiB/);
      assert.throws(() => plan([{ themeId: 'top', files: null, manifest: {
        ...budgetPack, mappings: oversized
      } }], 'portable'), /超过 32 KiB/);
    }

    // The larger rule cap must not relax the manifest or metadata byte limits.
    const manifestBase = JSON.stringify({ ...packObject([]), padding: '' });
    const paddingBytes = 64 * 1024 - Buffer.byteLength(manifestBase);
    const fullManifest = JSON.stringify({ ...packObject([]),
      padding: '中'.repeat(Math.floor(paddingBytes / 3)) + 'x'.repeat(paddingBytes % 3) });
    assert.equal(Buffer.byteLength(fullManifest), 64 * 1024);
    assert.doesNotThrow(() => parse(fullManifest));
    assert.throws(() => parse(fullManifest + ' '), /超过 64 KiB/);
    assert.doesNotThrow(() => parse(JSON.stringify({ ...packObject([]), version: 'v'.repeat(64) })));
    assert.throws(() => parse(JSON.stringify({ ...packObject([]), version: 'v'.repeat(65) })), /version/);

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
    console.log('Resource-pack 256-rule capacity, TSV/manifest byte budgets, portable paths, overlays and deletion tests passed.');
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}

testPortableDestinations().then(() => console.log('All Manager host tests passed.')).catch(error => {
  console.error(error);
  process.exitCode = 1;
});
