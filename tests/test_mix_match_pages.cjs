/* Host tests for parallel page loading, retained rows and lifecycle guards. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const root = path.resolve(__dirname, '..');
const DEFAULT_RESOURCE_CHOICE = '@default';
const SYSTEM_RESOURCE_CHOICE = '@system';
const RESOURCE_ORDER_URI = 'internal://files/resource-order.json';

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}

function catalog(revision = 1) {
  const paths = ['a', 'b'].map(name => ({
    sourcePath: `/resource/${name}.bin`,
    themes: [
      { themeId: 'base', name: 'Base', previewUri: `base/${name}.bin` },
      { themeId: 'dark', name: 'Dark', previewUri: `dark/${name}.bin` }
    ]
  }));
  return { revision, installedThemeIds: ['base', 'dark'], paths,
    byPath: new Map(paths.map(item => [item.sourcePath, item])) };
}

function page(name, dependencies = {}) {
  const source = fs.readFileSync(path.join(root, `manager/src/pages/${name}/${name}.ux`), 'utf8');
  const script = /<script>([\s\S]*?)<\/script>/.exec(source)[1]
    .replace(/^import[\s\S]*?from "[^"]+"\r?\n/gm, '')
    .replace('export default', 'globalThis.page =');
  const context = {
    DEFAULT_RESOURCE_CHOICE, SYSTEM_RESOURCE_CHOICE, RESOURCE_ORDER_URI,
    getResourceCatalog: async () => catalog(),
    getResourceFileApi: file => file,
    withResourceOperation: operation => operation(),
    isResourceCatalogCurrent: () => true,
    loadResourceOverrides: async () => ({}),
    resolveResourceChoice(registration, overrides) {
      const choice = overrides[registration.sourcePath] || DEFAULT_RESOURCE_CHOICE;
      return choice === DEFAULT_RESOURCE_CHOICE || choice === SYSTEM_RESOURCE_CHOICE ||
        registration.themes.some(theme => theme.themeId === choice) ? choice : DEFAULT_RESOURCE_CHOICE;
    },
    resolveResourceOrder: ids => [...ids, SYSTEM_RESOURCE_CHOICE],
    saveResourceOverride: async () => {},
    file: { readOptionalText: async () => null },
    prompt: { showToast() {} },
    router: { back() {}, push() {} },
    ...dependencies
  };
  vm.runInNewContext(script, context, { filename: `${name}.ux` });
  return { ...context.page, ...context.page.private, visible: true };
}

async function testList() {
  const snapshot = catalog();
  const catalogRead = deferred();
  const overrideRead = deferred();
  const started = [];
  let choices = {};
  let cold = true;
  const list = page('mix-match', {
    getResourceCatalog() { started.push('catalog'); return cold ? catalogRead.promise : Promise.resolve(snapshot); },
    loadResourceOverrides() { started.push('overrides'); return cold ? overrideRead.promise : Promise.resolve(choices); }
  });
  const initial = list.loadCatalog();
  assert.deepEqual(started, ['catalog', 'overrides'], 'independent reads start before either completes');
  catalogRead.resolve(snapshot);
  overrideRead.resolve({});
  await initial;
  assert.equal(list.loaded, true);
  assert.equal(list.statusText, '');
  const rows = list.rows;
  const first = rows[0];
  cold = false;
  list.onHide();
  choices = { '/resource/a.bin': 'dark' };
  list.visible = true;
  const returning = list.loadCatalog();
  assert.equal(list.loaded, true, 'returning does not unmount the list');
  assert.equal(list.statusText, '', 'returning does not flash the loading state');
  await returning;
  assert.equal(list.rows, rows, 'returning retains the rows array');
  assert.equal(list.rows[0], first, 'returning retains individual row identity');
  assert.equal(first.choiceName, 'Dark');
  assert.equal(first.choiceId, 'dark');

  const old = deferred();
  let calls = 0;
  const guarded = page('mix-match', {
    getResourceCatalog() { return calls++ === 0 ? old.promise : Promise.resolve(snapshot); }
  });
  const outdated = guarded.loadCatalog();
  guarded.onHide();
  guarded.visible = true;
  await guarded.loadCatalog();
  const currentRows = guarded.rows;
  old.reject(new Error('late read failure'));
  await outdated;
  assert.equal(guarded.rows, currentRows, 'a late failure cannot clear a newer screen');
  assert.equal(guarded.statusText, '');

  const hiddenRead = deferred();
  const hidden = page('mix-match', { getResourceCatalog: () => hiddenRead.promise });
  const pending = hidden.loadCatalog();
  hidden.onHide();
  hiddenRead.resolve(snapshot);
  await pending;
  assert.equal(hidden.loaded, false, 'hidden pages ignore completed loads');
}

async function testSelection() {
  const snapshot = catalog();
  const gates = [deferred(), deferred(), deferred()];
  const started = [];
  const saves = [];
  const toasts = [];
  let saveGate = deferred();
  const select = page('mix-match-select', {
    getResourceCatalog() { started.push('catalog'); return gates[0].promise; },
    loadResourceOverrides() { started.push('overrides'); return gates[1].promise; },
    file: { readOptionalText(uri) {
      assert.equal(uri, RESOURCE_ORDER_URI);
      started.push('order');
      return gates[2].promise;
    } },
    resolveResourceOrder(ids, text) {
      assert.equal(ids, snapshot.installedThemeIds);
      assert.equal(text, 'ordering snapshot');
      return ['dark', '@system', 'base'];
    },
    saveResourceOverride(source, choice) { saves.push([source, choice]); return saveGate.promise; },
    prompt: { showToast(value) { toasts.push(value); } }
  });
  select.sourcePath = '/resource/a.bin';
  const loading = select.loadChoices();
  assert.deepEqual(started, ['catalog', 'overrides', 'order']);
  gates[0].resolve(snapshot);
  gates[1].resolve({ '/resource/a.bin': 'base' });
  gates[2].resolve('ordering snapshot');
  await loading;
  assert.equal(select.loaded, true);
  assert.deepEqual(Array.from(select.rows, row => row.choiceId), ['@default', '@system', 'dark', 'base']);
  assert.equal(select.selectedChoice, 'base');
  const rows = select.rows;
  const saving = select.selectChoice('dark');
  await select.selectChoice('@default');
  assert.equal(saves.length, 1, 'rapid taps do not launch a second save');
  assert.equal(select.saving, true);
  saveGate.resolve();
  await saving;
  assert.equal(select.rows, rows, 'selection changes marks without recreating preview rows');
  assert.equal(select.selectedChoice, 'dark');
  assert.equal(select.rows.find(row => row.choiceId === 'dark').selected, true);
  assert.equal(select.saving, false);

  saveGate = deferred();
  const hiddenSave = select.selectChoice('base');
  select.onHide();
  saveGate.reject(new Error('late save failure'));
  await hiddenSave;
  assert.equal(toasts.length, 0, 'hidden pages do not emit obsolete error feedback');
  assert.equal(select.selectedChoice, 'dark');
  assert.equal(select.saving, false, 'save guard is released even after hiding');

  select.visible = true;
  saveGate = deferred();
  const failed = select.selectChoice('base');
  saveGate.reject(new Error('save failed'));
  await failed;
  assert.equal(select.statusText, 'save failed');
  assert.equal(toasts.length, 1);
  assert.equal(select.saving, false);

  const hiddenCatalog = deferred();
  const hidden = page('mix-match-select', { getResourceCatalog: () => hiddenCatalog.promise });
  hidden.sourcePath = '/resource/a.bin';
  const pending = hidden.loadChoices();
  hidden.onHide();
  hiddenCatalog.resolve(snapshot);
  await pending;
  assert.equal(hidden.loaded, false);
  assert.equal(hidden.rows.length, 0);

  const invalid = page('mix-match-select', {
    getResourceCatalog() { throw new Error('invalid path must not load resources'); }
  });
  invalid.sourcePath = '/resource/';
  await invalid.loadChoices();
  assert.equal(invalid.statusText, '替换资源路径无效');
}

async function testReload() {
  const source = fs.readFileSync(path.join(root, 'manager/src/pages/index/index.ux'), 'utf8');
  const opacityExpression = /class="reload-action"\s+style="opacity:\s*\{\{(.*?)\}\};"/.exec(source);
  assert(opacityExpression, 'reload button opacity is bound to its busy state');
  const opacity = home => vm.runInNewContext(opacityExpression[1], { sendingReload: home.sendingReload });
  const receipt = deferred();
  const receiptStarted = deferred();
  const cleanup = deferred();
  const cleanupStarted = deferred();
  const order = deferred();
  const paths = deferred();
  const readsStarted = deferred();
  const orderStarted = deferred();
  const snapshot = catalog();
  const nativeFile = {};
  const started = [];
  const home = page('index', {
    getResourceFileApi() { return nativeFile; },
    invalidateResourceCatalog() { throw new Error('Reload must retain the package snapshot'); },
    getResourceCatalog(file) {
      assert.equal(file, nativeFile);
      started.push('catalog');
      readsStarted.resolve();
      return paths.promise;
    },
    loadResourceOrder(ids, file) {
      assert.equal(ids, snapshot.installedThemeIds);
      assert.equal(file, nativeFile);
      started.push('order');
      orderStarted.resolve();
      return order.promise;
    },
    async reconcileResourceOverrides(paths) {
      assert.equal(paths, snapshot.paths);
      started.push('reconcile');
      return {};
    },
    async regenerateActiveMappings(order, revision, file, overrides, input) {
      assert.equal(file, nativeFile);
      assert.equal(input, snapshot, 'activation consumes the app-owned reusable snapshot');
      started.push('generate');
      return { generation: 'test' };
    },
    async sendReloadSignal() { started.push('signal'); },
    async waitForReloadOutcome() {
      receiptStarted.resolve();
      return receipt.promise;
    },
    async cleanupInactiveGenerations() {
      started.push('cleanup');
      cleanupStarted.resolve();
      await cleanup.promise;
    }
  });
  assert.equal(opacity(home), 1);
  const pending = home.requestReload();
  assert.equal(home.sendingReload, true, 'busy starts synchronously on the first click');
  assert.equal(opacity(home), 0.4);
  const revision = home.reloadRevision;
  for (let click = 0; click < 10; click++) await home.requestReload();
  assert.equal(home.reloadRevision, revision, 'rapid clicks cannot start another reload');
  await readsStarted.promise;
  assert.deepEqual(started, ['catalog']);
  paths.resolve(snapshot);
  await orderStarted.promise;
  assert.deepEqual(started, ['catalog', 'order']);
  order.resolve(['base', '@system']);
  await receiptStarted.promise;
  assert.equal(opacity(home), 0.4, 'busy persists while waiting for the module receipt');
  await home.requestReload();
  assert.equal(home.reloadRevision, revision);
  receipt.resolve({ successful: true, message: 'done' });
  await cleanupStarted.promise;
  assert.equal(opacity(home), 0.4, 'busy persists until generation cleanup finishes');
  cleanup.resolve();
  await pending;
  assert.deepEqual(started, ['catalog', 'order', 'reconcile', 'generate', 'signal', 'cleanup']);
  assert.equal(home.sendingReload, false);
  assert.equal(opacity(home), 1);

  for (const failure of ['snapshot', 'copy', 'receipt']) {
    const events = [];
    const failed = page('index', {
      invalidateResourceCatalog() { throw new Error('Reload must not invalidate resources'); },
      loadResourceOrder: async () => ['base', '@system'],
      async getResourceCatalog() {
        if (failure === 'snapshot') throw new Error('snapshot failed');
        return snapshot;
      },
      reconcileResourceOverrides: async () => ({}),
      async regenerateActiveMappings() {
        events.push('generate');
        if (failure === 'copy') throw new Error('copy failed');
        return { generation: 'new' };
      },
      async sendReloadSignal() { events.push('signal'); },
      async waitForReloadOutcome() { return { successful: false, message: 'rejected' }; },
      async cleanupInactiveGenerations() { events.push('cleanup'); },
      prompt: { showToast() {} }
    });
    await failed.requestReload();
    assert.equal(events.includes('signal'), failure === 'receipt', 'failed preparation cannot send reload');
    assert.equal(events.includes('cleanup'), false, 'old generations survive a failed reload');
    assert.equal(failed.sendingReload, false);
    assert.equal(opacity(failed), 1, 'errors restore full opacity and allow another click');
  }

  const revisions = [];
  const retained = catalog();
  let expectedOrder = ['base', 'dark', '@system'];
  let expectedOverrides = {};
  let acknowledged = false;
  let cleanups = 0;
  const dependencies = {
    invalidateResourceCatalog() { throw new Error('Repeated reload cannot drop the cached snapshot'); },
    getResourceCatalog: async () => retained,
    loadResourceOrder: async ids => {
      assert.equal(ids, retained.installedThemeIds);
      return expectedOrder;
    },
    reconcileResourceOverrides: async () => expectedOverrides,
    async regenerateActiveMappings(order, revision, file, overrides, input) {
      assert.equal(input, retained, 'returning home shares the same snapshot');
      assert.equal(order, expectedOrder, 'order is reread even when the package snapshot is cached');
      assert.equal(overrides, expectedOverrides, 'choices are reread even when the package snapshot is cached');
      return { generation: 'retained' };
    },
    async sendReloadSignal(revision) { revisions.push(revision); },
    async waitForReloadOutcome(revision) {
      assert.equal(revision, revisions[revisions.length - 1]);
      return { successful: acknowledged, message: 'result' };
    },
    async cleanupInactiveGenerations(generation) {
      assert.equal(generation, 'retained');
      cleanups++;
    }
  };
  await page('index', dependencies).requestReload();
  assert.equal(cleanups, 0, 'a rejected receipt cannot clean the reusable generation');
  acknowledged = true;
  const returningHome = page('index', dependencies);
  await returningHome.requestReload();
  expectedOrder = ['dark', 'base', '@system'];
  expectedOverrides = { '/resource/a.bin': '@system' };
  await returningHome.requestReload();
  assert.equal(new Set(revisions).size, 3, 'each cached reload still sends a fresh request revision');
  assert.equal(cleanups, 2);

  const queue = deferred();
  const queued = page('index', { withResourceOperation: () => queue.promise });
  const queuedReload = queued.requestReload();
  assert.equal(opacity(queued), 0.4, 'waiting for another resource operation is also busy');
  await queued.requestReload();
  queue.reject(new Error('operation queue failed'));
  await assert.rejects(queuedReload, /operation queue failed/);
  assert.equal(opacity(queued), 1, 'queue rejection also releases the busy state');
}

const idle = deferred();
const failOnIdle = () => idle.reject(new Error('Page test stalled waiting for an expected callback'));
process.once('beforeExit', failOnIdle);
Promise.race([(async () => {
  await testList();
  await testSelection();
  await testReload();
  console.log('Mix-match parallel loading, retained rows, lifecycle and reload sequencing tests passed.');
})(), idle.promise]).finally(() => {
  process.removeListener('beforeExit', failOnIdle);
}).catch(error => {
  console.error(error);
  process.exitCode = 1;
});
