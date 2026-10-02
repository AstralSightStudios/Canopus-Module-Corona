/* Deterministic host regressions for shared catalogs and serialized override mutations. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const installedUri = 'internal://files/interconnect-themes.json';
const inventoryUri = 'internal://files/resource-files.json';
const overridesUri = 'internal://files/resource-overrides.json';
const orderUri = 'internal://files/resource-order.json';
const themeRoot = 'internal://files/themes/';
const manifestUri = id => `${themeRoot}${id}/corona.json`;

function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}

// A turn barrier drains runnable callbacks, not a timing or performance threshold.
const drainCallbacks = () => new Promise(resolve => setImmediate(resolve));

function makeFile() {
  const text = new Map();
  const calls = [];
  const listeners = [];
  const file = {
    text, calls,
    record(method, uri) {
      calls.push({ method, uri });
      for (const listener of [...listeners]) listener();
    },
    count(method, uri) {
      return calls.filter(call => call.method === method && (uri === undefined || call.uri === uri)).length;
    },
    when(method, uri, count = 1) {
      if (file.count(method, uri) >= count) return Promise.resolve();
      const ready = deferred();
      const listener = () => {
        if (file.count(method, uri) < count) return;
        listeners.splice(listeners.indexOf(listener), 1);
        ready.resolve();
      };
      listeners.push(listener);
      return ready.promise;
    },
    async readOptionalText(uri) {
      file.record('read', uri);
      const value = text.has(uri) ? text.get(uri) : null;
      return file.onRead ? file.onRead(uri, value) : value;
    },
    async writeText(uri, value) {
      file.record('write', uri);
      if (file.onWrite) await file.onWrite(uri, value);
      text.set(uri, value);
    },
    async readFileInfo(uri, recursive) {
      file.record('info', uri);
      if (file.onInfo) return file.onInfo(uri, recursive);
      throw new Error(`Unexpected metadata query: ${uri}`);
    },
    async listDirectory(uri) {
      file.record('list', uri);
      if (file.onList) return file.onList(uri);
      throw new Error(`Unexpected directory query: ${uri}`);
    },
    async readArrayBuffer() { throw new Error('Unexpected binary read'); },
    async writeArrayBuffer() { throw new Error('Unexpected binary write'); },
    async makeDirectory() { throw new Error('Unexpected directory creation'); },
    async deleteFile() { throw new Error('Unexpected deletion'); },
    async removeDirectory() { throw new Error('Unexpected directory removal'); },
    isFileNotFound() { return false; }
  };
  return file;
}

function manifest(id, name = id, destination = 'assets/') {
  return JSON.stringify({
    format: 'canopus-resource-pack', formatVersion: 1, themeId: id, name,
    mappings: [{ source: '/resource/', destination }]
  });
}

function indexedFile(ids) {
  const file = makeFile();
  file.text.set(installedUri, JSON.stringify(ids));
  const themes = {};
  for (const id of ids) {
    file.text.set(manifestUri(id), manifest(id));
    themes[id] = [{ relativePath: 'assets/shared.bin', sizeBytes: 1 }];
  }
  file.text.set(inventoryUri, JSON.stringify({ version: 1, themes }));
  return file;
}

function setOverrides(file, overrides) {
  file.text.set(overridesUri, JSON.stringify({ version: 1, overrides }));
}

function savedOverrides(file) {
  return JSON.parse(file.text.get(overridesUri)).overrides;
}

function registration(sourcePath, ids) {
  return { sourcePath, themes: ids.map(themeId => ({ themeId, name: themeId, previewUri: '' })) };
}

async function testSharedCatalog(api) {
  const ids = ['zeta', 'alpha', 'middle', 'last'];
  const file = indexedFile(ids);
  const indexGate = deferred();
  const gates = new Map(ids.map(id => [manifestUri(id), deferred()]));
  let active = 0, peak = 0;
  file.onRead = async (uri, value) => {
    if (uri === inventoryUri) { await indexGate.promise; return value; }
    if (!gates.has(uri)) return value;
    active++;
    peak = Math.max(peak, active);
    try { await gates.get(uri).promise; return value; }
    finally { active--; }
  };

  const first = api.getResourceCatalog(file);
  const second = api.getResourceCatalog(file);
  assert.strictEqual(second, first, 'simultaneous callers share the exact in-flight promise');
  await file.when('read', manifestUri('alpha'));
  await drainCallbacks();
  assert.equal(active, 2);
  assert.equal(file.count('read', manifestUri('middle')), 0, 'a third manifest waits for a reader slot');
  assert.equal(file.count('read', inventoryUri), 1, 'the global inventory is read once');
  gates.get(manifestUri('alpha')).resolve();
  await file.when('read', manifestUri('middle'));
  assert.equal(active, 2);
  gates.get(manifestUri('middle')).resolve();
  await file.when('read', manifestUri('last'));
  gates.get(manifestUri('last')).resolve();
  gates.get(manifestUri('zeta')).resolve();
  indexGate.resolve();
  const snapshot = await first;
  assert.strictEqual(await second, snapshot);
  assert.equal(peak, 2, 'manifest concurrency is bounded by two');
  assert.deepEqual(snapshot.installedThemeIds, ids);
  assert.deepEqual(snapshot.paths[0].themes.map(theme => theme.themeId), ids,
    'completion order must not change pack priority');
  assert.strictEqual(snapshot.byPath.get('/resource/shared.bin'), snapshot.paths[0]);
  assert.equal(file.count('read'), ids.length * 2 + 2, 'both manifest names are checked for ambiguity');
  assert.equal(file.count('info') + file.count('list') + file.count('write'), 0);
  const before = file.calls.length;
  const warm = api.getResourceCatalog(file);
  assert.strictEqual(warm, first, 'warm callers reuse the completed promise');
  assert.strictEqual(await warm, snapshot);
  assert.equal(file.calls.length, before, 'a warm catalog performs no I/O');

  const otherFile = indexedFile(['other']);
  assert.notStrictEqual(await api.getResourceCatalog(otherFile), snapshot,
    'the snapshot is scoped to the file API instance');

  // Replacing a pack retains its ID, so an ID-list-only cache would be stale.
  file.onRead = undefined;
  file.text.set(manifestUri('zeta'), manifest('zeta', 'Updated', 'new/'));
  const inventory = JSON.parse(file.text.get(inventoryUri));
  inventory.themes.zeta = [{ relativePath: 'new/updated.bin', sizeBytes: 2 }];
  file.text.set(inventoryUri, JSON.stringify(inventory));
  api.invalidateResourceCatalog();
  assert.equal(api.isResourceCatalogCurrent(snapshot), false);
  const updatedPromise = api.getResourceCatalog(file);
  assert.notStrictEqual(updatedPromise, first);
  const updated = await updatedPromise;
  assert.equal(api.isResourceCatalogCurrent(updated), true);
  assert.deepEqual(updated.installedThemeIds, ids);
  assert.equal(updated.byPath.get('/resource/updated.bin').themes[0].name, 'Updated');
  assert.deepEqual(updated.byPath.get('/resource/shared.bin').themes.map(theme => theme.themeId), ids.slice(1));
  assert.equal(file.count('read', inventoryUri), 2);

  file.text.set(installedUri, JSON.stringify(ids.slice(1)));
  file.text.delete(manifestUri('zeta'));
  api.invalidateResourceCatalog();
  assert.equal(api.isResourceCatalogCurrent(updated), false);
  const removed = await api.getResourceCatalog(file);
  assert.deepEqual(removed.installedThemeIds, ids.slice(1));
  assert.equal(removed.byPath.has('/resource/updated.bin'), false, 'deleted packs disappear after invalidation');
  assert.equal(file.count('read', manifestUri('zeta')), 2, 'a deleted manifest is not queried again');
}

async function testInvalidationAndRetry(api) {
  {
    const file = indexedFile(['base']);
    const gate = deferred();
    file.onRead = async (uri, value) => {
      if (uri === manifestUri('base') && file.count('read', uri) === 1) await gate.promise;
      return value;
    };
    const pending = api.getResourceCatalog(file);
    await file.when('read', manifestUri('base'));
    file.text.set(manifestUri('base'), manifest('base', 'Fresh'));
    api.invalidateResourceCatalog();
    gate.resolve();
    const snapshot = await pending;
    assert.equal(snapshot.paths[0].themes[0].name, 'Fresh', 'an invalidated in-flight load retries automatically');
    assert.equal(api.isResourceCatalogCurrent(snapshot), true, 'no stale snapshot is published');
    assert.equal(file.count('read', installedUri), 2);
    assert.equal(file.count('read', inventoryUri), 2);
    assert.strictEqual(await api.getResourceCatalog(file), snapshot);
  }
  {
    const file = indexedFile(['base']);
    const gate = deferred();
    file.onRead = async (uri, value) => {
      if (uri === manifestUri('base') && file.count('read', uri) === 1) {
        await gate.promise;
        throw new Error('obsolete load failed');
      }
      return value;
    };
    const oldPromise = api.getResourceCatalog(file);
    await file.when('read', manifestUri('base'));
    api.invalidateResourceCatalog();
    const currentPromise = api.getResourceCatalog(file);
    const current = await currentPromise;
    gate.resolve();
    assert.strictEqual(await oldPromise, current, 'a stale failure joins the newer in-flight or completed load');
    assert.strictEqual(api.getResourceCatalog(file), currentPromise, 'a stale failure must not evict the newer cache');
    assert.equal(file.count('read', installedUri), 2);
  }
  for (const failure of ['installed', 'manifest']) {
    const file = indexedFile(['base']);
    if (failure === 'installed') file.text.set(installedUri, '{bad index');
    else file.text.set(manifestUri('base'), '{bad manifest');
    const failed = api.getResourceCatalog(file);
    assert.strictEqual(api.getResourceCatalog(file), failed);
    await assert.rejects(failed, failure === 'installed' ? /索引损坏/ : /manifest/);
    file.text.set(installedUri, JSON.stringify(['base']));
    file.text.set(manifestUri('base'), manifest('base'));
    const retry = api.getResourceCatalog(file);
    assert.notStrictEqual(retry, failed, 'a rejected catalog promise is not permanently cached');
    assert.equal((await retry).paths.length, 1);
    assert.equal(file.count('read', installedUri), 2);
    assert.strictEqual(api.getResourceCatalog(file), retry);
  }
}

async function testFallbackAndCancellation(api) {
  const file = indexedFile(['first', 'second']);
  file.text.delete(inventoryUri);
  const firstGate = deferred();
  file.onInfo = async (uri, recursive) => {
    assert.equal(recursive, true);
    if (uri === `${themeRoot}first/`) await firstGate.promise;
    return { uri, type: 'dir', length: 0, subFiles: [
      { uri: `${uri}assets/shared.bin`, type: 'file', length: 1 }
    ] };
  };
  const pending = api.getResourceCatalog(file);
  await file.when('info', `${themeRoot}first/`);
  await drainCallbacks();
  assert.equal(file.count('info', `${themeRoot}second/`), 0, 'fallback enumeration is sequential across packs');
  firstGate.resolve();
  const snapshot = await pending;
  assert.deepEqual(snapshot.paths[0].themes.map(theme => theme.themeId), ['first', 'second']);
  assert.equal(file.count('info'), 2);
  assert.equal(file.count('write'), 0, 'legacy discovery never rewrites the optional inventory');
  const before = file.calls.length;
  assert.strictEqual(await api.getResourceCatalog(file), snapshot);
  assert.equal(file.calls.length, before, 'legacy fallback is also cached for the session');

  // Cancel a list/get fallback while the first leaf metadata callback is outstanding.
  const cancelled = indexedFile(['first', 'second']);
  cancelled.text.set(inventoryUri, '{corrupt optional inventory');
  const leafGate = deferred();
  const firstRoot = `${themeRoot}first/`;
  let current = true;
  cancelled.onInfo = async (uri, recursive) => {
    if (recursive) return { uri, type: 'dir', length: 0 };
    assert.equal(uri, `${firstRoot}assets/a.bin`);
    await leafGate.promise;
    return { uri, type: 'file', length: 1 };
  };
  cancelled.onList = async uri => {
    assert.equal(uri, firstRoot);
    return ['a.bin', 'b.bin'].map(name => ({ uri: `${firstRoot}assets/${name}`, length: 1 }));
  };
  const loading = api.loadRegisteredResourcePaths(['first', 'second'], cancelled, () => current);
  const rejection = assert.rejects(loading, /资源目录已更新/);
  await cancelled.when('info', `${firstRoot}assets/a.bin`);
  current = false;
  leafGate.resolve();
  await rejection;
  assert.deepEqual(cancelled.calls.filter(call => call.method === 'info').map(call => call.uri),
    [firstRoot, `${firstRoot}assets/a.bin`], 'cancellation prevents later leaf and later pack metadata queries');
  assert.equal(cancelled.count('list'), 1);
  assert.equal(cancelled.count('write'), 0);

  // Recursive get errors are caught for fallback, but cancellation must still prevent list/get.
  const recursive = indexedFile(['first']);
  recursive.text.delete(inventoryUri);
  const rootGate = deferred();
  current = true;
  recursive.onInfo = async () => { await rootGate.promise; return { type: 'dir', length: 0 }; };
  const rootLoading = api.loadRegisteredResourcePaths(['first'], recursive, () => current);
  const rootRejection = assert.rejects(rootLoading, /资源目录已更新/);
  await recursive.when('info', firstRoot);
  current = false;
  rootGate.resolve();
  await rootRejection;
  assert.equal(recursive.count('list'), 0, 'cancelled recursive get does not begin directory fallback');
}

async function testOverrideTransactions(api) {
  {
    const file = makeFile();
    const readGate = deferred(), writeGate = deferred();
    file.onRead = async (uri, value) => { if (file.count('read', uri) === 1) await readGate.promise; return value; };
    file.onWrite = async () => { if (file.count('write') === 1) await writeGate.promise; };
    const first = api.saveResourceOverride('/resource/a.bin', 'base', file);
    await file.when('read', overridesUri);
    const second = api.saveResourceOverride('/resource/b.bin', '@system', file);
    const readAfterSaves = api.loadResourceOverrides(file);
    await drainCallbacks();
    assert.equal(file.count('read'), 1, 'the whole save read-modify-write is queued, not just its write');
    const unrelated = makeFile();
    await api.saveResourceOverride('/resource/independent.bin', 'base', unrelated);
    assert.deepEqual(savedOverrides(unrelated), { '/resource/independent.bin': 'base' },
      'a blocked transaction does not stall a different file API');
    readGate.resolve();
    await file.when('write', overridesUri);
    await drainCallbacks();
    assert.equal(file.count('read'), 1, 'queued saves and subsequent readers wait for commit');
    assert.equal(file.text.has(overridesUri), false);
    writeGate.resolve();
    await Promise.all([first, second]);
    const committed = { '/resource/a.bin': 'base', '/resource/b.bin': '@system' };
    assert.deepEqual(savedOverrides(file), committed, 'overlapping saves preserve both choices');
    assert.deepEqual({ ...await readAfterSaves }, committed, 'a read after queued saves observes both commits');
  }
  const catalog = [registration('/resource/a.bin', ['base']), registration('/resource/b.bin', ['base'])];
  for (const firstOperation of ['reconcile', 'save', 'remove']) {
    const file = makeFile();
    setOverrides(file, { '/resource/a.bin': 'removed', '/resource/orphan.bin': 'removed' });
    const gate = deferred();
    file.onWrite = async () => { if (file.count('write') === 1) await gate.promise; };
    let first, second;
    if (firstOperation === 'save') {
      first = api.saveResourceOverride('/resource/b.bin', 'base', file);
      await file.when('write', overridesUri);
      second = api.reconcileResourceOverrides(catalog, file);
    } else {
      first = firstOperation === 'remove'
        ? api.removeThemeFromResourceOverrides('removed', file)
        : api.reconcileResourceOverrides(catalog, file);
      await file.when('write', overridesUri);
      second = api.saveResourceOverride('/resource/b.bin', 'base', file);
    }
    await drainCallbacks();
    assert.equal(file.count('read', overridesUri), 1, `${firstOperation}/save overlap waits before reading`);
    gate.resolve();
    await Promise.all([first, second]);
    const expected = { '/resource/a.bin': '@default', '/resource/b.bin': 'base' };
    if (firstOperation === 'remove') expected['/resource/orphan.bin'] = '@default';
    assert.deepEqual(savedOverrides(file), expected, `${firstOperation}/save overlap must not lose updates`);
  }
  {
    const file = makeFile();
    const gate = deferred();
    file.onWrite = async () => { if (file.count('write') === 1) await gate.promise; };
    const failed = api.saveResourceOverride('/resource/a.bin', 'base', file);
    const rejection = assert.rejects(failed, /write failed/);
    await file.when('write', overridesUri);
    const recovery = api.saveResourceOverride('/resource/b.bin', 'base', file);
    const readAfterSave = api.loadResourceOverrides(file);
    gate.reject(new Error('write failed'));
    await rejection;
    await recovery;
    assert.deepEqual({ ...await readAfterSave }, { '/resource/b.bin': 'base' }, 'a failed write does not poison the queue');
    assert.equal(file.count('write'), 2);
  }
}

async function testReadOnlyDisplay(api, order) {
  const file = indexedFile(['base', 'newpack']);
  setOverrides(file, { '/resource/shared.bin': 'removed', '/resource/orphan.bin': 'removed' });
  file.text.set(orderUri, JSON.stringify({ version: 1, order: ['removed', '@system', 'base'] }));
  const diskBefore = [...file.text.entries()];
  file.onWrite = async () => { throw new Error('Display must not write to disk'); };
  const snapshot = await api.getResourceCatalog(file);
  const choices = await api.loadResourceOverrides(file);
  const item = snapshot.byPath.get('/resource/shared.bin');
  Object.freeze(choices);
  Object.freeze(item.themes);
  Object.freeze(item);
  assert.equal(api.resolveResourceChoice(item, choices), '@default', 'stale selections display Default without reconciliation');
  assert.equal(api.resolveResourceChoice(item, Object.freeze({})), '@default');
  for (const choice of ['@default', '@system', 'base', 'newpack']) {
    assert.equal(api.resolveResourceChoice(item, Object.freeze({ [item.sourcePath]: choice })), choice);
  }
  const ids = Object.freeze([...snapshot.installedThemeIds]);
  assert.deepEqual(order.resolveResourceOrder(ids, await file.readOptionalText(orderUri)), ['newpack', '@system', 'base']);
  assert.deepEqual(order.resolveResourceOrder(ids, null), ['base', 'newpack', '@system']);
  assert.throws(() => order.resolveResourceOrder(ids, '{broken'), /排序记录损坏/);
  assert.equal(choices['/resource/shared.bin'], 'removed');
  assert.equal(choices['/resource/orphan.bin'], 'removed');
  assert.deepEqual([...file.text.entries()], diskBefore, 'page display helpers leave stale persisted choices and order untouched');
  assert.equal(file.count('write'), 0);
}

async function testPageReadBudget(api, order) {
  const ids = Array.from({ length: 10 }, (_, index) => `pack${index}`);
  const file = indexedFile(ids);
  // Match the audited sample: ten packs, each providing the same hundred source paths.
  const themes = {};
  for (const id of ids) {
    themes[id] = Array.from({ length: 100 }, (_, index) => ({
      relativePath: `assets/icon${index}.bin`, sizeBytes: index + 1
    }));
  }
  file.text.set(inventoryUri, JSON.stringify({ version: 1, themes }));
  setOverrides(file, { '/resource/icon0.bin': 'pack0' });
  file.text.set(orderUri, JSON.stringify({ version: 1, order: [...ids, '@system'] }));

  const [coldCatalog, coldOverrides] = await Promise.all([
    api.getResourceCatalog(file), api.loadResourceOverrides(file)
  ]);
  assert.equal(coldCatalog.paths.length, 100);
  assert.equal(api.resolveResourceChoice(coldCatalog.paths[0], coldOverrides), 'pack0');
  const coldReads = file.count('read');
  const coldIndexes = file.count('read', inventoryUri);
  assert.equal(coldReads, 23, 'cold list checks both names for ten manifests, installed IDs, one inventory and overrides');
  assert.equal(coldIndexes, 1);

  const [selectionCatalog, selectionOverrides, orderText] = await Promise.all([
    api.getResourceCatalog(file), api.loadResourceOverrides(file), file.readOptionalText(orderUri)
  ]);
  assert.strictEqual(selectionCatalog, coldCatalog);
  assert.equal(api.resolveResourceChoice(selectionCatalog.paths[0], selectionOverrides), 'pack0');
  assert.deepEqual(order.resolveResourceOrder(selectionCatalog.installedThemeIds, orderText), [...ids, '@system']);
  const [returnedCatalog, returnedOverrides] = await Promise.all([
    api.getResourceCatalog(file), api.loadResourceOverrides(file)
  ]);
  assert.strictEqual(returnedCatalog, coldCatalog);
  assert.equal(api.resolveResourceChoice(returnedCatalog.paths[0], returnedOverrides), 'pack0');
  const roundtripReads = file.count('read') - coldReads;
  const roundtripIndexes = file.count('read', inventoryUri) - coldIndexes;
  assert.equal(roundtripReads, 3, 'warm selection reads overrides/order and returning list reads overrides only');
  assert.equal(roundtripIndexes, 0);
  assert.equal(file.count('info') + file.count('list') + file.count('write'), 0);
  console.log(`10-pack/100-files-per-pack sample: cold list ${coldReads} text reads / ${coldIndexes} global index; ` +
    `warm selection + return ${roundtripReads} text reads / ${roundtripIndexes} global indexes ` +
    '(prior audited baseline: cold 22/10, roundtrip 45/20).');
}

async function testApplicationSession(api, entry, storageEntry) {
  // Requiring fresh copies models separate webpack tables in Vela page/app bundles.
  const bundle = () => {
    delete require.cache[require.resolve(entry)];
    return require(entry);
  };
  const listBundle = bundle();
  const selectBundle = bundle();
  const receiverBundle = bundle();
  const file = indexedFile(['base']);
  const listFile = makeFile();
  const selectFile = makeFile();
  api.initializeResourceCatalogSession(file);
  try {
    const listLoad = listBundle.getResourceCatalog(listFile);
    const selectLoad = selectBundle.getResourceCatalog(selectFile);
    assert.strictEqual(listLoad, selectLoad, 'separate bundles use the canonical app adapter and promise');
    const snapshot = await listLoad;
    assert.equal(file.count('read', inventoryUri), 1);
    assert.equal(listFile.calls.length + selectFile.calls.length, 0, 'page namespace identities do not fork the coordinator');
    assert.equal(receiverBundle.isResourceCatalogCurrent(snapshot), true);
    const prepared = { key: 'unchanged-inputs', plan: { mappings: '# Cached\n', generation: null, copies: [] } };
    snapshot.activeMappings = prepared;
    assert.strictEqual((await selectBundle.getResourceCatalog(selectFile)).activeMappings, prepared,
      'activation metadata shares the app-owned snapshot across page bundles');

    const gate = deferred();
    file.onWrite = async () => { if (file.count('write') === 1) await gate.promise; };
    const first = selectBundle.saveResourceOverride('/resource/shared.bin', 'base', selectFile);
    await file.when('write', overridesUri);
    const second = listBundle.saveResourceOverride('/resource/other.bin', '@system', listFile);
    const reading = receiverBundle.loadResourceOverrides(makeFile());
    await drainCallbacks();
    assert.equal(file.count('read', overridesUri), 1, 'transactions in different bundles share one queue');
    gate.resolve();
    await Promise.all([first, second]);
    assert.deepEqual({ ...await reading }, {
      '/resource/shared.bin': 'base', '/resource/other.bin': '@system'
    });
    file.onWrite = undefined;

    file.text.set(manifestUri('base'), manifest('base', 'Updated across bundles'));
    receiverBundle.invalidateResourceCatalog();
    assert.equal(listBundle.isResourceCatalogCurrent(snapshot), false, 'receiver invalidation reaches a retained list page');
    const updated = await listBundle.getResourceCatalog(listFile);
    assert.equal(updated.paths[0].themes[0].name, 'Updated across bundles');
    assert.equal(updated.activeMappings, undefined, 'same-ID package updates invalidate prepared mappings');
    assert.strictEqual(await selectBundle.getResourceCatalog(selectFile), updated);

    // Exercise the real deletion hook from yet another module namespace.
    const storage = require(storageEntry);
    const reloadGate = deferred();
    const reloadStarted = deferred();
    const reload = listBundle.withResourceOperation(async () => {
      reloadStarted.resolve();
      await reloadGate.promise;
    });
    await reloadStarted.promise;
    const receiverEvents = [];
    const receive = receiverBundle.withResourceOperation(async () => { receiverEvents.push('write'); });
    file.removeDirectory = async uri => {
      file.record('removeDirectory', uri);
      file.text.delete(manifestUri('base'));
    };
    const storageFile = makeFile();
    storageFile.readOptionalText = async () => { throw new Error('detached page adapter must not be used'); };
    const deleting = storage.removeInstalledTheme('base', storageFile);
    await drainCallbacks();
    assert.deepEqual(receiverEvents, [], 'receiver writes wait for a reload in another bundle');
    assert.equal(file.count('removeDirectory'), 0, 'deletion cannot mutate a captured reload snapshot');
    assert.equal(listBundle.isResourceCatalogCurrent(updated), true);
    reloadGate.resolve();
    await Promise.all([reload, receive, deleting]);
    assert.deepEqual(receiverEvents, ['write']);
    assert.equal(storageFile.calls.length, 0, 'queued deletion uses the application adapter, not a detached page');
    const failedOperation = receiverBundle.withResourceOperation(async () => { throw new Error('operation failed'); });
    await assert.rejects(failedOperation, /operation failed/);
    await selectBundle.withResourceOperation(async () => { receiverEvents.push('recovered'); });
    assert.deepEqual(receiverEvents, ['write', 'recovered'], 'failed operations do not poison the queue');
    assert.equal(selectBundle.isResourceCatalogCurrent(updated), false);
    const removed = await selectBundle.getResourceCatalog(selectFile);
    assert.deepEqual(removed.installedThemeIds, []);
    assert.deepEqual(removed.paths, []);
    assert.equal(removed.activeMappings, undefined, 'deletion cannot reuse an older activation plan');
    assert.equal(savedOverrides(file)['/resource/shared.bin'], '@default');

    // Teardown invalidates identity as well as revision, without restarting old I/O.
    api.destroyResourceCatalogSession();
    assert.equal(listBundle.isResourceCatalogCurrent(removed), false);
    const restartedFile = indexedFile(['base']);
    api.initializeResourceCatalogSession(restartedFile);
    assert.equal(selectBundle.isResourceCatalogCurrent(removed), false);
    const pendingGate = deferred();
    restartedFile.onRead = async (uri, value) => {
      if (uri === manifestUri('base')) await pendingGate.promise;
      return value;
    };
    const pending = listBundle.getResourceCatalog(listFile);
    const rejected = assert.rejects(pending, /会话已结束/);
    await restartedFile.when('read', manifestUri('base'));
    api.destroyResourceCatalogSession();
    const before = restartedFile.calls.length;
    pendingGate.resolve();
    await rejected;
    assert.equal(restartedFile.calls.length, before, 'teardown does not retry with a page-local fallback');
  } finally {
    api.destroyResourceCatalogSession();
  }
}

async function main() {
  const temporary = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'resource-hook-catalog-async-')));
  // Unresolved callback gates must fail rather than let Node silently exit successfully.
  // Event-loop idleness is deterministic and does not impose an elapsed-time deadline.
  const idle = deferred();
  const failOnIdle = () => idle.reject(new Error('Test stalled waiting for an expected callback'));
  process.once('beforeExit', failOnIdle);
  try {
    execFileSync(path.join(root, 'manager/node_modules/.bin/tsc'), [
      path.join(root, 'manager/src/ts/resource-overrides.ts'),
      path.join(root, 'manager/src/ts/resource-order.ts'),
      path.join(root, 'manager/src/ts/resource-storage.ts'),
      '--outDir', temporary, '--module', 'commonjs', '--target', 'es2018',
      '--lib', 'es2018,dom', '--skipLibCheck'
    ], { stdio: 'inherit' });
    const api = require(path.join(temporary, 'resource-overrides.js'));
    const order = require(path.join(temporary, 'resource-order.js'));
    await Promise.race([(async () => {
      await testSharedCatalog(api);
      await testInvalidationAndRetry(api);
      await testFallbackAndCancellation(api);
      await testOverrideTransactions(api);
      await testReadOnlyDisplay(api, order);
      await testPageReadBudget(api, order);
      await testApplicationSession(api, path.join(temporary, 'resource-overrides.js'),
        path.join(temporary, 'resource-storage.js'));
    })(), idle.promise]);
    console.log('Async resource catalog caching, cancellation, mutation queues and read-only display tests passed.');
  } finally {
    process.removeListener('beforeExit', failOnIdle);
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
