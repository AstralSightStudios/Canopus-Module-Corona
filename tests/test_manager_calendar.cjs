/* Host tests for corona.json calendar declarations, CFG publication and package retention. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'resource-hook-calendar-manager-'));
const themeRoot = 'internal://files/themes/';
const cfgUri = 'internal://files/calendar.cfg';
const mappingsUri = 'internal://files/mappings.tsv';
const registryUri = 'internal://files/resource-active-generations.json';
const textItem = { type: 'text', x: -3, y: 4, align: 'top_mid', size: 0 };
const counts = { month: 12, day: 31, weekday: 7 };
const base = id => ({ format: 'canopus-resource-pack', formatVersion: 1,
  themeId: id, name: id, mappings: [] });

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
      if (recursive) return { type: 'dir', length: 0, subFiles: [...assets].filter(
        ([key]) => key.startsWith(uri)).map(([key, length]) => ({ uri: key, type: 'file', length })) };
      throw Object.assign(new Error(`missing ${uri}`), { code: 301 });
    },
    async listDirectory() { throw new Error('Unexpected list fallback'); },
    async makeDirectory(uri) { calls.push(['mkdir', uri]); },
    async copyFile(from, to) { assert(assets.has(from)); assets.set(to, assets.get(from)); },
    async removeDirectory(uri) { calls.push(['remove', uri]); },
    isFileNotFound(error) { return error.code === 301; }
  };
}

async function main() {
  execFileSync(path.join(root, 'manager/node_modules/.bin/tsc'), [
    path.join(root, 'manager/src/ts/resource-storage.ts'),
    '--outDir', temporary, '--module', 'commonjs', '--target', 'es2018',
    '--lib', 'es2018,dom', '--skipLibCheck'
  ], { stdio: 'inherit' });
  const pack = require(path.join(temporary, 'resource-pack.js'));
  const activation = require(path.join(temporary, 'resource-activation.js'));
  const catalog = require(path.join(temporary, 'resource-overrides.js'));
  const storage = require(path.join(temporary, 'resource-storage.js'));
  const paths = require(path.join(temporary, 'resource-path.js'));
  const parse = (calendar, id = 'test') => pack.parseResourcePackManifest(JSON.stringify({
    ...base(id), ...(calendar === undefined ? {} : { calendar })
  }));
  const theme = (manifest, files = [], active = true) => ({
    themeId: manifest.themeId, manifest, files, active
  });
  const serialize = activation.serializeCalendar;
  const snapshot = (...themes) => ({ installedThemeIds: themes.map(t => t.themeId),
    themes: new Map(themes.map(t => [t.themeId, t])) });
  const image = (name, file = 'date.bin') => ({ ...textItem, type: 'image',
    context: Array(counts[name]).fill(file) });

  assert.equal(parse().calendar, undefined);
  assert.deepEqual(parse({}).calendar, {});
  assert.equal(serialize([]), 'version=1\n');
  assert.equal(serialize([theme(parse())]), 'version=1\n');
  assert.equal(serialize([theme(parse({}))]), 'version=1\n');
  const weekdays = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];
  const declaration = { month: { type: 'none' }, day: { ...textItem, color: '#a1b2c3' },
    weekday: { ...textItem, size: 72, align: 'center', context: weekdays } };
  const nativeBackground = pack.parseResourcePackManifest(JSON.stringify({ ...base('test'), calendar: {},
    mappings: [{ source: '/resource/app/perpetual_calendar/launcher_icon.bin', destination: 'bg.bin' }] }));
  assert.equal(pack.serializeResourcePackMappings(nativeBackground),
    '/resource/app/perpetual_calendar/launcher_icon.bin\tthemes/test/bg.bin\n');
  const parsed = parse(declaration);
  assert.deepEqual(pack.parseResourcePackManifest(JSON.stringify(parsed)), parsed);
  assert.equal(serialize([theme(parsed)]), 'version=1\n' +
    'month\tnone\ttop_left\t0\t0\t0\tnative\t0\n' +
    'day\ttext\ttop_mid\t-3\t4\t0\tA1B2C3\t0\n' +
    'weekday\ttext\tcenter\t-3\t4\t72\tnative\t7\nMon\nTue\nWed\nThu\nFri\nSat\nSun\n');
  assert.match(serialize([theme(parse({ month: { ...textItem, context: Array(12).fill('') } }))]),
    /\tnative\t12\n\n{12}$/);
  assert.equal(serialize([theme(parse({ day: { ...textItem, context: [] } }))]),
    serialize([theme(parse({ day: textItem }))]));
  for (const align of ['top_left', 'top_mid', 'top_right', 'center'])
    assert.equal(parse({ day: { ...textItem, align } }).calendar.day.align, align);
  for (const x of [-112, 112]) parse({ day: { ...textItem, x, y: x } });
  for (const size of [0, 8, 72]) parse({ day: { ...textItem, size } });
  for (const invalid of [null, [], '', { background: 'x.bin' }, { week: textItem },
    { day: null }, { day: { type: 'none', x: 0 } }, { day: { type: 'unknown' } },
    { day: { ...textItem, extra: true } }]) assert.throws(() => parse(invalid));
  for (const [field, values] of Object.entries({
    x: [null, '0', 1.5, -113, 113], y: [null, '0', 1.5, -113, 113],
    align: [null, '', 'bottom_mid', 'CENTER'], size: [null, '8', 7, 73, -1, 8.5],
    color: [null, '', 'native', 'FFFFFF', '#fff', '#GG0000', '#000000\n'],
    context: [null, '', {}, [1], Array(6).fill('x'), Array(8).fill('x')]
  })) for (const value of values)
    assert.throws(() => parse({ weekday: { ...textItem, [field]: value } }), `${field}=${value}`);
  // Partial overrides inherit item-specific native placement, alignment and font.
  for (const name of pack.CALENDAR_ITEM_NAMES) {
    const native = { type: 'text', x: 0, y: name === 'day' ? 34 : 17, align: 'top_mid', size: 0 };
    assert.deepEqual(parse({ [name]: { type: 'text' } }).calendar[name], native);
    for (const field of ['x', 'y', 'align', 'size']) {
      const item = { ...textItem }; delete item[field];
      assert.equal(parse({ [name]: item }).calendar[name][field], native[field]);
    }
    const context = Array(counts[name]).fill('date.bin');
    const imageOnly = parse({ [name]: { type: 'image', context } });
    assert.deepEqual(imageOnly.calendar[name], { ...native, type: 'image', context });
    assert.deepEqual(pack.parseResourcePackManifest(JSON.stringify(imageOnly)), imageOnly);
    assert.equal(serialize([theme(imageOnly)]), `version=1\n${name}\timage\ttop_mid\t0\t${native.y}\t0\tnative\t${counts[name]}\n` +
      'themes/test/date.bin\n'.repeat(counts[name]));
    const positioned = parse({ [name]: { type: 'image', x: 11, y: -9, context } });
    assert.match(serialize([theme(positioned)]), /\timage\ttop_mid\t11\t-9\t0\tnative\t/);
    assert.deepEqual(parse({ [name]: { type: 'image', size: 0, context } }).calendar[name],
      imageOnly.calendar[name]);
    for (const properties of [{ size: 8 }, { size: 72 }, { color: '#3482FF' }, { color: null }])
      assert.throws(() => parse({ [name]: { type: 'image', context, ...properties } }));
  }
  const colorOnly = parse({ day: { type: 'text', color: '#fff000' } });
  assert.deepEqual(colorOnly.calendar.day, { type: 'text', x: 0, y: 34, align: 'top_mid', size: 0, color: '#FFF000' });
  assert.equal(serialize([theme(colorOnly)]), 'version=1\nday\ttext\ttop_mid\t0\t34\t0\tFFF000\t0\n');
  assert.deepEqual(pack.parseResourcePackManifest(JSON.stringify(colorOnly)), colorOnly);
  assert.equal(serialize([theme(parse({ weekday: { type: 'text' } }))]),
    'version=1\nweekday\ttext\ttop_mid\t0\t17\t0\tnative\t0\n', 'weekday native color is not hard-coded');
  for (const name of pack.CALENDAR_ITEM_NAMES) {
    for (const entry of ['a'.repeat(64), '界'.repeat(22), '\t', '\n', '\r', '\0', '\x01', '\x1f', '\x7f', '\x80', '\x85', '\x9f', '\ud800', '\udfff', '\ud800a', 'a\udfff'])
      assert.throws(() => parse({ [name]: { ...textItem, context: Array(counts[name]).fill(entry) } }));
    for (const entry of ['a'.repeat(63), '界'.repeat(21), '😀'.repeat(15) + 'abc', ''])
      parse({ [name]: { ...textItem, context: Array(counts[name]).fill(entry) } });
    for (const context of [undefined, [], Array(counts[name] - 1).fill('x.bin')])
      assert.throws(() => parse({ [name]: { ...textItem, type: 'image', context } }));
    for (const file of ['../x.bin', '/x.bin', 'a//b.bin', 'a\\b.bin', './x.bin',
      'a:b.bin', 'dir/', 'x.png', 'x.BIN', 'x\n.bin', 'x\x85.bin', 'x\ud800.bin', 'corona.json/x.bin', 'canora.json/x.bin'])
      assert.throws(() => parse({ [name]: image(name, file) }));
    const manifest = parse({ [name]: image(name, 'assets/date.bin') });
    assert.throws(() => pack.validateResourcePackFiles(manifest, []), /calendar.*不存在/);
    pack.validateResourcePackFiles(manifest, ['assets/date.bin']);
    assert.match(serialize([theme(manifest)]), /themes\/test\/assets\/date.bin\n/);
  }
  const relativeBudget = 255 - paths.WORST_CASE_FILES_ROOT_BYTES - 'themes/test/'.length;
  parse({ month: image('month', 'a'.repeat(relativeBudget - 4) + '.bin') });
  assert.throws(() => parse({ month: image('month', 'a'.repeat(relativeBudget - 3) + '.bin') }));
  assert.throws(() => parse({ month: image('month', '界'.repeat(70) + '.bin') }));

  // Whole declaration wins, not a per-item merge. Empty declaration resets native defaults.
  const low = theme(parse({ month: image('month') }, 'low'), [{ relativePath: 'date.bin', sizeBytes: 1 }]);
  const top = theme(parse({ day: textItem }, 'top'));
  const reset = theme(parse({}, 'reset'));
  const inherited = theme(parse(undefined, 'inherit'));
  assert.equal(serialize([inherited, low]), serialize([low]));
  assert.equal(serialize([top, low]), serialize([top]));
  assert.equal(serialize([reset, low]), 'version=1\n');
  assert.equal(serialize([{ ...low, active: false }]), 'version=1\n');

  // Uniform publication: all targets emit identical calendar.cfg; runtime module handles adaptation.
  for (const target of ['xiaomi-band-11-4.100.139', 'xiaomi-band-11-4.100.155', 'xiaomi-band-10-pro-3.101.043', 'xiaomi-band-11-4.100.177']) {
    const targetFile = makeFile();
    await activation.regenerateActiveMappings(['reset', '@system'], 'target-test',
      targetFile, {}, snapshot(reset), target);
    assert.equal(targetFile.text.get(cfgUri), 'version=1\n');
  }
  const exact = makeFile();
  await activation.regenerateActiveMappings(['reset', '@system'], 'exact', exact, {}, snapshot(reset),
    'xiaomi-band-11-4.100.177');
  assert.equal(exact.text.get(cfgUri), 'version=1\n');
  const exactSnapshot = snapshot(reset);
  await activation.regenerateActiveMappings(['reset', '@system'], 'cacheexact', exact, {}, exactSnapshot,
    'xiaomi-band-11-4.100.177');
  const below = makeFile();
  await activation.regenerateActiveMappings(['@system', 'low'], 'below', below, {}, snapshot(low),
    'xiaomi-band-10-pro-3.101.043');
  assert.equal(below.text.get(cfgUri), 'version=1\n', 'inactive declarations do not assert runtime support');

  // Publish with TSV, retain old/native-resident dependencies until acknowledged cleanup.
  const file = makeFile();
  const snap = snapshot(low, top, reset, inherited);
  const first = await activation.regenerateActiveMappings(['low', '@system', 'top', 'reset', 'inherit'],
    'first', file, {}, snap);
  assert.equal(first.calendar, serialize([low]));
  assert.equal(file.text.get(cfgUri), first.calendar);
  assert.equal(file.text.get(mappingsUri), first.mappings);
  assert.deepEqual(await activation.readProtectedThemeIds(file), ['low']);
  const before = file.calls.length;
  const cached = await activation.regenerateActiveMappings(['low', '@system', 'top', 'reset', 'inherit'],
    'cached', file, {}, snap);
  assert.strictEqual(cached, first);
  assert.deepEqual(file.calls.slice(before), [['read', 'internal://files/resource-overrides.json']],
    'unchanged plan rereads feature choices but performs no publication I/O');
  await assert.rejects(storage.removeInstalledTheme('low', file), e => e.resourceStorageReason === 'active-theme');
  const second = await activation.regenerateActiveMappings(['reset', 'low', '@system', 'top', 'inherit'],
    'second', file, {}, snap);
  assert.equal(second.calendar, 'version=1\n');
  assert.equal(second.mappings, first.mappings);
  assert.deepEqual(await activation.readProtectedThemeIds(file), ['low']);
  await activation.cleanupInactiveGenerations(null, file, second.mappings);
  assert.deepEqual(await activation.readProtectedThemeIds(file), []);

  // Either a truncating calendar write or a later TSV write must preserve dependencies and evict the cache.
  for (const failUri of [cfgUri, mappingsUri]) {
    const write = file.writeText;
    file.writeText = async (uri, value) => {
      if (uri === failUri) { file.text.set(uri, value.slice(0, 12)); throw new Error('partial write'); }
      return write(uri, value);
    };
    await assert.rejects(activation.regenerateActiveMappings(['low', '@system', 'top', 'reset', 'inherit'],
      'failed', file, {}, snap), /partial write/);
    assert.equal(snap.activeMappings, undefined);
    assert.deepEqual(await activation.readProtectedThemeIds(file), ['low']);
    file.writeText = write;
    const repair = await activation.regenerateActiveMappings(['low', '@system', 'top', 'reset', 'inherit'],
      'repair', file, {}, snap);
    assert.equal(file.text.get(cfgUri), first.calendar);
    await activation.cleanupInactiveGenerations(null, file, repair.mappings);
    await activation.regenerateActiveMappings(['reset', 'low', '@system', 'top', 'inherit'],
      'reset', file, {}, snap);
    await activation.cleanupInactiveGenerations(null, file, second.mappings);
  }
  // Calendar-only disk configs protect files even without a durable generation registry.
  file.text.delete(registryUri);
  file.text.set(cfgUri, first.calendar);
  await assert.rejects(storage.removeInstalledTheme('low', file), e => e.resourceStorageReason === 'active-theme');
  file.text.set(cfgUri, serialize([theme(parse({ month: { ...textItem,
    context: Array(12).fill('themes/low/date.bin') } }))]));
  assert.deepEqual(await activation.readProtectedThemeIds(file), [], 'text is not an image dependency');

  // Activation rechecks image presence for both immutable snapshots and legacy stat-based callers.
  const absent = makeFile();
  await assert.rejects(activation.regenerateActiveMappings(['low', '@system'], 'absent', absent, {},
    snapshot({ ...low, files: [] })), /calendar.*不存在/);
  absent.text.set(`${themeRoot}low/corona.json`, JSON.stringify(low.manifest));
  await assert.rejects(activation.regenerateActiveMappings(['low', '@system'], 'absent', absent), /missing/);
  absent.assets.set(`${themeRoot}low/date.bin`, 1);
  assert.equal((await activation.regenerateActiveMappings(['low', '@system'], 'present', absent)).calendar,
    first.calendar);

  // Calendar-only packs discover disk assets without mappings or an optional inventory.
  for (const index of [undefined, { version: 1, themes: { low: [{ relativePath: 'wrong.bin', sizeBytes: 1 }] } }]) {
    const disk = makeFile();
    disk.text.set(`${themeRoot}low/corona.json`, JSON.stringify(low.manifest));
    if (index) disk.text.set('internal://files/resource-files.json', JSON.stringify(index));
    disk.assets.set(`${themeRoot}low/date.bin`, 1);
    const loaded = await catalog.loadResourcePackSnapshot(['low'], disk);
    assert.deepEqual(loaded.themes.get('low').files, [{ relativePath: 'date.bin', sizeBytes: 1 }]);
    assert.deepEqual(loaded.paths, [], 'calendar images are not replacement UI entries');
    disk.assets.clear();
    await assert.rejects(catalog.loadResourcePackSnapshot(['low'], disk), /calendar.*不存在|Unexpected list/);
  }

  // Overlay generations retain calendar fields in the lightweight cached plan and keep package images.
  const overlay = makeFile();
  const upper = theme({ ...base('upper'), mappings: [{ source: '/resource/', destination: 'assets/' }] },
    Array.from({ length: 256 }, (_, n) => ({ relativePath: `assets/${n}.bin`, sizeBytes: 1 })));
  const lower = theme({ ...low.manifest, mappings: [{ source: '/resource/', destination: 'assets/' }] },
    [{ relativePath: 'assets/hole.bin', sizeBytes: 1 }, ...low.files]);
  for (const t of [upper, lower]) for (const asset of t.files)
    overlay.assets.set(`${themeRoot}${t.themeId}/${asset.relativePath}`, 1);
  const overlaySnap = snapshot(upper, lower);
  const plan = await activation.regenerateActiveMappings(['upper', 'low', '@system'], 'overlay', overlay, {}, overlaySnap);
  assert.equal(plan.copies.length, 257);
  assert.equal(plan.generation, 'overlay');
  const lightweight = await activation.regenerateActiveMappings(['upper', 'low', '@system'], 'again', overlay, {}, overlaySnap);
  assert.equal(lightweight.copies.length, 0);
  assert.equal(lightweight.calendar, plan.calendar);
  await activation.cleanupInactiveGenerations(plan.generation, overlay, plan.mappings);
  assert.deepEqual(await activation.readProtectedThemeIds(overlay), ['low']);
  assert.deepEqual(JSON.parse(overlay.text.get(registryUri)).generations, ['overlay']);
  console.log('Manager calendar manifest, wire format, inheritance, inventory, publication failure and generation retention tests passed.');
}

main().catch(error => { console.error(error); process.exitCode = 1; })
  .finally(() => fs.rmSync(temporary, { recursive: true, force: true }));
