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

async function testQuickAppIcons(temporary) {
  const assert = require('node:assert/strict');
  const { parseResourcePackManifest: parse, serializeResourcePackMappings: serialize,
    validateResourcePackFiles: validateFiles } = require(path.join(temporary, 'resource-pack.js'));
  const { planActiveMappings: plan, regenerateActiveMappings: regenerate } =
    require(path.join(temporary, 'resource-activation.js'));
  const { validSourcePath, loadResourcePackSnapshot: loadSnapshot, saveResourceOverride: save,
    loadResourceOverrides: load, reconcileResourceOverrides: reconcile } =
    require(path.join(temporary, 'resource-overrides.js'));
  const { safeRelativeResourcePath, safeAbsoluteResourcePath } =
    require(path.join(temporary, 'resource-path.js'));
  const { validateThemeRelativePath } = require(path.join(temporary, 'interconnect.js'));
  const key = '@quickapp-icon/ng.lst.corona';
  const base = { format: 'canopus-resource-pack', formatVersion: 1, themeId: 'top',
    name: 'Icon pack', version: '1.2', author: 'Author', description: 'Package icons',
    targets: ['band11'], mappings: [],
    quickappIcons: [{ package: 'ng.lst.corona', destination: 'icons/corona.bin' }] };
  const parseObject = value => parse(JSON.stringify(value));
  const pack = parseObject(base);
  assert.deepEqual(pack.mappings, [{ source: key, destination: 'icons/corona.bin' }]);
  assert.equal(serialize(pack), `${key}\tthemes/top/icons/corona.bin\n`);
  assert.deepEqual(parseObject(pack), pack, 'canonical manifests remain idempotent after JSON serialization');
  for (const field of ['name', 'version', 'author', 'description', 'targets'])
    assert.deepEqual(pack[field], base[field], `normalization retains ${field}`);
  assert.equal(safeRelativeResourcePath(key), false, 'virtual keys are not relative file paths');
  assert.equal(safeAbsoluteResourcePath(key), false, 'virtual keys are not native resource paths');
  assert.equal(validateThemeRelativePath(key, 'top'), false, 'transfer paths cannot carry semantic keys');
  assert.equal(validSourcePath(key), true);
  assert.doesNotThrow(() => validateFiles(pack, ['canora.json', 'icons/corona.bin']));
  assert.throws(() => validateFiles(pack, ['icons/corona.bin/child']), /不存在/);
  assert.throws(() => parseObject({ ...base, mappings: undefined }), /mappings/);
  assert.throws(() => parseObject({ ...base, quickappIcons: null }), /quickappIcons/);
  assert.throws(() => parseObject({ ...base, quickappIcons: [{}] }), /package/);
  assert.throws(() => parseObject({ ...base, mappings: pack.mappings }), /重复/);
  assert.deepEqual(parseObject({ ...base, mappings: pack.mappings, quickappIcons: undefined }), pack);

  for (const packageId of ['a.b', '_a.1-a.B_2', `a.${'x'.repeat(125)}`]) {
    const source = `@quickapp-icon/${packageId}`;
    assert.equal(validSourcePath(source), true);
    assert.equal(parseObject({ ...base, quickappIcons: [{ package: packageId,
      destination: 'icons/icon.bin' }] }).mappings[0].source, source);
  }
  for (const packageId of ['', 'single', '.a.b', 'a.b.', 'a..b', 'a.-b', '-a.b',
    'a/b.c', 'a\\b.c', 'a:b.c', 'a.b\n', 'a.b\r', 'a.b\t', 'a.b\0', '中.a',
    'a b.c', `a.${'x'.repeat(126)}`, null, 42]) {
    assert.throws(() => parseObject({ ...base, quickappIcons: [{ package: packageId,
      destination: 'icons/corona.bin' }] }), /package/);
    assert.equal(validSourcePath(`@quickapp-icon/${packageId}`), false);
  }
  for (const source of ['@arbitrary/a.b', '@system', '@quickapp-icon/', `${key}/`,
    `${key}/child.bin`, `@quickapp-icon/${'x'.repeat(256)}`]) {
    assert.equal(validSourcePath(source), false);
    assert.throws(() => parseObject({ ...base, quickappIcons: [],
      mappings: [{ source, destination: 'icons/corona.bin' }] }));
    assert.throws(() => plan([], 'icons', { [source]: '@system' }), /路径无效/);
  }
  for (const destination of ['icons/', 'icons/corona.png', 'icons/corona.BIN',
    'icons/corona.Bin', 'icons/corona.bin/',
    '/absolute.bin', '../escape.bin', '@system', key, 'icons/a\tb.bin']) {
    assert.throws(() => parseObject({ ...base, quickappIcons: [{ package: 'ng.lst.corona', destination }] }));
    assert.throws(() => plan([{ themeId: 'top', files: null,
      manifest: { ...pack, mappings: [{ source: key, destination }] } }], 'icons'), /路径无效/);
  }

  // Native rules and virtual icon rules share both module capacity limits.
  const nativeMappings = Array.from({ length: 255 }, (_, index) => ({
    source: `/resource/${index}.bin`, destination: 'shared.bin'
  }));
  assert.equal(parseObject({ ...base, mappings: nativeMappings }).mappings.length, 256);
  assert.throws(() => parseObject({ ...base, mappings: [...nativeMappings, {
    source: '/resource/extra.bin', destination: 'shared.bin'
  }] }), /最多 256 条/);
  const icons = Array.from({ length: 256 }, (_, index) => ({
    package: `p${index}.icon`, destination: 'shared.bin'
  }));
  assert.equal(parseObject({ ...base, quickappIcons: icons }).mappings.length, 256);
  assert.throws(() => parseObject({ ...base, quickappIcons: [...icons, {
    package: 'p256.icon', destination: 'shared.bin'
  }] }), /最多 256 条/);
  const sourceBytes = 128 - Buffer.byteLength('\tthemes/top/shared.bin\n');
  const budget = icons.map((icon, index) => {
    const prefix = index % 2 ? `@quickapp-icon/p${index}.` : `/resource/${index}/`;
    return { source: prefix + 'x'.repeat(sourceBytes - prefix.length), destination: icon.destination };
  });
  const mixedBudget = { ...base,
    mappings: budget.filter((_rule, index) => index % 2 === 0),
    quickappIcons: budget.filter((_rule, index) => index % 2 === 1).map(rule => ({
      package: rule.source.slice('@quickapp-icon/'.length), destination: rule.destination
    })) };
  assert.equal(Buffer.byteLength(serialize(parseObject(mixedBudget))), 32 * 1024);
  assert.throws(() => parseObject({ ...mixedBudget, quickappIcons: mixedBudget.quickappIcons.map(
    (icon, index) => index === 0 ? { ...icon, package: icon.package + 'x' } : icon) }), /超过 32 KiB/);
  const full = parseObject({ ...base, mappings: budget, quickappIcons: undefined });
  assert.equal(Buffer.byteLength(serialize(full)), 32 * 1024);
  assert.equal(plan([{ themeId: 'top', manifest: full, files: null }], 'icons').mappings, serialize(full));
  assert.throws(() => parseObject({ ...base, mappings: budget.map((rule, index) =>
    index === 1 ? { ...rule, source: rule.source + 'x' } : rule), quickappIcons: undefined }), /超过 32 KiB/);

  const maxIconDestination = 255 - 35 - Buffer.byteLength('themes/top/');
  const longestBin = 'x'.repeat(maxIconDestination - 4) + '.bin';
  assert.doesNotThrow(() => parseObject({ ...base, quickappIcons: [{
    package: 'ng.lst.corona', destination: longestBin
  }] }));
  assert.throws(() => parseObject({ ...base, quickappIcons: [{
    package: 'ng.lst.corona', destination: 'x' + longestBin
  }] }), /路径限制/);

  const low = parseObject({ ...base, themeId: 'low', name: 'Low' });
  const themes = [{ themeId: 'top', manifest: pack, files: null },
    { themeId: 'low', manifest: low, files: null }];
  assert.equal(plan(themes, 'icons').mappings, serialize(pack));
  assert.equal(plan(themes, 'icons', { [key]: 'low' }).mappings, serialize(low));
  assert.equal(plan(themes, 'icons', { [key]: '@system' }).mappings, `${key}\t@system\n`);
  assert.equal(plan(themes, 'icons', { [key]: '@default' }).mappings, serialize(pack));
  const overlayThemes = themes.map(theme => ({ ...theme,
    manifest: { ...theme.manifest, mappings: [...theme.manifest.mappings,
      { source: '/resource/icons/', destination: 'icons/' }] },
    files: Array.from({ length: 257 }, (_, index) => ({ relativePath: `icons/${index}.bin`, sizeBytes: 1 })) }));
  const fallback = plan(overlayThemes, 'icons', { [key]: '@system' });
  assert.ok(fallback.mappings.includes(`${key}\t@system\n`));
  assert.ok(fallback.mappings.includes('/resource/icons/\tthemes/.active-icons/'));
  assert.equal(fallback.copies.length, 257);
  assert.ok(fallback.copies.every(copy => !copy.destinationUri.includes('@quickapp-icon')));

  const texts = new Map([
    ['internal://files/themes/top/canora.json', JSON.stringify(base)],
    ['internal://files/themes/low/canora.json', JSON.stringify({ ...base, themeId: 'low', name: 'Low' })],
    ['internal://files/resource-files.json', JSON.stringify({ version: 1, themes: {
      top: [{ relativePath: 'icons/corona.bin', sizeBytes: 1 }],
      low: [{ relativePath: 'icons/corona.bin', sizeBytes: 1 }]
    } })]
  ]);
  const api = {
    async readOptionalText(uri) { return texts.get(uri) ?? null; },
    async writeText(uri, text) { texts.set(uri, text); },
    async readFileInfo() { throw new Error('semantic keys must not trigger native resource lookups'); },
    async copyFile() { throw new Error('semantic keys must not require overlays'); }
  };
  const snapshot = await loadSnapshot(['top', 'low'], api);
  assert.deepEqual(snapshot.paths.map(item => item.sourcePath), [key]);
  assert.deepEqual(snapshot.byPath.get(key).themes.map(option => [option.name, option.previewUri]), [
    ['Icon pack', 'internal://files/themes/top/icons/corona.bin'],
    ['Low', 'internal://files/themes/low/icons/corona.bin']
  ]);
  await save(key, 'low', api);
  let overrides = await reconcile(snapshot.paths, api);
  assert.equal(overrides[key], 'low');
  assert.equal((await regenerate(['top', '@system', 'low'], 'icons', api, overrides, snapshot)).mappings,
    serialize(low), 'inactive packages remain selectable by exact semantic key');
  await save(key, '@system', api);
  overrides = await load(api);
  assert.equal((await regenerate(['top', 'low', '@system'], 'icons', api, overrides, snapshot)).mappings,
    `${key}\t@system\n`);
  await assert.rejects(save('@unknown/a.b', '@system', api), /无效/);
  texts.set('internal://files/resource-overrides.json', JSON.stringify({ version: 1,
    overrides: { '@unknown/a.b': '@system' } }));
  await assert.rejects(load(api), /无效/);
  console.log('QuickApp icon normalization, validation, combined budgets, semantic catalog and system-mask tests passed.');
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
      '/resource/icons/a.bin\tthemes/low/icons/a.bin\n' +
      '/resource/icons/b.bin\t@system\n');
    assert.equal(overlay.copies.length, 0);
    assert.equal(overlay.generation, null);
    assert.ok(!overlay.mappings.includes('/data/'));
    const fallback = plan([
      themes[0],
      { themeId: 'low', manifest: low, files: Array.from({ length: 257 }, (_, index) =>
        asset(`icons/icon${index}.bin`)) }
    ], 'portable');
    assert.equal(fallback.mappings, '/resource/icons/\tthemes/.active-portable/r0/\n');
    assert.equal(fallback.copies.length, 258);
    assert.ok(fallback.copies.every(copy => copy.destinationUri.startsWith(
      'internal://files/themes/.active-portable/r0/')));
    assert.ok(!fallback.mappings.includes('/data/'));

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
    const overlayThemes = (suffix, forceFallback = false) => themes.map(theme => ({ ...theme,
      files: [asset(`icons/${suffix}`), ...(forceFallback ? Array.from({ length: 256 }, (_, index) =>
        asset(`icons/padding${index}.bin`)) : [])] }));
    assert.equal(plan(overlayThemes('x'.repeat(maxOverlaySuffix + 1)), generation).copies.length, 0,
      'direct pack paths need not satisfy the longer overlay-generation path budget');
    assert.equal(plan(overlayThemes('x'.repeat(maxOverlaySuffix), true), generation).copies.length, 257);
    assert.throws(() => plan(overlayThemes('x'.repeat(maxOverlaySuffix + 1), true), generation));
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
    await testQuickAppIcons(temporary);
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}

testPortableDestinations().then(() => console.log('All Manager host tests passed.')).catch(error => {
  console.error(error);
  process.exitCode = 1;
});
