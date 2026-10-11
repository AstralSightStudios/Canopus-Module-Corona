/* Host tests for whole-calendar and per-region background mix-match choices. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'resource-hook-features-'));
const themeRoot = 'internal://files/themes/';
const overridesUri = 'internal://files/resource-overrides.json';
const registryUri = 'internal://files/resource-active-generations.json';
const fileIndexUri = 'internal://files/resource-files.json';
const base = id => ({ format: 'canopus-resource-pack', formatVersion: 1, themeId: id, name: id, mappings: [] });
const textItem = { type: 'text', x: 1, y: 2, align: 'center', size: 24 };

function makeFile() {
  const text = new Map();
  const assets = new Map();
  const calls = [];
  return {
    text, assets, calls,
    async readOptionalText(uri) { calls.push(['read', uri]); return text.get(uri) ?? null; },
    async writeText(uri, value) { calls.push(['write', uri]); text.set(uri, value); },
    async readFileInfo(uri, recursive) {
      calls.push(['info', uri]);
      if (assets.has(uri)) return { type: 'file', length: assets.get(uri) };
      if (recursive) return { type: 'dir', length: 0, subFiles: [...assets]
        .filter(([key]) => key.startsWith(uri)).map(([uri, length]) => ({ uri, type: 'file', length })) };
      throw Object.assign(new Error(`missing ${uri}`), { code: 301 });
    },
    async listDirectory() { throw new Error('unexpected directory fallback'); },
    async makeDirectory(uri) { calls.push(['mkdir', uri]); },
    async copyFile(from, to) { assert(assets.has(from)); assets.set(to, assets.get(from)); },
    async removeDirectory(uri) { calls.push(['remove', uri]); },
    isFileNotFound(error) { return error.code === 301; }
  };
}

async function testOverrideDocument(choices) {
  const file = makeFile();
  const expected = { overrides: { '/resource/ordinary.bin': '@system' }, features: { calendar: 'inactive', launcher: '@blur' } };
  file.text.set(overridesUri, JSON.stringify({ version: 2, ...expected }));
  const originalParse = JSON.parse;
  let parses = 0;
  JSON.parse = (...args) => { parses++; return originalParse(...args); };
  let document;
  try {
    document = await choices.loadResourceOverrideDocument(file);
  } finally {
    JSON.parse = originalParse;
  }
  assert.deepEqual({ overrides: { ...document.overrides }, features: { ...document.features } }, expected);
  assert.deepEqual(file.calls, [['read', overridesUri]], 'both sections need one document read');
  assert.equal(parses, 1, 'both sections need one document parse');

  file.text.set(overridesUri, JSON.stringify({ version: 1, overrides: expected.overrides }));
  const legacy = await choices.loadResourceOverrideDocument(file);
  assert.deepEqual({ ...legacy.overrides }, expected.overrides);
  assert.deepEqual({ ...legacy.features }, {}, 'legacy documents have no feature selections');
  file.text.delete(overridesUri);
  const missing = await choices.loadResourceOverrideDocument(file);
  assert.deepEqual({ ...missing.overrides }, {});
  assert.deepEqual({ ...missing.features }, {});
  assert.equal(Object.getPrototypeOf(missing.overrides), null);
  assert.equal(Object.getPrototypeOf(missing.features), null);
  assert.equal(file.calls.length, 3, 'later reads see current disk contents instead of cached documents');

  const originalRead = file.readOptionalText;
  const nativeError = Object.assign(new Error('native read failed'), { code: 300 });
  file.readOptionalText = async () => { throw nativeError; };
  await assert.rejects(choices.loadResourceOverrideDocument(file), error => error === nativeError,
    'native I/O errors pass through unchanged');
  file.readOptionalText = originalRead;
  for (const [text, message] of [
    ['{broken', /配置损坏/],
    [' '.repeat(64 * 1024 + 1), /超过 64 KiB/],
    [JSON.stringify({ version: 2, overrides: { '/resource/': '@system' }, features: {} }), /无效路径或选项/]
  ]) {
    file.text.set(overridesUri, text);
    await assert.rejects(choices.loadResourceOverrideDocument(file), message);
  }
  file.text.set(overridesUri, JSON.stringify({ version: 2, ...expected }));
  const recovered = await choices.loadResourceOverrideDocument(file);
  assert.deepEqual({ overrides: { ...recovered.overrides }, features: { ...recovered.features } }, expected,
    'failed reads and invalid documents do not poison subsequent loads');
}

async function main() {
  execFileSync(path.join(root, 'manager/node_modules/.bin/tsc'), [
    path.join(root, 'manager/src/ts/resource-overrides.ts'), '--outDir', temporary,
    '--module', 'commonjs', '--target', 'es2018', '--lib', 'es2018,dom', '--skipLibCheck'
  ], { stdio: 'inherit' });
  const choices = require(path.join(temporary, 'resource-overrides.js'));
  await testOverrideDocument(choices);
  const activation = require(path.join(temporary, 'resource-activation.js'));
  const pack = require(path.join(temporary, 'resource-pack.js'));
  const parsed = value => pack.parseResourcePackManifest(JSON.stringify(value));
  const image = { ...textItem, type: 'image', size: 0, context: Array(31).fill('date.bin') };
  const active = parsed({ ...base('active'), calendar: { month: { type: 'none' }, day: textItem },
    backgrounds: { launcher: 'active.bin', controlCenter: 'active.bin', notifications: 'active.bin' } });
  const inactive = parsed({ ...base('inactive'), calendar: { day: image },
    backgrounds: { launcher: 'inactive.bin', notifications: { mode: 'blur' }, widgets: 'widget.bin' },
    mappings: [{ source: '/resource/ordinary.bin', destination: 'ordinary.bin' }] });
  const empty = parsed({ ...base('empty'), calendar: {} });
  const plain = parsed(base('plain'));
  const filesFor = id => id === 'active' ? [{ relativePath: 'active.bin', sizeBytes: 1 }] : id === 'inactive'
    ? ['inactive.bin', 'widget.bin', 'date.bin', 'ordinary.bin'].map(relativePath => ({ relativePath, sizeBytes: 1 })) : [];
  const snapshot = { installedThemeIds: ['active', 'inactive', 'empty', 'plain'],
    themes: new Map([active, inactive, empty, plain].map(manifest => [manifest.themeId,
      { themeId: manifest.themeId, manifest, files: filesFor(manifest.themeId) }])), paths: [], byPath: new Map() };
  const order = ['active', '@system', 'inactive', 'empty', 'plain'];
  const file = makeFile();
  for (const manifest of [active, inactive, empty, plain]) {
    file.text.set(`${themeRoot}${manifest.themeId}/corona.json`, JSON.stringify(manifest));
    for (const asset of filesFor(manifest.themeId)) file.assets.set(`${themeRoot}${manifest.themeId}/${asset.relativePath}`, 1);
  }

  assert.deepEqual(choices.FEATURE_RESOURCE_ITEMS.map(item => item.id),
    ['launcher', 'controlCenter', 'notifications', 'widgets', 'calendar']);
  assert.deepEqual(choices.featureResourceOptions(snapshot, 'calendar').map(item => item.themeId),
    ['active', 'inactive', 'empty'], 'an empty declaration is a selectable native-default style');
  assert.deepEqual(choices.featureResourceOptions(snapshot, 'widgets'), [{ themeId: 'inactive', name: 'inactive',
    previewUri: `${themeRoot}inactive/widget.bin` }]);
  assert.equal(choices.resolveFeatureChoice(snapshot, 'calendar', { calendar: '@blur' }), '@default');
  assert.equal(choices.resolveFeatureChoice(snapshot, 'calendar', { calendar: 'plain' }), '@default');
  assert.equal(choices.resolveFeatureChoice(snapshot, 'launcher', { launcher: '@blur' }), '@blur');
  assert.equal(choices.validFeatureResourceId('/resource/x.bin'), false);
  await assert.rejects(choices.saveFeatureOverride('calendar', '@blur', file), /选项无效/);
  await assert.rejects(choices.saveFeatureOverride('unknown', '@system', file), /选项无效/);

  file.text.set(overridesUri, JSON.stringify({ version: 1, overrides: { '/resource/ordinary.bin': '@system' } }));
  assert.deepEqual({ ...await choices.loadFeatureOverrides(file) }, {}, 'legacy files have no feature overrides');
  await Promise.all([
    choices.saveFeatureOverride('calendar', 'inactive', file),
    choices.saveFeatureOverride('launcher', 'inactive', file),
    choices.saveResourceOverride('/resource/ordinary.bin', 'inactive', file),
    choices.saveFeatureOverride('controlCenter', '@blur', file),
    choices.saveFeatureOverride('notifications', '@system', file),
    choices.saveFeatureOverride('widgets', 'inactive', file)
  ]);
  const document = JSON.parse(file.text.get(overridesUri));
  assert.equal(document.version, 2);
  assert.deepEqual(document.overrides, { '/resource/ordinary.bin': 'inactive' });
  assert.deepEqual(document.features, { launcher: 'inactive', controlCenter: '@blur', notifications: '@system',
    widgets: 'inactive', calendar: 'inactive' }, 'queued cross-section saves cannot drop choices');
  await choices.reconcileResourceOverrides([], file);
  assert.deepEqual({ ...await choices.loadFeatureOverrides(file) }, document.features,
    'ordinary-path reconciliation preserves feature choices');

  const first = await activation.regenerateActiveMappings(order, 'first', file, {}, snapshot);
  assert.equal(first.mappings, '# No active resource replacements.\n',
    'feature-only selection does not activate inactive ordinary mappings');
  assert.equal(first.calendar, activation.serializeCalendar([{ themeId: 'inactive', manifest: inactive, files: [], active: false }], 'inactive'));
  assert(!first.calendar.includes('month\t'), 'whole-style selection does not merge the active month');
  assert.match(first.calendar, /themes\/inactive\/date.bin/);
  assert.equal(first.backgrounds, 'version=1\nlauncher=themes/inactive/inactive.bin\ncontrol_center=blur\nnotifications=system\nwidgets=themes/inactive/widget.bin\n');
  assert.deepEqual(await activation.readProtectedThemeIds(file), ['inactive']);
  assert.deepEqual(JSON.parse(file.text.get(registryUri)).protectedThemes, ['inactive']);
  const before = file.calls.length;
  assert.strictEqual(await activation.regenerateActiveMappings(order, 'cached', file, {}, snapshot), first);
  assert.deepEqual(file.calls.slice(before), [['read', overridesUri]], 'cache must read current feature choices but not republish');

  await choices.saveFeatureOverride('calendar', '@system', file);
  await choices.saveFeatureOverride('launcher', '@system', file);
  await choices.saveFeatureOverride('widgets', '@blur', file);
  const second = await activation.regenerateActiveMappings(order, 'second', file, {}, snapshot);
  assert.notStrictEqual(second, first, 'feature choices invalidate cached plans');
  assert.equal(second.calendar, 'version=1\n');
  assert.equal(second.backgrounds, 'version=1\nlauncher=system\ncontrol_center=blur\nnotifications=system\nwidgets=blur\n');
  assert.deepEqual(await activation.readProtectedThemeIds(file), ['inactive'], 'old runtime dependencies remain until acknowledgement');
  await activation.cleanupInactiveGenerations(null, file, second.mappings);
  assert.deepEqual(await activation.readProtectedThemeIds(file), []);

  await choices.saveFeatureOverride('calendar', 'empty', file);
  const reset = await activation.regenerateActiveMappings(order, 'reset', file, {}, snapshot);
  assert.equal(reset.calendar, 'version=1\n', 'an explicit empty declaration resets native defaults');
  await choices.saveFeatureOverride('calendar', '@default', file);
  await choices.saveFeatureOverride('launcher', '@default', file);
  const defaults = await activation.regenerateActiveMappings(order, 'defaults', file, {}, snapshot);
  assert.equal(defaults.calendar, activation.serializeCalendar([{ themeId: 'active', manifest: active, files: [] }]));
  assert.match(defaults.backgrounds, /launcher=themes\/active\/active.bin/);

  // Explicit sources work for legacy callers without a package snapshot too.
  await choices.saveFeatureOverride('calendar', 'inactive', file);
  await choices.saveFeatureOverride('launcher', 'inactive', file);
  const uncached = await activation.regenerateActiveMappings(order, 'uncached', file);
  assert.equal(uncached.calendar, first.calendar);
  assert.match(uncached.backgrounds, /launcher=themes\/inactive\/inactive.bin/);
  assert.equal(uncached.mappings, first.mappings);
  await choices.saveFeatureOverride('notifications', 'inactive', file);
  await choices.saveFeatureOverride('controlCenter', 'active', file);
  const mixed = await activation.regenerateActiveMappings(order, 'mixed', file, {}, snapshot);
  assert.match(mixed.backgrounds, /notifications=blur/,
    'explicit pack selection adopts its declared blur mode');
  assert.match(mixed.backgrounds, /control_center=themes\/active\/active.bin/,
    'each region can choose its own source');
  assert.equal(mixed.calendar, first.calendar);

  const brokenSnapshot = { ...snapshot, themes: new Map(snapshot.themes) };
  brokenSnapshot.themes.set('inactive', { ...snapshot.themes.get('inactive'), files: [] });
  delete brokenSnapshot.activeMappings;
  await assert.rejects(activation.regenerateActiveMappings(order, 'missing-image', file, {}, brokenSnapshot), /calendar 图片/);
  await choices.saveFeatureOverride('calendar', '@system', file);
  await assert.rejects(activation.regenerateActiveMappings(order, 'missing-bg', file, {}, brokenSnapshot), /backgrounds.launcher.image/);

  await choices.saveResourceOverride('/resource/ordinary.bin', 'inactive', file);
  await choices.saveFeatureOverride('calendar', 'inactive', file);
  await choices.removeThemeFromResourceOverrides('inactive', file);
  assert.equal((await choices.loadResourceOverrides(file))['/resource/ordinary.bin'], '@default');
  const removed = await choices.loadFeatureOverrides(file);
  assert.equal(removed.calendar, '@default');
  assert.equal(removed.launcher, '@default');
  assert.equal(removed.widgets, '@blur', 'deletion does not clear built-in choices');
  assert.equal(removed.notifications, '@default');

  await choices.saveFeatureOverride('calendar', 'plain', file);
  await choices.saveFeatureOverride('launcher', 'missing', file);
  await activation.regenerateActiveMappings(order, 'stale', file, {}, snapshot);
  assert.equal((await choices.loadFeatureOverrides(file)).calendar, '@default');
  assert.equal((await choices.loadFeatureOverrides(file)).launcher, '@default');
  await choices.saveFeatureOverride('calendar', 'inactive', file);
  await choices.saveFeatureOverride('launcher', 'inactive', file);
  const updatedSnapshot = { ...snapshot, themes: new Map(snapshot.themes) };
  delete updatedSnapshot.activeMappings;
  updatedSnapshot.themes.set('inactive', { ...snapshot.themes.get('inactive'),
    manifest: { ...plain, themeId: 'inactive', name: 'inactive' } });
  const updated = await activation.regenerateActiveMappings(order, 'updated', file, {}, updatedSnapshot);
  assert.equal((await choices.loadFeatureOverrides(file)).calendar, '@default');
  assert.equal((await choices.loadFeatureOverrides(file)).launcher, '@default');
  assert.equal(updated.calendar, defaults.calendar,
    'same-ID update removing declarations falls back to active default style');

  // Background-only packages still populate image inventories when no ordinary mappings exist.
  const bgOnly = makeFile();
  const bgManifest = { ...base('bgonly'), backgrounds: { launcher: 'bg.bin' } };
  bgOnly.text.set(`${themeRoot}bgonly/corona.json`, JSON.stringify(bgManifest));
  bgOnly.text.set(fileIndexUri, JSON.stringify({ version: 1, themes: { bgonly: [{ relativePath: 'corona.json', sizeBytes: 1 }] } }));
  bgOnly.assets.set(`${themeRoot}bgonly/bg.bin`, 1);
  const bgSnapshot = await choices.loadResourcePackSnapshot(['bgonly'], bgOnly);
  assert(bgSnapshot.themes.get('bgonly').files.some(asset => asset.relativePath === 'bg.bin'));
  await choices.saveFeatureOverride('launcher', 'bgonly', bgOnly);
  assert.match((await activation.regenerateActiveMappings(['@system', 'bgonly'], 'bg-only', bgOnly, {}, bgSnapshot)).backgrounds,
    /launcher=themes\/bgonly\/bg.bin/);

  for (const bad of [
    { version: 2, overrides: {}, features: { calendar: '@blur' } },
    { version: 2, overrides: {}, features: { unknown: '@system' } },
    { version: 2, overrides: {}, features: [] },
    { version: 2, overrides: {}, features: { launcher: '../escape' } }
  ]) {
    const badFile = makeFile();
    badFile.text.set(overridesUri, JSON.stringify(bad));
    await assert.rejects(choices.loadFeatureOverrides(badFile), /配置/);
    await assert.rejects(choices.loadResourceOverrideDocument(badFile), /配置/);
  }
  console.log('resource feature override tests passed');
}

main().finally(() => fs.rmSync(temporary, { recursive: true, force: true })).catch(error => {
  console.error(error);
  process.exitCode = 1;
});
