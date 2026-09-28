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
    async writeArrayBuffer(uri, value, position = 0) {
      const previous = binary.get(uri) || new Uint8Array(0);
      const next = new Uint8Array(Math.max(previous.length, position + value.length));
      next.set(previous);
      next.set(value, position);
      binary.set(uri, next);
    },
    async makeDirectory(uri) {
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
    assert.match(plan.mappings, /^\/resource\/icons\/\t\/data\/quickapp\/files\/ng\.lst\.corona\/themes\/.active-g1\/r0\/\n$/);
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
    await activation.regenerateActiveMappings(['top', 'base', systemId], 'legacy1', file);
    const inventory = JSON.parse(file.text.get(fileIndexUri)).themes;
    assert.deepEqual(inventory.top.map(item => item.relativePath), ['canora.json', 'icons/top.bin']);
    assert.deepEqual(inventory.base.map(item => item.relativePath), ['canora.json', 'assets/base.bin']);
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

  console.log('Resource order persistence, new-pack priority, pack overlays and active generation cleanup passed.');
}

main().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => {
  fs.rmSync(temporary, { recursive: true, force: true });
});
