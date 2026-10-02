/* Host regressions for direct activation, snapshot caching and native overlay barriers. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const themeRoot = 'internal://files/themes/';
const inventoryUri = 'internal://files/resource-files.json';
const installedUri = 'internal://files/interconnect-themes.json';
const mappingsUri = 'internal://files/mappings.tsv';
const generationsUri = 'internal://files/resource-active-generations.json';
const oldMappings = '/resource/\tthemes/.active-old/r0/\n';
const manifestUri = id => `${themeRoot}${id}/corona.json`;
const generationRoot = id => `${themeRoot}.active-${id}/`;

function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
const drainCallbacks = () => new Promise(resolve => setImmediate(resolve));

function makeFile() {
  const text = new Map();
  const binary = new Map();
  const directories = new Set();
  const calls = [];
  const listeners = [];
  const file = {
    text, binary, directories, calls,
    record(method, uri, extra = {}) {
      calls.push({ method, uri, ...extra });
      for (const listener of [...listeners]) listener();
    },
    count(method, uri) {
      return calls.filter(call => call.method === method && (uri === undefined || call.uri === uri)).length;
    },
    when(method, uri, count = 1) {
      if (file.count(method, uri) >= count) return Promise.resolve();
      const gate = deferred();
      const listener = () => {
        if (file.count(method, uri) < count) return;
        listeners.splice(listeners.indexOf(listener), 1);
        gate.resolve();
      };
      listeners.push(listener);
      return gate.promise;
    },
    async readOptionalText(uri) {
      file.record('read', uri);
      const value = text.has(uri) ? text.get(uri) : null;
      return file.onRead ? file.onRead(uri, value) : value;
    },
    async writeText(uri, value) {
      file.record('write', uri, { value });
      text.set(uri, value);
    },
    async readFileInfo(uri, recursive) {
      file.record('info', uri, { recursive });
      if (file.onInfo) {
        const value = await file.onInfo(uri, recursive);
        if (value !== undefined) return value;
      }
      if (binary.has(uri)) return { length: binary.get(uri).length, type: 'file' };
      if (directories.has(uri)) return { length: 0, type: 'dir' };
      throw Object.assign(new Error(`Missing metadata: ${uri}`), { code: 301 });
    },
    async listDirectory(uri) {
      file.record('list', uri);
      if (file.onList) return file.onList(uri);
      throw new Error(`Unexpected enumeration: ${uri}`);
    },
    async copyFile(srcUri, dstUri) {
      file.record('copy', srcUri, { dstUri });
      if (file.onCopy) await file.onCopy(srcUri, dstUri);
      assert(binary.has(srcUri), `Native source exists: ${srcUri}`);
      binary.set(dstUri, binary.get(srcUri).slice());
      if (file.afterCopy) await file.afterCopy(srcUri, dstUri);
      file.record('copied', dstUri);
    },
    async makeDirectory(uri, recursive) {
      file.record('mkdir', uri, { recursive });
      assert.equal(recursive, true);
      assert(!directories.has(uri), `Directory must be prepared only once: ${uri}`);
      directories.add(uri);
    },
    async removeDirectory(uri) {
      file.record('remove', uri);
      for (const key of [...binary.keys()]) if (key.startsWith(uri)) binary.delete(key);
      for (const key of [...text.keys()]) if (key.startsWith(uri)) text.delete(key);
      for (const key of [...directories]) if (key.startsWith(uri)) directories.delete(key);
    },
    async readArrayBuffer(uri) { file.record('binaryRead', uri); throw new Error('Manual copy forbidden'); },
    async writeArrayBuffer(uri) { file.record('binaryWrite', uri); throw new Error('Manual copy forbidden'); },
    async deleteFile(uri) { file.record('delete', uri); throw new Error('Manual destination deletion forbidden'); },
    isFileNotFound(error) { return error && error.code === 301; }
  };
  text.set(mappingsUri, oldMappings);
  text.set(generationsUri, JSON.stringify({ version: 1, generations: ['old'] }));
  binary.set(`${generationRoot('old')}r0/retained.bin`, Uint8Array.of(99));
  return file;
}

function manifest(id, mappings = [{ source: '/resource/', destination: 'assets/' }], name = id) {
  return JSON.stringify({ format: 'canopus-resource-pack', formatVersion: 1, themeId: id, name, mappings });
}

function indexedFile(ids = ['top', 'base'], assets = [
  { relativePath: 'assets/a.bin', sizeBytes: 2 },
  { relativePath: 'assets/b.bin', sizeBytes: 3 },
  { relativePath: 'assets/nested/c.bin', sizeBytes: 4 }
]) {
  const file = makeFile();
  const themes = {};
  file.text.set(installedUri, JSON.stringify(ids));
  ids.forEach((id, index) => {
    file.text.set(manifestUri(id), manifest(id));
    themes[id] = assets.map(asset => ({ ...asset }));
    for (const asset of assets) {
      file.binary.set(`${themeRoot}${id}/${asset.relativePath}`, new Uint8Array(asset.sizeBytes).fill(index + 1));
    }
  });
  file.text.set(inventoryUri, JSON.stringify({ version: 1, themes }));
  return file;
}

function fallbackFile(ids = ['top', 'base']) {
  return indexedFile(ids, [
    { relativePath: 'assets/a.bin', sizeBytes: 2 },
    { relativePath: 'assets/b.bin', sizeBytes: 3 },
    ...Array.from({ length: 254 }, (_, index) => ({ relativePath: `assets/extra${index}.bin`, sizeBytes: 1 })),
    { relativePath: 'assets/nested/c.bin', sizeBytes: 4 }
  ]);
}

function assertNativeOnly(file) {
  for (const method of ['binaryRead', 'binaryWrite', 'delete']) assert.equal(file.count(method), 0, method);
}

function assertRolledBack(file, generation) {
  assert.equal(file.text.get(mappingsUri), oldMappings, 'failed reload retains old TSV verbatim');
  assert.equal(file.count('write', mappingsUri), 0, 'failed reload never publishes a TSV');
  assert.deepEqual(JSON.parse(file.text.get(generationsUri)), { version: 1, generations: ['old'] });
  assert.deepEqual(file.calls.filter(call => call.method === 'remove').map(call => call.uri),
    [generationRoot(generation)], 'only the new generation is removed');
  assert(![...file.binary.keys(), ...file.directories].some(uri => uri.startsWith(generationRoot(generation))));
  assert.deepEqual([...file.binary.get(`${generationRoot('old')}r0/retained.bin`)], [99]);
  assertNativeOnly(file);
}

async function testFreshSnapshots(api, activation) {
  const ids = ['top', 'base', 'below', 'unused'];
  const file = indexedFile(ids);
  const cached = await api.getResourceCatalog(file);
  const inventory = JSON.parse(file.text.get(inventoryUri));
  inventory.themes.top.push({ relativePath: 'assets/fresh.bin', sizeBytes: 1 });
  file.binary.set(`${themeRoot}top/assets/fresh.bin`, Uint8Array.of(42));
  file.text.set(inventoryUri, JSON.stringify(inventory));
  file.text.set(manifestUri('top'), manifest('top', undefined, 'Fresh top'));
  const before = file.calls.length;
  const snapshot = await api.loadResourcePackSnapshot(ids, file);
  assert.notStrictEqual(snapshot, cached, 'reload snapshot never uses the page catalog cache');
  assert(!cached.byPath.has('/resource/fresh.bin'));
  assert(snapshot.byPath.has('/resource/fresh.bin'));
  assert.equal(snapshot.themes.get('top').manifest.name, 'Fresh top');
  assert.deepEqual(snapshot.installedThemeIds, ids);
  assert.deepEqual(snapshot.paths[0].themes.map(theme => theme.themeId), ids);
  assert.strictEqual(snapshot.byPath.get(snapshot.paths[0].sourcePath), snapshot.paths[0]);
  const reloadReads = file.calls.slice(before).filter(call => call.method === 'read');
  assert.equal(reloadReads.length, ids.length * 2 + 1);
  for (const id of ids) assert.equal(reloadReads.filter(call => call.uri === manifestUri(id)).length, 1);
  assert.equal(reloadReads.filter(call => call.uri === inventoryUri).length, 1);

  // Disk edits after snapshot capture must not trigger activation's old second load.
  for (const id of ids) file.text.set(manifestUri(id), '{not to be read again');
  file.text.set(inventoryUri, '{not to be read again');
  const choices = { '/resource/a.bin': 'below', '/resource/b.bin': '@system' };
  const readsBefore = file.count('read');
  const plan = await activation.regenerateActiveMappings(['top', 'base', '@system', 'below', 'unused'],
    'fresh', file, choices, snapshot);
  assert.equal(file.count('read') - readsBefore, 2, 'only protection metadata and prior active TSV are read');
  assert.match(plan.mappings, /\/resource\/a\.bin\tthemes\/below\/assets\/a\.bin\n/,
    'explicit selection below the system boundary remains available');
  assert.match(plan.mappings, /\/resource\/b\.bin\t@system\n/);
  assert(!plan.copies.some(copy => copy.sourceUri.includes('/below/') || copy.sourceUri.includes('/unused/')),
    'below-system packs are not normal overlay candidates');
  assert.equal(file.text.get(mappingsUri), plan.mappings);
  assertNativeOnly(file);
}

async function testPriorityAndPrefixes(api, activation) {
  const file = indexedFile(['top', 'base', 'below']);
  file.text.set(manifestUri('top'), manifest('top', [
    { source: '/resource/', destination: 'assets/' },
    { source: '/resource/a.bin', destination: 'special.bin' },
    { source: '/resource/nested/', destination: 'specific/' }
  ]));
  file.binary.set(`${themeRoot}top/special.bin`, Uint8Array.of(7, 8));
  const inventory = JSON.parse(file.text.get(inventoryUri));
  inventory.themes.top.push({ relativePath: 'special.bin', sizeBytes: 2 },
    { relativePath: 'assets/nested/hole.bin', sizeBytes: 1 });
  file.binary.set(`${themeRoot}top/assets/nested/hole.bin`, Uint8Array.of(1));
  file.text.set(inventoryUri, JSON.stringify(inventory));
  const snapshot = await api.loadResourcePackSnapshot(['top', 'base', 'below'], file);
  assert.equal(snapshot.byPath.get('/resource/a.bin').themes[0].previewUri, `${themeRoot}top/special.bin`);
  assert.deepEqual(snapshot.byPath.get('/resource/nested/c.bin').themes.map(item => item.themeId), ['base', 'below'],
    'longest prefix masks the top broad mapping even if its destination lacks the file');
  const defaults = await activation.regenerateActiveMappings(['top', 'base', '@system', 'below'],
    'priority-default', file, {}, snapshot);
  assert.match(defaults.mappings, /\/resource\/a\.bin\tthemes\/top\/special\.bin\n/);
  assert.match(defaults.mappings, /\/resource\/nested\/c\.bin\tthemes\/base\/assets\/nested\/c\.bin\n/);
  assert(!defaults.mappings.includes('hole.bin'), 'a masked path with no lower candidate falls back to firmware');
  const plan = await activation.regenerateActiveMappings(['top', 'base', '@system', 'below'], 'priority', file,
    { '/resource/a.bin': 'base', '/resource/b.bin': '@system', '/resource/nested/c.bin': 'below' }, snapshot);
  assert.equal(plan.generation, null);
  assert.deepEqual(plan.copies, []);
  assert(!plan.mappings.includes('/resource/\t'), 'no broad mapping can bypass masked files');
  assert.match(plan.mappings, /\/resource\/a\.bin\tthemes\/base\/assets\/a\.bin\n/);
  assert.match(plan.mappings, /\/resource\/b\.bin\t@system\n/);
  assert.match(plan.mappings, /\/resource\/nested\/c\.bin\tthemes\/below\/assets\/nested\/c\.bin\n/);
  for (const copy of plan.copies) {
    assert.deepEqual(file.binary.get(copy.destinationUri), file.binary.get(copy.sourceUri));
  }
  assertNativeOnly(file);
}

async function testResolvedEnumeration(api, activation) {
  for (const mode of ['missing', 'stale', 'corrupt', 'unreadable', 'list-get']) {
    const ids = ['top', 'base'];
    const file = indexedFile(ids);
    const originalIndex = file.text.get(inventoryUri);
    if (mode === 'missing' || mode === 'list-get') file.text.delete(inventoryUri);
    if (mode === 'stale') file.text.set(inventoryUri, JSON.stringify({ version: 1, themes: {
      top: [{ relativePath: 'corona.json', sizeBytes: 1 }],
      base: [{ relativePath: 'corona.json', sizeBytes: 1 }]
    } }));
    if (mode === 'corrupt') file.text.set(inventoryUri, '{bad optional index');
    if (mode === 'unreadable') file.onRead = async (uri, value) => {
      if (uri === inventoryUri) throw new Error('Optional index unavailable');
      return value;
    };
    const inventoryBefore = file.text.get(inventoryUri);
    file.onInfo = async (uri, recursive) => {
      if (!recursive) return undefined;
      assert(ids.some(id => uri === `${themeRoot}${id}/`));
      if (mode === 'list-get') return { uri, length: 0, type: 'dir' };
      return { uri, length: 0, type: 'dir', subFiles: [
        { uri: `${uri}assets/`, length: 0, type: 'dir', subFiles: [
          { uri: `${uri}assets/a.bin`, length: 2, type: 'file' },
          { uri: `${uri}assets/b.bin`, length: 3, type: 'file' },
          { uri: `${uri}assets/nested/`, length: 0, type: 'dir', subFiles: [
            { uri: `${uri}assets/nested/c.bin`, length: 4, type: 'file' }
          ] }
        ] }
      ] };
    };
    file.onList = async uri => {
      if (ids.some(id => uri === `${themeRoot}${id}/`)) return [{ uri: 'assets/', length: 0 }];
      if (uri.endsWith('/assets/')) return [
        { uri: 'a.bin', length: 2 }, { uri: 'b.bin', length: 3 }, { uri: 'nested/', length: 0 }
      ];
      assert(uri.endsWith('/assets/nested/'));
      return [{ uri: 'c.bin', length: 4 }];
    };
    const snapshot = await api.loadResourcePackSnapshot(ids, file);
    assert.equal(snapshot.paths.length, 3, mode);
    for (const id of ids) assert.equal(file.count('info', `${themeRoot}${id}/`), 1, mode);
    const listsBefore = file.count('list');
    assert.equal(listsBefore, mode === 'list-get' ? 6 : 0);
    await activation.regenerateActiveMappings([...ids, '@system'], `scan-${mode}`, file, {}, snapshot);
    for (const id of ids) {
      assert.equal(file.count('info', `${themeRoot}${id}/`), 1, 'activation reuses the resolved enumeration');
      assert.equal(file.count('read', manifestUri(id)), 1);
    }
    assert.equal(file.count('list'), listsBefore);
    assert.equal(file.count('read', inventoryUri), 1);
    assert.equal(file.count('write', inventoryUri), 0, 'optional inventory is not rewritten');
    assert.equal(file.text.get(inventoryUri), inventoryBefore);
    if (mode === 'unreadable') assert.equal(inventoryBefore, originalIndex);
    assertNativeOnly(file);
  }
}

async function testIncompleteSnapshot(api, activation) {
  for (const cached of [false, true]) {
    for (const damage of ['missing-theme', 'missing-id', 'wrong-theme', 'wrong-manifest', 'missing-files',
      'below', 'unused-below']) {
      const file = indexedFile(['top', 'base', 'below', 'unused']);
      const snapshot = await api.loadResourcePackSnapshot(['top', 'base', 'below', 'unused'], file);
      const order = ['top', 'base', '@system', 'below', 'unused'];
      const choices = { '/resource/a.bin': 'below' };
      if (cached) await activation.regenerateActiveMappings(order, 'complete', file, choices, snapshot);
      const previousMappings = file.text.get(mappingsUri);
      if (damage === 'missing-theme') snapshot.themes.delete('base');
      if (damage === 'missing-id') snapshot.installedThemeIds = ['top', 'below', 'unused'];
      if (damage === 'wrong-theme') snapshot.themes.get('base').themeId = 'other';
      if (damage === 'wrong-manifest') snapshot.themes.get('base').manifest.themeId = 'other';
      if (damage === 'missing-files') snapshot.themes.get('base').files = null;
      if (damage === 'below') snapshot.themes.delete('below');
      if (damage === 'unused-below') snapshot.themes.delete('unused');
      const before = file.calls.length;
      await assert.rejects(activation.regenerateActiveMappings(order, 'incomplete', file, choices, snapshot),
        /资源快照缺少资源包/);
      assert.equal(file.calls.length, before, 'incomplete snapshot fails closed even before cache reuse');
      assert.equal(file.text.get(mappingsUri), previousMappings);
    }
  }
}

async function testCopyBarrierAndDirectories(api, activation) {
  const file = fallbackFile();
  const snapshot = await api.loadResourcePackSnapshot(['top', 'base'], file);
  const first = deferred(), last = deferred();
  file.onCopy = async src => {
    if (src.endsWith('/a.bin')) await first.promise;
    if (src.endsWith('/c.bin')) await last.promise;
  };
  let finished = false;
  const pending = activation.regenerateActiveMappings(['top', 'base', '@system'], 'barrier', file, {}, snapshot)
    .then(plan => { finished = true; return plan; });
  await file.when('copy', `${themeRoot}top/assets/a.bin`);
  await drainCallbacks();
  assert.equal(finished, false);
  assert.equal(file.count('write', mappingsUri), 0);
  assert.equal(file.text.get(mappingsUri), oldMappings);
  assert.equal(snapshot.activeMappings, undefined, 'in-flight materialization is not cached');
  assert.equal(file.count('info'), 0, 'copy preparation performs no asset stats');
  first.resolve();
  await file.when('copy', `${themeRoot}top/assets/nested/c.bin`);
  await drainCallbacks();
  assert.equal(file.count('copied'), 256);
  assert.equal(file.count('write', mappingsUri), 0, 'even the final outstanding copy blocks publication');
  last.resolve();
  const plan = await pending;
  assert.equal(file.count('copy'), 257);
  assert.equal(file.count('write', mappingsUri), 1);
  assert.equal(file.text.get(mappingsUri), plan.mappings);
  assert.deepEqual(file.calls.filter(call => call.method === 'mkdir').map(call => call.uri), [
    `${generationRoot('barrier')}r0/`, `${generationRoot('barrier')}r0/nested/`
  ], 'shared parent is prepared once, nested parent separately');
  for (const copy of plan.copies) {
    const native = file.calls.findIndex(call => call.method === 'copy' && call.uri === copy.sourceUri);
    const completion = file.calls.findIndex(call => call.method === 'copied' && call.uri === copy.destinationUri);
    const publication = file.calls.findIndex(call => call.method === 'write' && call.uri === mappingsUri);
    assert(native < completion && completion < publication);
  }
  assert.equal(file.count('info'), 0, 'source/destination size and type checks have been removed');
  assert.equal(plan.copies.length, 257, 'first result retains diagnostics');
  assert.deepEqual(snapshot.activeMappings.plan.copies, [], 'cache does not retain the large copy list');
  // Retry after an external signal/receipt failure reuses the published generation.
  const before = file.calls.length;
  const reused = await activation.regenerateActiveMappings(['top', 'base', '@system'], 'barrier2', file, {}, snapshot);
  assert.strictEqual(reused, snapshot.activeMappings.plan);
  assert.equal(reused.generation, 'barrier');
  assert.equal(reused.mappings, plan.mappings);
  assert.equal(file.calls.length, before, 'unchanged fallback reload does zero file operations');
  assertNativeOnly(file);
}

async function testInventoryValidation(api, activation) {
  for (const overlay of [false, true]) {
    const file = overlay ? fallbackFile() : indexedFile();
    const inventory = JSON.parse(file.text.get(inventoryUri));
    inventory.themes.top[0].sizeBytes = 0;
    file.text.set(inventoryUri, JSON.stringify(inventory));
    const snapshot = await api.loadResourcePackSnapshot(['top', 'base'], file);
    const before = file.calls.length;
    await assert.rejects(activation.regenerateActiveMappings(['top', 'base', '@system'], 'empty-asset',
      file, {}, snapshot), /空资源文件/);
    assert.equal(file.count('copy'), 0);
    assert.equal(file.count('info'), 0, 'zero-length rejection comes from inventory, not stats');
    assert.equal(snapshot.activeMappings, undefined);
    if (overlay) assertRolledBack(file, 'empty-asset');
    else assert.equal(file.calls.length, before, 'invalid direct plan is never published');
  }
  {
    const file = fallbackFile(['top', 'base', 'below']);
    file.text.set(manifestUri('top'), manifest('top', [
      { source: '/resource/', destination: 'assets/' },
      { source: '/resource/nested/', destination: 'missing-specific/' }
    ]));
    const inventory = JSON.parse(file.text.get(inventoryUri));
    inventory.themes.top.push({ relativePath: 'assets/nested/hole.bin', sizeBytes: 1 });
    file.text.set(inventoryUri, JSON.stringify(inventory));
    const snapshot = await api.loadResourcePackSnapshot(['top', 'base', 'below'], file);
    file.onInfo = async () => { throw new Error('Per-asset stat is forbidden'); };
    const plan = await activation.regenerateActiveMappings(['top', 'base', '@system', 'below'], 'no-stats',
      file, { '/resource/a.bin': '@system', '/resource/b.bin': 'below' }, snapshot);
    assert.equal(plan.generation, 'no-stats');
    assert.equal(plan.copies.length, 255);
    assert(plan.copies.some(copy => copy.sourceUri === `${themeRoot}base/assets/nested/c.bin`),
      'overlay fallback also preserves longest-prefix masking and pack priority');
    assert(!plan.copies.some(copy => copy.sourceUri.endsWith('/hole.bin')),
      'a fully masked firmware hole stays absent from the overlay');
    assert(!plan.copies.some(copy => copy.sourceUri.endsWith('/a.bin') || copy.sourceUri.endsWith('/b.bin')),
      'neither direct system nor explicit below-boundary choices are copied into overlays');
    assert.equal(file.count('info'), 0);
    assert.equal(file.text.get(mappingsUri), plan.mappings);
    assertNativeOnly(file);
  }
}

async function testNativeErrors(api, activation) {
  for (const code of [202, 300, 301]) {
    const file = fallbackFile();
    const snapshot = await api.loadResourcePackSnapshot(['top', 'base'], file);
    const failure = Object.assign(new Error(`Native failure ${code}`), { code });
    file.onCopy = async (_src, dst) => {
      file.binary.set(dst, Uint8Array.of(10)); // Native workers may leave a partial destination.
      throw failure;
    };
    await assert.rejects(activation.regenerateActiveMappings(['top', 'base', '@system'], `err-${code}`, file, {}, snapshot),
      error => error === failure, 'native failure is propagated, not treated as a compatibility signal');
    assert.equal(file.count('copy'), 1, 'no retry or manual fallback');
    assert.equal(file.count('copied'), 0);
    assertRolledBack(file, `err-${code}`);
    assert.equal(snapshot.activeMappings, undefined, 'native failures never fill the cache');
  }
  {
    const file = fallbackFile();
    const snapshot = await api.loadResourcePackSnapshot(['top', 'base'], file);
    const failure = Object.assign(new Error('Second native copy failed'), { code: 202 });
    file.onCopy = async (_src, dst) => {
      if (file.count('copy') === 2) {
        assert(file.binary.has(`${generationRoot('second')}r0/a.bin`), 'first new file exists before second failure');
        file.binary.set(dst, Uint8Array.of(11));
        throw failure;
      }
    };
    await assert.rejects(activation.regenerateActiveMappings(['top', 'base', '@system'], 'second', file, {}, snapshot),
      error => error === failure);
    assert.equal(file.count('copy'), 2);
    assert.equal(file.count('copied'), 1);
    assertRolledBack(file, 'second');
    assert.equal(snapshot.activeMappings, undefined);
  }
}

async function testReadBudget(api, activation) {
  const ids = ['top', 'base'];
  const assets = Array.from({ length: 100 }, (_, index) => ({
    relativePath: `assets/icon${index}.bin`, sizeBytes: index + 1
  }));
  const shared = indexedFile(ids, assets);
  const snapshot = await api.loadResourcePackSnapshot(ids, shared);
  const sharedPlan = await activation.regenerateActiveMappings([...ids, '@system'], 'sample', shared, {}, snapshot);
  assert.equal(snapshot.paths.length, 100);
  assert.equal(sharedPlan.copies.length, 0);
  assert.equal(sharedPlan.generation, null);
  for (const id of ids) assert.equal(shared.count('read', manifestUri(id)), 1);
  assert.equal(shared.count('read', inventoryUri), 1);
  assert.equal(shared.count('read'), 7, 'both names for two manifests, one inventory, protection registry and prior TSV');
  for (const method of ['info', 'copy', 'mkdir', 'list']) assert.equal(shared.count(method), 0, method);
  const before = shared.calls.length;
  assert.strictEqual(await activation.regenerateActiveMappings([...ids, '@system'], 'sample2', shared, {}, snapshot),
    sharedPlan, 'request revisions do not affect the cache key');
  assert.equal(shared.calls.length, before, 'cache hit skips registry and TSV reads/writes');

  const separate = indexedFile(ids, assets);
  await api.loadRegisteredResourcePaths(ids, separate);
  const separatePlan = await activation.regenerateActiveMappings([...ids, '@system'], 'sample', separate);
  assert.equal(separatePlan.mappings, sharedPlan.mappings);
  assert.deepEqual(separatePlan.copies, []);
  for (const id of ids) assert.equal(separate.count('read', manifestUri(id)), 2);
  assert.equal(separate.count('read', inventoryUri), 2);
  assert.equal(separate.count('read'), 12);

  const choices = { '/resource/icon0.bin': '@system', '/resource/icon1.bin': 'base' };
  const selected = await activation.regenerateActiveMappings([...ids, '@system'], 'choices', shared, choices, snapshot);
  const afterChoices = shared.calls.length;
  assert.strictEqual(await activation.regenerateActiveMappings([...ids, '@system'], 'choices-retry', shared,
    { '/resource/icon1.bin': 'base', '/resource/icon0.bin': '@system' }, snapshot), selected,
    'override key order is canonical');
  assert.equal(shared.calls.length, afterChoices);
  const reversed = await activation.regenerateActiveMappings(['base', 'top', '@system'], 'reversed',
    shared, choices, snapshot);
  assert.match(reversed.mappings, /\/resource\/icon2\.bin\tthemes\/base\/assets\/icon2\.bin\n/);
  assert.notStrictEqual(reversed, selected, 'pack priority is part of the key');
  const boundary = await activation.regenerateActiveMappings(['top', '@system', 'base'], 'boundary',
    shared, choices, snapshot);
  assert.notStrictEqual(boundary, reversed, 'system boundary is part of the key');
  const fresh = await activation.regenerateActiveMappings([...ids, '@system'], 'back-to-original', shared, {}, snapshot);
  assert.notStrictEqual(fresh, sharedPlan, 'only the single latest published input is cached');
  assert.equal(fresh.mappings, sharedPlan.mappings);
  assertNativeOnly(shared);
  assertNativeOnly(separate);
  console.log('2-pack/100-icons activation: 0 native copies, 0 stats; unchanged reload: 0 file operations.');
}

async function testPublishedCache(api, activation) {
  const file = fallbackFile();
  const snapshot = await api.loadResourcePackSnapshot(['top', 'base'], file);
  const order = ['top', 'base', '@system'];
  const first = await activation.regenerateActiveMappings(order, 'cache-a', file, {}, snapshot);
  const cacheA = snapshot.activeMappings;
  const choices = { '/resource/a.bin': '@system' };
  const copyError = new Error('changed input native failure');
  file.onCopy = async () => { throw copyError; };
  await assert.rejects(activation.regenerateActiveMappings(order, 'cache-failed', file, choices, snapshot),
    error => error === copyError);
  assert.strictEqual(snapshot.activeMappings, cacheA, 'failed preparation retains the last published cache');
  assert.equal(file.text.get(mappingsUri), first.mappings);
  file.onCopy = undefined;

  const publicationError = new Error('TSV publication failure');
  const originalWrite = file.writeText;
  file.writeText = async (uri, value) => {
    if (uri === mappingsUri) {
      file.text.set(uri, '# Partial native write\n');
      throw publicationError;
    }
    return originalWrite(uri, value);
  };
  await assert.rejects(activation.regenerateActiveMappings(order, 'cache-b', file, choices, snapshot),
    error => error === publicationError);
  assert.equal(snapshot.activeMappings, undefined, 'a possibly partial TSV publication invalidates the cache');
  assert.equal(file.text.get(mappingsUri), '# Partial native write\n');
  assert(![...file.binary.keys()].some(uri => uri.startsWith(generationRoot('cache-b'))));
  assert.deepEqual(JSON.parse(file.text.get(generationsUri)).generations, ['old', 'cache-a']);
  file.writeText = originalWrite;
  const restored = await activation.regenerateActiveMappings(order, 'cache-restore', file, {}, snapshot);
  assert.equal(restored.generation, 'cache-restore', 'retrying previous inputs cannot hit the old cache');
  assert.equal(restored.copies.length, 257);
  assert.equal(file.text.get(mappingsUri), restored.mappings, 'previous inputs restore the damaged TSV');
  const second = await activation.regenerateActiveMappings(order, 'cache-b', file, choices, snapshot);
  assert.equal(second.copies.length, 256, 'failed publication can be retried after rollback');
  assert.equal(snapshot.activeMappings.plan.generation, 'cache-b');
  await activation.cleanupInactiveGenerations(second.generation, file);
  assert(![...file.binary.keys()].some(uri => uri.startsWith(generationRoot('cache-a'))));
  assert.deepEqual(JSON.parse(file.text.get(generationsUri)).generations, ['cache-b']);

  const copiesBefore = file.count('copy');
  const third = await activation.regenerateActiveMappings(order, 'cache-c', file, {}, snapshot);
  assert.equal(third.generation, 'cache-c', 'switching back never reuses an already cleaned-up tree');
  assert.equal(file.count('copy') - copiesBefore, 257);
  await activation.cleanupInactiveGenerations(third.generation, file);
  const before = file.calls.length;
  const reused = await activation.regenerateActiveMappings(order, 'cache-new-request', file, {}, snapshot);
  assert.equal(reused.generation, 'cache-c');
  assert.deepEqual(reused.copies, []);
  assert.equal(file.calls.length, before, '>256-file repeated reload performs 0 additional copies or other I/O');
  assertNativeOnly(file);
}

async function testDirectDependencyReceipts(api, activation) {
  const file = indexedFile();
  const snapshot = await api.loadResourcePackSnapshot(['top', 'base'], file);
  const top = await activation.regenerateActiveMappings(['top', 'base', '@system'], 'direct-top', file, {}, snapshot);
  assert.deepEqual(await activation.readProtectedThemeIds(file), ['top']);
  const dependencyWrite = file.calls.findIndex(call => call.method === 'write' && call.uri === generationsUri);
  const tsvWrite = file.calls.findIndex(call => call.method === 'write' && call.uri === mappingsUri);
  assert(dependencyWrite < tsvWrite, 'direct package protection is durable before TSV publication');
  const base = await activation.regenerateActiveMappings(['base', 'top', '@system'], 'direct-base', file, {}, snapshot);
  assert.notEqual(base.mappings, top.mappings);
  assert.deepEqual(await activation.readProtectedThemeIds(file), ['base', 'top'],
    'a failed or missing switch receipt retains both potentially resident direct packs');
  const before = file.calls.length;
  await activation.regenerateActiveMappings(['base', 'top', '@system'], 'direct-base-retry', file, {}, snapshot);
  assert.equal(file.calls.length, before, 'unacknowledged direct plan remains reusable');
  assert.deepEqual(await activation.readProtectedThemeIds(file), ['base', 'top']);
  await activation.cleanupInactiveGenerations(base.generation, file, base.mappings);
  assert.deepEqual(await activation.readProtectedThemeIds(file), ['base'],
    'only acknowledgement releases dependencies from the old direct plan');
  const indexBefore = file.text.get(generationsUri);
  file.text.delete(mappingsUri);
  await assert.rejects(activation.cleanupInactiveGenerations(null, file), /缺少已确认/);
  assert.equal(file.text.get(generationsUri), indexBefore, 'missing config cannot discard package protection');
}

async function main() {
  const temporary = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'resource-hook-reload-native-')));
  const idle = deferred();
  const failOnIdle = () => idle.reject(new Error('Test stalled waiting for an expected native callback'));
  process.once('beforeExit', failOnIdle);
  try {
    execFileSync(path.join(root, 'manager/node_modules/.bin/tsc'), [
      path.join(root, 'manager/src/ts/resource-overrides.ts'),
      path.join(root, 'manager/src/ts/resource-activation.ts'),
      '--outDir', temporary, '--module', 'commonjs', '--target', 'es2018',
      '--lib', 'es2018,dom', '--skipLibCheck'
    ], { stdio: 'inherit' });
    const api = require(path.join(temporary, 'resource-overrides.js'));
    const activation = require(path.join(temporary, 'resource-activation.js'));
    await Promise.race([(async () => {
      await testFreshSnapshots(api, activation);
      await testPriorityAndPrefixes(api, activation);
      await testResolvedEnumeration(api, activation);
      await testIncompleteSnapshot(api, activation);
      await testCopyBarrierAndDirectories(api, activation);
      await testInventoryValidation(api, activation);
      await testNativeErrors(api, activation);
      await testReadBudget(api, activation);
      await testPublishedCache(api, activation);
      await testDirectDependencyReceipts(api, activation);
    })(), idle.promise]);
    console.log('Direct activation, snapshot reuse, native copy barriers, inventory validation and rollback tests passed.');
  } finally {
    process.removeListener('beforeExit', failOnIdle);
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
