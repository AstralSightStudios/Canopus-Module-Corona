/* Host regressions for fresh reload snapshots and mandatory native overlay copies. */
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
const manifestUri = id => `${themeRoot}${id}/canora.json`;
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
  assert.equal(reloadReads.length, ids.length + 1);
  for (const id of ids) assert.equal(reloadReads.filter(call => call.uri === manifestUri(id)).length, 1);
  assert.equal(reloadReads.filter(call => call.uri === inventoryUri).length, 1);

  // Disk edits after snapshot capture must not trigger activation's old second load.
  for (const id of ids) file.text.set(manifestUri(id), '{not to be read again');
  file.text.set(inventoryUri, '{not to be read again');
  const choices = { '/resource/a.bin': 'below', '/resource/b.bin': '@system' };
  const readsBefore = file.count('read');
  const plan = await activation.regenerateActiveMappings(['top', 'base', '@system', 'below', 'unused'],
    'fresh', file, choices, snapshot);
  assert.equal(file.count('read') - readsBefore, 1, 'only the generation registry is read during activation');
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
  inventory.themes.top.push({ relativePath: 'special.bin', sizeBytes: 2 });
  file.text.set(inventoryUri, JSON.stringify(inventory));
  const snapshot = await api.loadResourcePackSnapshot(['top', 'base', 'below'], file);
  assert.equal(snapshot.byPath.get('/resource/a.bin').themes[0].previewUri, `${themeRoot}top/special.bin`);
  assert.deepEqual(snapshot.byPath.get('/resource/nested/c.bin').themes.map(item => item.themeId), ['base', 'below'],
    'longest prefix masks the top broad mapping even if its destination lacks the file');
  const plan = await activation.regenerateActiveMappings(['top', 'base', '@system', 'below'], 'priority', file,
    { '/resource/a.bin': 'base', '/resource/b.bin': '@system', '/resource/nested/c.bin': 'below' }, snapshot);
  const byDestination = new Map(plan.copies.map(copy => [copy.destinationUri, copy.sourceUri]));
  assert.equal(byDestination.get(`${generationRoot('priority')}r0/a.bin`), `${themeRoot}top/special.bin`);
  assert.equal(byDestination.get(`${generationRoot('priority')}r0/b.bin`), `${themeRoot}top/assets/b.bin`);
  assert.equal(byDestination.get(`${generationRoot('priority')}r0/nested/c.bin`), `${themeRoot}base/assets/nested/c.bin`);
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
      top: [{ relativePath: 'canora.json', sizeBytes: 1 }],
      base: [{ relativePath: 'canora.json', sizeBytes: 1 }]
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
  for (const damage of ['missing-theme', 'missing-id', 'wrong-theme', 'wrong-manifest', 'missing-files', 'below']) {
    const file = indexedFile(['top', 'base', 'below']);
    const snapshot = await api.loadResourcePackSnapshot(['top', 'base', 'below'], file);
    if (damage === 'missing-theme') snapshot.themes.delete('base');
    if (damage === 'missing-id') snapshot.installedThemeIds = ['top', 'below'];
    if (damage === 'wrong-theme') snapshot.themes.get('base').themeId = 'other';
    if (damage === 'wrong-manifest') snapshot.themes.get('base').manifest.themeId = 'other';
    if (damage === 'missing-files') snapshot.themes.get('base').files = null;
    if (damage === 'below') snapshot.themes.delete('below');
    const before = file.calls.length;
    await assert.rejects(activation.regenerateActiveMappings(['top', 'base', '@system', 'below'], 'incomplete',
      file, { '/resource/a.bin': 'below' }, snapshot), /资源快照缺少资源包/);
    assert.equal(file.calls.length, before, 'incomplete snapshot fails closed, without a disk reread or any mutation');
    assert.equal(file.text.get(mappingsUri), oldMappings);
  }
}

async function testCopyBarrierAndDirectories(api, activation) {
  const file = indexedFile();
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
  assert.equal(file.count('info', `${generationRoot('barrier')}r0/a.bin`), 0,
    'destination validation also waits for native completion');
  first.resolve();
  await file.when('copy', `${themeRoot}top/assets/nested/c.bin`);
  await drainCallbacks();
  assert.equal(file.count('copied'), 2);
  assert.equal(file.count('write', mappingsUri), 0, 'even the final outstanding copy blocks publication');
  last.resolve();
  const plan = await pending;
  assert.equal(file.count('copy'), 3);
  assert.equal(file.count('write', mappingsUri), 1);
  assert.equal(file.text.get(mappingsUri), plan.mappings);
  assert.deepEqual(file.calls.filter(call => call.method === 'mkdir').map(call => call.uri), [
    `${generationRoot('barrier')}r0/`, `${generationRoot('barrier')}r0/nested/`
  ], 'shared parent is prepared once, nested parent separately');
  for (const copy of plan.copies) {
    const sourceCheck = file.calls.findIndex(call => call.method === 'info' && call.uri === copy.sourceUri);
    const native = file.calls.findIndex(call => call.method === 'copy' && call.uri === copy.sourceUri);
    const completion = file.calls.findIndex(call => call.method === 'copied' && call.uri === copy.destinationUri);
    const destinationCheck = file.calls.findIndex(call => call.method === 'info' && call.uri === copy.destinationUri);
    const publication = file.calls.findIndex(call => call.method === 'write' && call.uri === mappingsUri);
    assert(sourceCheck < native && native < completion && completion < destinationCheck && destinationCheck < publication);
  }
  // A second reload gets its own prepared-directory set.
  await activation.regenerateActiveMappings(['top', 'base', '@system'], 'barrier2', file, {}, snapshot);
  assert.equal(file.count('mkdir'), 4);
  assertNativeOnly(file);
}

async function testValidationAndRollback(api, activation) {
  for (const mode of ['source-size', 'destination-size', 'source-dir', 'destination-dir',
    'source-empty-type', 'destination-empty-type', 'empty-asset']) {
    const assets = [{ relativePath: 'assets/a.bin', sizeBytes: mode === 'empty-asset' ? 0 : 2 }];
    const file = indexedFile(['top', 'base'], assets);
    const snapshot = await api.loadResourcePackSnapshot(['top', 'base'], file);
    if (mode === 'source-size') file.binary.set(`${themeRoot}top/assets/a.bin`, Uint8Array.of(1));
    if (mode === 'destination-size') file.afterCopy = async (_src, dst) => file.binary.set(dst, Uint8Array.of(1));
    if (mode.includes('type') || mode.endsWith('-dir')) file.onInfo = async uri => {
      const source = uri === `${themeRoot}top/assets/a.bin`;
      const destination = uri.startsWith(generationRoot(mode));
      if ((mode.startsWith('source-') && source) || (mode.startsWith('destination-') && destination)) {
        return { length: 2, type: mode.endsWith('-dir') ? 'dir' : '' };
      }
      return undefined;
    };
    await assert.rejects(activation.regenerateActiveMappings(['top', 'base', '@system'], mode, file, {}, snapshot),
      mode === 'empty-asset' ? /空资源文件/ : mode.startsWith('source-') ? /大小与快照不一致/ : /复制大小不一致/);
    assert.equal(file.count('copy'), mode.startsWith('destination-') ? 1 : 0, mode);
    assertRolledBack(file, mode);
    assert(file.binary.has(`${themeRoot}top/assets/a.bin`), 'source is never deleted');
  }
  {
    const file = indexedFile();
    const snapshot = await api.loadResourcePackSnapshot(['top', 'base'], file);
    file.onInfo = async uri => file.binary.has(uri) ? { length: file.binary.get(uri).length } : undefined;
    const plan = await activation.regenerateActiveMappings(['top', 'base', '@system'], 'no-type', file, {}, snapshot);
    assert.equal(file.count('copy'), 3, 'omitted metadata type is accepted for source and destination');
    assert.equal(file.text.get(mappingsUri), plan.mappings);
    assertNativeOnly(file);
  }
}

async function testNativeErrors(api, activation) {
  for (const code of [202, 300, 301]) {
    const file = indexedFile();
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
  }
  {
    const file = indexedFile();
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
  }
}

async function testReadBudget(api, activation) {
  const ids = Array.from({ length: 10 }, (_, index) => `pack${index}`);
  const assets = Array.from({ length: 100 }, (_, index) => ({
    relativePath: `assets/icon${index}.bin`, sizeBytes: index + 1
  }));
  const shared = indexedFile(ids, assets);
  const snapshot = await api.loadResourcePackSnapshot(ids, shared);
  const sharedPlan = await activation.regenerateActiveMappings([...ids, '@system'], 'sample', shared, {}, snapshot);
  assert.equal(snapshot.paths.length, 100);
  assert.equal(sharedPlan.copies.length, 100);
  for (const id of ids) assert.equal(shared.count('read', manifestUri(id)), 1);
  assert.equal(shared.count('read', inventoryUri), 1);
  assert.equal(shared.count('read'), 12, 'ten manifests, one index, one generation registry');
  assert.equal(shared.count('info'), 200, 'one source and one destination check per native copy');
  assert.equal(shared.count('copy'), 100);
  assert.equal(shared.count('mkdir'), 1);
  assert.equal(shared.count('list'), 0);

  // Measure the separate-operation reference using the same native API, not a legacy copy loop.
  const separate = indexedFile(ids, assets);
  await api.loadRegisteredResourcePaths(ids, separate);
  const separatePlan = await activation.regenerateActiveMappings([...ids, '@system'], 'sample', separate);
  assert.equal(separatePlan.mappings, sharedPlan.mappings);
  assert.deepEqual(separatePlan.copies, sharedPlan.copies);
  for (const id of ids) assert.equal(separate.count('read', manifestUri(id)), 2);
  assert.equal(separate.count('read', inventoryUri), 2);
  assert.equal(separate.count('read'), 23);
  assertNativeOnly(shared);
  assertNativeOnly(separate);
  console.log(`10-pack/100-files-per-pack reload: shared snapshot ${shared.count('read')} text reads / ` +
    `${shared.count('read', inventoryUri)} inventory; separate loads ${separate.count('read')} / ` +
    `${separate.count('read', inventoryUri)}. Both use 100 native copies; no binary read/write/delete.`);
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
      await testValidationAndRollback(api, activation);
      await testNativeErrors(api, activation);
      await testReadBudget(api, activation);
    })(), idle.promise]);
    console.log('Fresh reload snapshots, native copy barriers, validation and generation rollback tests passed.');
  } finally {
    process.removeListener('beforeExit', failOnIdle);
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
