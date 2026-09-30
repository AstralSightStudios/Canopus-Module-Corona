/* Host tests for persisted pack ordering and static overlay generation. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const temporary = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'resource-hook-order-')));
const orderUri = 'internal://files/resource-order.json';
const fileIndexUri = 'internal://files/resource-files.json';
const mappingsUri = 'internal://files/mappings.tsv';
const activeIndexUri = 'internal://files/resource-active-generations.json';
const systemId = '@system';

function manifest(themeId, mapping) {
  return JSON.stringify({
    format: 'canopus-resource-pack', formatVersion: 1, themeId,
    name: themeId, mappings: [mapping]
  });
}

function makeFileApi() {
  const text = new Map();
  const binary = new Map();
  const directories = new Set(['internal://files/']);
  const api = {
    text, binary, directories,
    async readOptionalText(uri) { return text.has(uri) ? text.get(uri) : null; },
    async writeText(uri, value) { text.set(uri, value); },
    async readArrayBuffer(uri, position = 0, length) {
      const value = binary.get(uri);
      if (!value) throw Object.assign(new Error(`missing ${uri}`), { code: 301 });
      return value.slice(position, length === undefined ? undefined : position + length);
    },
    async copyFile(sourceUri, destinationUri) {
      const value = binary.get(sourceUri);
      if (!value) throw Object.assign(new Error(`missing ${sourceUri}`), { code: 301 });
      binary.set(destinationUri, value.slice());
    },
    async writeArrayBuffer(uri, value, position = 0) {
      const previous = binary.get(uri) || new Uint8Array(0);
      const next = new Uint8Array(Math.max(previous.length, position + value.length));
      next.set(previous);
      next.set(value, position);
      binary.set(uri, next);
    },
    async readFileInfo(uri) {
      if (directories.has(uri)) return { length: 0, type: 'dir' };
      if (binary.has(uri)) return { length: binary.get(uri).length, type: 'file' };
      if (text.has(uri)) return { length: Buffer.byteLength(text.get(uri)), type: 'file' };
      throw Object.assign(new Error(`missing ${uri}`), { code: 301 });
    },
    async makeDirectory(uri) {
      if (directories.has(uri))
        throw Object.assign(new Error('file already exists'), { code: 300, data: 'file already exists' });
      let current = 'internal://';
      for (const segment of uri.slice('internal://'.length).split('/').filter(Boolean)) {
        current += `${segment}/`;
        directories.add(current);
      }
    },
    async deleteFile(uri) {
      if (!binary.delete(uri) && !text.delete(uri))
        throw Object.assign(new Error(`missing ${uri}`), { code: 301 });
    },
    async removeDirectory(uri) {
      const prefix = uri.endsWith('/') ? uri : `${uri}/`;
      let removed = false;
      for (const key of [...binary.keys()]) if (key.startsWith(prefix)) { binary.delete(key); removed = true; }
      for (const key of [...text.keys()]) if (key.startsWith(prefix)) { text.delete(key); removed = true; }
      for (const key of [...directories]) if (key === uri || key.startsWith(prefix)) directories.delete(key);
      if (!removed) throw Object.assign(new Error(`missing ${uri}`), { code: 301 });
    },
    isFileNotFound(error) { return error && error.code === 301; }
  };
  return api;
}

async function main() {
  execFileSync(path.join(root, 'manager/node_modules/.bin/tsc'), [
    path.join(root, 'manager/src/ts/resource-order.ts'),
    path.join(root, 'manager/src/ts/resource-activation.ts'),
    path.join(root, 'manager/src/ts/resource-pack.ts'),
    '--outDir', temporary, '--module', 'commonjs', '--target', 'es2018',
    '--lib', 'es2018,dom', '--skipLibCheck'
  ], { stdio: 'inherit' });
  const order = require(path.join(temporary, 'resource-order.js'));
  const activation = require(path.join(temporary, 'resource-activation.js'));

  {
    const file = makeFileApi();
    const initial = await order.loadResourceOrder(['base', 'weather'], file);
    assert.deepEqual(initial, ['base', 'weather', systemId]);
    assert.deepEqual(JSON.parse(file.text.get(orderUri)), { version: 1, order: initial });

    await order.registerThemeInResourceOrder('newpack', ['base', 'weather'], file);
    assert.deepEqual(await order.loadResourceOrder(['base', 'weather', 'newpack'], file),
      ['newpack', 'base', 'weather', systemId]);

    await order.registerThemeInResourceOrder('base', ['newpack', 'weather'], file);
    assert.deepEqual(await order.loadResourceOrder(['newpack', 'base', 'weather'], file),
      ['newpack', 'base', 'weather', systemId], 'replacing an ordered pack preserves its slot');

    await order.removeThemeFromResourceOrder('base', file);
    assert.deepEqual(await order.loadResourceOrder(['newpack', 'weather'], file),
      ['newpack', 'weather', systemId]);
    assert.throws(() => order.serializeResourceOrder(['same', 'same', systemId]), /不重复/);
    await assert.rejects(order.loadResourceOrder(['newpack'], {
      async readOptionalText() { return '{bad'; }, async writeText() {}
    }), /排序记录损坏/);
  }

  {
    const file = makeFileApi();
    file.text.set(orderUri, JSON.stringify({ version: 1, order: ['active', systemId, 'inactive'] }));
    await order.registerThemeInResourceOrder('newpack', ['active', 'inactive'], file);
    assert.deepEqual(JSON.parse(file.text.get(orderUri)).order,
      ['newpack', 'active', systemId, 'inactive'], 'installing a pack must not move the system boundary');
  }

  {
    const file = makeFileApi();
    file.text.set('internal://files/themes/top/canora.json', manifest('top', {
      source: '/resource/icons/', destination: 'icons/'
    }));
    file.text.set('internal://files/themes/base/canora.json', manifest('base', {
      source: '/resource/icons/', destination: 'other/'
    }));
    file.text.set('internal://files/themes/inactive/canora.json', '{not loaded}');
    file.text.set(fileIndexUri, JSON.stringify({ version: 1, themes: {
      top: [{ relativePath: 'icons/a.bin', sizeBytes: 2 }],
      base: [
        { relativePath: 'other/a.bin', sizeBytes: 1 },
        { relativePath: 'other/b.bin', sizeBytes: 1 }
      ]
    } }));
    file.binary.set('internal://files/themes/top/icons/a.bin', Uint8Array.from([9, 9]));
    file.binary.set('internal://files/themes/base/other/a.bin', Uint8Array.from([1]));
    file.binary.set('internal://files/themes/base/other/b.bin', Uint8Array.from([2]));

    const plan = await activation.regenerateActiveMappings(
      ['top', 'base', systemId, 'inactive'], 'g1', file);
    assert.equal(plan.generation, 'g1');
    assert.equal(plan.copies.length, 2, 'only winning file versions are materialized');
    assert.match(plan.mappings, /^\/resource\/icons\/\tthemes\/.active-g1\/r0\/\n$/);
    assert(!plan.mappings.includes('inactive'));
    assert.deepEqual([...file.binary.get('internal://files/themes/.active-g1/r0/a.bin')], [9, 9]);
    assert.deepEqual([...file.binary.get('internal://files/themes/.active-g1/r0/b.bin')], [2]);
    assert.equal(file.text.get(mappingsUri), plan.mappings);
    assert.deepEqual(JSON.parse(file.text.get(activeIndexUri)), { version: 1, generations: ['g1'] });

    await activation.cleanupInactiveGenerations('g1', file);
    file.text.set(activeIndexUri, JSON.stringify({ version: 1, generations: ['g1', 'g2'] }));
    await activation.cleanupInactiveGenerations('g2', file);
    assert(!file.directories.has('internal://files/themes/.active-g1/'));
    assert.deepEqual(JSON.parse(file.text.get(activeIndexUri)), { version: 1, generations: ['g2'] });
  }

  {
    const file = makeFileApi();
    const topManifest = manifest('top', { source: '/legacy/', destination: 'icons/' });
    const baseManifest = manifest('base', { source: '/legacy/', destination: 'assets/' });
    file.text.set('internal://files/themes/top/canora.json', topManifest);
    file.text.set('internal://files/themes/base/canora.json', baseManifest);
    const entries = {
      top: ['icons/top.bin'],
      base: ['assets/base.bin']
    };
    file.listDirectory = async uri => {
      const match = /themes\/(top|base)\/(.*)$/.exec(uri);
      if (!match) return [];
      const [, themeId, relative] = match;
      if (!relative) return [
        { uri: `${uri}canora.json`, length: Buffer.byteLength(themeId === 'top' ? topManifest : baseManifest) },
        { uri: `${uri}${themeId === 'top' ? 'icons/' : 'assets/'}`, length: 0 }
      ];
      if (relative === 'icons/' || relative === 'assets/')
        return entries[themeId].map(path => ({ uri: `internal://files/themes/${themeId}/${path}`, length: 1 }));
      return [];
    };
    file.readFileInfo = async uri => ({
      length: uri.endsWith('canora.json')
        ? Buffer.byteLength(uri.includes('/top/') ? topManifest : baseManifest) : (uri.endsWith('/') ? 0 : 1),
      type: uri.endsWith('/') ? 'dir' : 'file'
    });
    file.binary.set('internal://files/themes/top/icons/top.bin', Uint8Array.from([7]));
    file.binary.set('internal://files/themes/base/assets/base.bin', Uint8Array.from([3]));
    const plan = await activation.regenerateActiveMappings(['top', 'base', systemId], 'legacy1', file);
    assert.deepEqual(plan.copies.map(copy => copy.sourceUri), [
      'internal://files/themes/top/icons/top.bin',
      'internal://files/themes/base/assets/base.bin'
    ], 'activation uses an in-memory inventory when the optional index is absent');
    assert.equal(file.text.get(fileIndexUri), undefined,
      'rebuilding mappings does not require an optional inventory index write');
  }

  {
    const topManifest = {
      mappings: [{ source: '/resource/', destination: 'root/' }]
    };
    const baseManifest = {
      mappings: [{ source: '/resource/icons/', destination: 'icons/' }]
    };
    const plan = activation.planActiveMappings([
      { themeId: 'top', manifest: topManifest, files: [
        { relativePath: 'root/icons/a.bin', sizeBytes: 1 },
        { relativePath: 'root/other.bin', sizeBytes: 1 }
      ] },
      { themeId: 'base', manifest: baseManifest, files: [
        { relativePath: 'icons/a.bin', sizeBytes: 1 },
        { relativePath: 'icons/b.bin', sizeBytes: 1 }
      ] }
    ], 'g-prefix');
    assert.equal(plan.mappings.split('\n').filter(Boolean).length, 1,
      'overlapping broad and nested sources collapse to one ordered rule');
    assert(plan.mappings.startsWith('/resource/\t'));
    assert.deepEqual(plan.copies.map(copy => copy.sourceUri), [
      'internal://files/themes/top/root/icons/a.bin',
      'internal://files/themes/top/root/other.bin',
      'internal://files/themes/base/icons/b.bin'
    ]);
  }

  {
    const plan = activation.planActiveMappings([
      { themeId: 'top', manifest: { mappings: [
        { source: '/resource/', destination: 'root/' },
        { source: '/resource/icons/', destination: 'specific/' }
      ] }, files: [
        { relativePath: 'root/icons/a.bin', sizeBytes: 1 },
        { relativePath: 'specific/other.bin', sizeBytes: 1 }
      ] },
      { themeId: 'base', manifest: { mappings: [
        { source: '/resource/icons/', destination: 'icons/' }
      ] }, files: [{ relativePath: 'icons/a.bin', sizeBytes: 1 }] }
    ], 'g-specific');
    assert(plan.copies.some(copy => copy.sourceUri === 'internal://files/themes/base/icons/a.bin'));
    assert(!plan.copies.some(copy => copy.sourceUri === 'internal://files/themes/top/root/icons/a.bin'),
      'a broad rule must not bypass a more-specific rule with a missing file');
  }

  {
    const file = makeFileApi();
    file.text.set('internal://files/themes/top/canora.json', manifest('top', {
      source: '/resource/', destination: 'top/'
    }));
    file.text.set('internal://files/themes/picked/canora.json', manifest('picked', {
      source: '/resource/', destination: 'picked/'
    }));
    file.text.set(fileIndexUri, JSON.stringify({ version: 1, themes: {
      top: [
        { relativePath: 'top/a.bin', sizeBytes: 1 },
        { relativePath: 'top/b.bin', sizeBytes: 1 },
        { relativePath: 'top/c.bin', sizeBytes: 1 }
      ],
      picked: [
        { relativePath: 'picked/a.bin', sizeBytes: 1 },
        { relativePath: 'picked/b.bin', sizeBytes: 1 },
        { relativePath: 'picked/c.bin', sizeBytes: 1 }
      ]
    } }));
    file.binary.set('internal://files/themes/top/top/a.bin', Uint8Array.from([1]));
    file.binary.set('internal://files/themes/top/top/b.bin', Uint8Array.from([2]));
    file.binary.set('internal://files/themes/top/top/c.bin', Uint8Array.from([5]));
    file.binary.set('internal://files/themes/picked/picked/a.bin', Uint8Array.from([3]));
    file.binary.set('internal://files/themes/picked/picked/b.bin', Uint8Array.from([4]));
    file.binary.set('internal://files/themes/picked/picked/c.bin', Uint8Array.from([6]));
    const plan = await activation.regenerateActiveMappings(
      ['top', systemId, 'picked'], 'g-picked', file,
      { '/resource/a.bin': 'picked', '/resource/b.bin': '@system' });
    assert.equal(plan.generation, null);
    assert.equal(plan.copies.length, 0,
      'per-file choices use direct TSV rules instead of materializing another overlay');
    assert.equal(file.text.get(mappingsUri),
      `/resource/\tthemes/top/top/\n` +
      `/resource/a.bin\tthemes/picked/picked/a.bin\n` +
      `/resource/b.bin\t@system\n`,
      'explicit pack and system choices override the normal ordered directory rule');
  }

  {
    const file = makeFileApi();
    file.text.set(mappingsUri, 'old active config');
    file.text.set('internal://files/themes/top/canora.json', manifest('top', {
      source: '/resource/', destination: 'top/'
    }));
    file.text.set('internal://files/themes/base/canora.json', manifest('base', {
      source: '/resource/', destination: 'base/'
    }));
    file.text.set(fileIndexUri, JSON.stringify({ version: 1, themes: {
      top: [{ relativePath: 'top/a.bin', sizeBytes: 1 }],
      base: [{ relativePath: 'base/a.bin', sizeBytes: 1 }]
    } }));
    file.binary.set('internal://files/themes/base/base/a.bin', Uint8Array.from([1]));
    await assert.rejects(activation.regenerateActiveMappings(['top', 'base', systemId], 'g-fail', file));
    assert.equal(file.text.get(mappingsUri), 'old active config', 'failed preparation must not publish config');
    assert.deepEqual(JSON.parse(file.text.get(activeIndexUri)), { version: 1, generations: [] });
    assert(!file.directories.has('internal://files/themes/.active-g-fail/'));
  }

  {
    const grouped = [
      { themeId: 'top', manifest: { mappings: [{ source: '/same/', destination: 'a/' }] }, files: null },
      { themeId: 'base', manifest: { mappings: [{ source: '/same/', destination: 'b/' }] }, files: null }
    ];
    assert.throws(() => activation.planActiveMappings(grouped, 'g3'), /缺少文件清单/);
    const unique = activation.planActiveMappings([
      { themeId: 'top', manifest: { mappings: [{ source: '/one.bin', destination: 'one.bin' }] }, files: null }
    ], 'g4');
    assert.equal(unique.generation, null);
    assert.equal(unique.copies.length, 0);
  }

  {
    const plan = activation.planActiveMappings([
      { themeId: 'top', active: true, manifest: { mappings: [
        { source: '/resource/', destination: 'top/' }
      ] }, files: [
        { relativePath: 'top/a.bin', sizeBytes: 1 },
        { relativePath: 'top/b.bin', sizeBytes: 1 }
      ] },
      { themeId: 'low', active: false, manifest: { mappings: [
        { source: '/resource/', destination: 'low/' }
      ] }, files: [
        { relativePath: 'low/a.bin', sizeBytes: 1 },
        { relativePath: 'low/b.bin', sizeBytes: 1 }
      ] }
    ], 'g-override', { '/resource/a.bin': 'low' });
    assert.equal(plan.copies.length, 0,
      'an explicit per-file choice does not copy either the selected or default assets');
    assert.equal(plan.mappings,
      `/resource/\tthemes/top/top/\n` +
      `/resource/a.bin\tthemes/low/low/a.bin\n`,
      'an explicit pack selection wins below the system boundary while others follow normal order');
  }

  {
    const plan = activation.planActiveMappings([
      { themeId: 'top', active: true, manifest: { mappings: [
        { source: '/resource/', destination: 'top/' }
      ] }, files: null }
    ], 'g-system', { '/resource/a.bin': '@system' });
    assert.equal(plan.copies.length, 0);
    assert.equal(plan.mappings,
      `/resource/\tthemes/top/top/\n` +
      `/resource/a.bin\t@system\n`,
      'the module-level System rule masks the parent directory rule without a fake target');
  }

  {
    const oversizedSuffix = 'x'.repeat(250);
    const plan = activation.planActiveMappings([
      { themeId: 'top', active: true, manifest: { mappings: [
        { source: '/resource/', destination: 'top/' }
      ] }, files: [
        { relativePath: 'top/a.bin', sizeBytes: 1 },
        { relativePath: `top/${oversizedSuffix}`, sizeBytes: 1 }
      ] },
      { themeId: 'low', active: true, manifest: { mappings: [
        { source: '/resource/', destination: 'low/' }
      ] }, files: [
        { relativePath: 'low/a.bin', sizeBytes: 1 },
        { relativePath: `low/${oversizedSuffix}`, sizeBytes: 1 }
      ] }
    ], 'g-long-path', { '/resource/a.bin': 'low' });
    assert.deepEqual(plan.copies.map(copy => copy.sourceUri), [
      'internal://files/themes/top/top/a.bin'
    ], 'unrepresentable source paths are skipped while direct per-file overrides add no copies');
  }

  {
    const manyThemes = [];
    const overrides = {};
    for (let i = 0; i < 256; i++) {
      const source = `/resource/${i}.bin`;
      const themeId = `p${i}`;
      manyThemes.push({ themeId, active: true, manifest: { mappings: [
        { source, destination: `${i}.bin` }
      ] }, files: null });
      overrides[source] = '@system';
    }
    const plain = activation.planActiveMappings(manyThemes, 'g-many');
    assert.equal(plain.mappings.split('\n').filter(Boolean).length, 256,
      '256 unique ordered mappings fit the module rule limit');
    const plan = activation.planActiveMappings(manyThemes, 'g-all-system', overrides);
    assert.equal(plan.mappings.split('\n').filter(Boolean).length, 256,
      'System pass-through entries count against the module rule limit');
    const source = '/resource/256.bin';
    manyThemes.push({ themeId: 'p256', active: true, manifest: { mappings: [
      { source, destination: '256.bin' }
    ] }, files: null });
    overrides[source] = '@system';
    assert.throws(() => activation.planActiveMappings(manyThemes, 'g-too-many'), /256 条/);
    assert.throws(() => activation.planActiveMappings(manyThemes, 'g-too-many', overrides), /256 条/);
  }

  {
    const file = makeFileApi();
    const capacityMappings = Array.from({ length: 256 }, (_, index) => ({
      source: `/resource/${index}.bin`, destination: 'shared.bin'
    }));
    const capacityManifest = { ...JSON.parse(manifest('capacity', capacityMappings[0])),
      mappings: capacityMappings };
    file.text.set('internal://files/themes/capacity/canora.json', JSON.stringify(capacityManifest));
    const plan = await activation.regenerateActiveMappings(['capacity', systemId], 'g-capacity', file);
    assert.equal(plan.mappings.split('\n').filter(Boolean).length, 256);
    assert.equal(file.text.get(mappingsUri), plan.mappings);
    file.text.set('internal://files/themes/extra/canora.json', manifest('extra', {
      source: '/resource/extra.bin', destination: 'extra.bin'
    }));
    await assert.rejects(activation.regenerateActiveMappings(
      ['capacity', 'extra', systemId], 'g-over-capacity', file), /256 条/);
    assert.equal(file.text.get(mappingsUri), plan.mappings,
      'merging 257 valid package rules must not publish a partial active config');
  }

  {
    const file = makeFileApi();
    const themeIds = ['budgeta', 'budgetb'];
    const sourceBytes = 128 - Buffer.byteLength('\tthemes/budgeta/shared.bin\n');
    const rules = Array.from({ length: 256 }, (_, index) => {
      const prefix = `/resource/${String(index).padStart(3, '0')}/`;
      const remaining = sourceBytes - Buffer.byteLength(prefix);
      return { source: prefix + '中'.repeat(Math.floor(remaining / 3)) + 'x'.repeat(remaining % 3),
        destination: 'shared.bin' };
    });
    themeIds.forEach((themeId, index) => file.text.set(
      `internal://files/themes/${themeId}/canora.json`, JSON.stringify({
        ...JSON.parse(manifest(themeId, rules[0])), mappings: rules.slice(index * 128, (index + 1) * 128)
      })));
    const plan = await activation.regenerateActiveMappings([...themeIds, systemId], 'g-budget', file);
    assert.equal(Buffer.byteLength(plan.mappings), 32 * 1024);
    assert.ok(plan.mappings.length < 32 * 1024, 'TSV limit uses bytes, not JS string length');
    const firstUri = 'internal://files/themes/budgeta/canora.json';
    const oversized = JSON.parse(file.text.get(firstUri));
    oversized.mappings[0].source += 'x';
    file.text.set(firstUri, JSON.stringify(oversized));
    await assert.rejects(activation.regenerateActiveMappings(
      [...themeIds, systemId], 'g-over-budget', file), /超过 32 KiB/);
    assert.equal(file.text.get(mappingsUri), plan.mappings,
      'valid per-pack TSV budgets must not bypass the merged active 32 KiB budget');
  }

  console.log('Resource order, 256-rule capacity, merged TSV byte budget, overlays and generation cleanup passed.');
}

main().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => {
  fs.rmSync(temporary, { recursive: true, force: true });
});
