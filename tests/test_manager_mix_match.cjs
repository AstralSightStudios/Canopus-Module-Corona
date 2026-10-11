/* Host regressions for feature rows and source selection in Manager pages. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '..');
const items = [
  { id: 'launcher', name: '启动器背景' },
  { id: 'controlCenter', name: '控制中心背景' },
  { id: 'notifications', name: '通知中心背景' },
  { id: 'widgets', name: '快捷组件背景' },
  { id: 'calendar', name: '日历样式' }
];
const pack = { themeId: 'pack', name: 'Pack', previewUri: '' };
const registration = { sourcePath: '/resource/a.bin', themes: [pack] };
const catalog = { revision: 1, paths: [registration], byPath: new Map([[registration.sourcePath, registration]]) };
let features = {};
let overrides = {};
let available = true;
let documentReads = 0;
const saves = [];
const routes = [];
const globals = {
  DEFAULT_RESOURCE_CHOICE: '@default', SYSTEM_RESOURCE_CHOICE: '@system', BLUR_RESOURCE_CHOICE: '@blur',
  FEATURE_RESOURCE_ITEMS: items,
  getResourceCatalog: async () => catalog,
  isResourceCatalogCurrent: () => true,
  loadResourceOverrideDocument: async () => { documentReads++; return { overrides, features }; },
  loadResourceOverrides() { throw new Error('Pages must read both selections together'); },
  loadFeatureOverrides() { throw new Error('Pages must read both selections together'); },
  featureResourceOptions: () => available ? [pack] : [],
  resolveFeatureChoice: (_catalog, _id, choices) => {
    const choice = choices[_id] || '@default';
    return choice !== 'pack' || available ? choice : '@default';
  },
  resolveResourceChoice: (item, choices) => choices[item.sourcePath] || '@default',
  validFeatureResourceId: id => items.some(item => item.id === id),
  validSourcePath: value => typeof value === 'string' && value.startsWith('/resource/'),
  saveFeatureOverride: async (id, choice) => { features[id] = choice; saves.push(['feature', id, choice]); },
  saveResourceOverride: async (source, choice) => { overrides[source] = choice; saves.push(['resource', source, choice]); },
  RESOURCE_ORDER_URI: 'order',
  resolveResourceOrder: () => ['pack', '@system'],
  file: { readOptionalText: async () => null },
  router: { push: route => routes.push(route), back() {} },
  prompt: { showToast() {} }
};
function page(name, values = {}) {
  const source = fs.readFileSync(path.join(root, `manager/src/pages/${name}/${name}.ux`), 'utf8');
  const script = /<script>([\s\S]*?)<\/script>/.exec(source)[1]
    .replace(/^import[\s\S]*?from "[^"]+"\n/gm, '')
    .replace('export default', 'globalThis.page =');
  const context = { ...globals };
  vm.runInNewContext(script, context);
  for (const key of Object.keys(values)) {
    if (key === 'featureId' || key === 'sourcePath')
      assert(Object.hasOwn(context.page.protected || {}, key), 'route parameters must use protected data');
  }
  return { ...context.page, ...context.page.private, ...context.page.protected,
    $nextTick(callback) { callback(); }, visible: true, ...values };
}
const names = rows => Array.from(rows, row => row.name);
const listNames = list => Array.from(list.rows, row => list.rowName(row.rowId));
async function main() {
  const list = page('mix-match');
  await list.loadCatalog();
  assert.equal(documentReads, 1, 'the list loads ordinary and feature choices in one document');
  assert.deepEqual(listNames(list), [...items.map(item => item.name), registration.sourcePath]);
  for (const row of list.rows)
    assert.deepEqual(Object.keys(row).sort(), ['choiceId', 'choiceName', 'rowId'], 'all rows have only three reactive fields');
  assert.equal(new Set(list.rows.map(row => row.rowId)).size, 6, 'feature and path identities are unique');
  list.openRow(list.rows[4].rowId);
  assert.deepEqual(JSON.parse(JSON.stringify(routes.pop())), {
    uri: 'pages/mix-match-select', params: { featureId: 'calendar' }
  });
  assert.equal(list.rows.length, 0, 'feature navigation releases the list');
  await list.onShow();
  list.openRow(list.rows[5].rowId);
  assert.equal(routes.pop().params.sourcePath, registration.sourcePath);
  assert.equal(list.pathRows.length, 0, 'ordinary navigation releases the cached row order');
  await list.onShow();

  const retained = list.pathRows.slice();
  features = { calendar: 'pack', controlCenter: '@blur' };
  overrides = { [registration.sourcePath]: '@system' };
  await list.loadCatalog();
  assert.deepEqual(listNames(list), ['控制中心背景', '日历样式', registration.sourcePath,
    '启动器背景', '通知中心背景', '快捷组件背景']);
  assert.equal(list.rows[0].choiceName, '模糊');
  assert.equal(list.rows[1].choiceName, 'Pack');
  assert(list.pathRows.every((row, index) => row === retained[index]), 'visible refreshes retain row objects');
  features = {};
  overrides = {};
  await list.loadCatalog();
  assert.deepEqual(listNames(list), [...items.map(item => item.name), registration.sourcePath]);

  const readsBeforeCalendar = documentReads;
  const calendar = page('mix-match-select', { featureId: 'calendar' });
  await calendar.loadChoices();
  assert.equal(documentReads, readsBeforeCalendar + 1, 'feature selection loads one configuration document');
  assert.deepEqual(names(calendar.rows), ['默认', '系统', 'Pack']);
  assert.equal(calendar.headerTitle, '< 日历样式');
  await calendar.selectChoice('pack');
  assert.deepEqual(saves.pop(), ['feature', 'calendar', 'pack']);
  assert.equal(calendar.rows.find(row => row.choiceId === 'pack').selected, true);
  await calendar.selectChoice('@blur');
  assert.equal(saves.length, 0, 'calendar cannot save blur');

  const background = page('mix-match-select', { featureId: 'launcher' });
  await background.loadChoices();
  assert.deepEqual(names(background.rows), ['默认', '系统', '模糊', 'Pack']);
  await background.selectChoice('@blur');
  assert.deepEqual(saves.pop(), ['feature', 'launcher', '@blur']);
  const readsBeforeResource = documentReads;
  const resource = page('mix-match-select', { sourcePath: registration.sourcePath });
  await resource.loadChoices();
  assert.equal(documentReads, readsBeforeResource + 1, 'ordinary selection also loads one configuration document');
  assert.deepEqual(names(resource.rows), ['默认', '系统', 'Pack']);
  await resource.selectChoice('@system');
  assert.deepEqual(saves.pop(), ['resource', registration.sourcePath, '@system']);

  available = false;
  const empty = page('mix-match-select', { featureId: 'calendar' });
  await empty.loadChoices();
  assert.equal(empty.selectedChoice, '@default', 'removed feature sources fall back to default');
  assert.deepEqual(names(empty.rows), ['默认', '系统']);
  const invalid = page('mix-match-select', { featureId: 'invalid', sourcePath: registration.sourcePath });
  await invalid.loadChoices();
  assert.equal(invalid.loaded, false, 'an invalid feature route cannot become an ordinary path route');
  catalog.paths = [];
  catalog.byPath.clear();
  catalog.revision++;
  const noPacks = page('mix-match');
  await noPacks.loadCatalog();
  assert.deepEqual(listNames(noPacks), ['启动器背景', ...items.slice(1).map(item => item.name)],
    'feature rows remain accessible with an empty resource catalog');
  const failed = page('mix-match-select', { featureId: 'widgets' });
  await failed.loadChoices();
  failed.visible = false;
  await failed.selectChoice('@blur');
  assert.equal(saves.length, 0, 'hidden pages do not save selections');

  const originalLoad = globals.loadResourceOverrideDocument;
  let resume;
  globals.loadResourceOverrideDocument = () => new Promise(resolve => { resume = resolve; });
  const stale = page('mix-match');
  const pending = stale.loadCatalog();
  stale.onHide();
  resume({ overrides: {}, features: {} });
  await pending;
  assert.equal(stale.loaded, false, 'late loading results cannot update hidden pages');
  globals.loadResourceOverrideDocument = originalLoad;

  const originalSave = globals.saveFeatureOverride;
  globals.saveFeatureOverride = async () => { throw new Error('write failed'); };
  const writeFailure = page('mix-match-select', { featureId: 'notifications' });
  await writeFailure.loadChoices();
  await writeFailure.selectChoice('@blur');
  assert.equal(writeFailure.selectedChoice, '@default');
  assert.equal(writeFailure.saving, false, 'failed writes release the save guard');
  assert.equal(writeFailure.statusText, 'write failed');
  globals.saveFeatureOverride = originalSave;
  console.log('Manager mix-match page tests passed.');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
