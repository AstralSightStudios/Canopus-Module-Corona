/* Host tests for the installed replacement catalog and per-file choices. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const temporary = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'resource-hook-overrides-')));
const overridesUri = 'internal://files/resource-overrides.json';
const filesUri = 'internal://files/resource-files.json';
const themeRoot = 'internal://files/themes/';

function manifest(themeId, name, mappings) {
  return JSON.stringify({
    format: 'canopus-resource-pack', formatVersion: 1, themeId, name, mappings
  });
}

function makeFile() {
  const text = new Map();
  return {
    text,
    async readOptionalText(uri) { return text.has(uri) ? text.get(uri) : null; },
    async writeText(uri, value) { text.set(uri, value); },
    async readFileInfo() { throw new Error('unexpected file enumeration'); },
    async listDirectory() { throw new Error('unexpected file enumeration'); },
    async readArrayBuffer() { throw new Error('unused'); },
    async writeArrayBuffer() { throw new Error('unused'); },
    async makeDirectory() { throw new Error('unused'); },
    async deleteFile() { throw new Error('unused'); },
    async removeDirectory() { throw new Error('unused'); },
    isFileNotFound() { return false; }
  };
}

async function main() {
  execFileSync(path.join(root, 'manager/node_modules/.bin/tsc'), [
    path.join(root, 'manager/src/ts/resource-overrides.ts'),
    '--outDir', temporary, '--module', 'commonjs', '--target', 'es2018',
    '--lib', 'es2018,dom', '--skipLibCheck'
  ], { stdio: 'inherit' });
  const overridesModule = require(path.join(temporary, 'resource-overrides.js'));
  const file = makeFile();
  const installed = ['base', 'dark', 'inactive'];
  file.text.set(`${themeRoot}base/canora.json`, manifest('base', 'Base', [
    { source: '/resource/', destination: 'assets/' },
    { source: '/resource/a.bin', destination: 'special.bin' },
    { source: '/resource/missing/', destination: 'more-specific/' }
  ]));
  file.text.set(`${themeRoot}dark/canora.json`, manifest('dark', 'Dark', [
    { source: '/resource/', destination: 'files/' }
  ]));
  file.text.set(`${themeRoot}inactive/canora.json`, manifest('inactive', 'Inactive', [
    { source: '/resource/', destination: 'other/' }
  ]));
  file.text.set(filesUri, JSON.stringify({ version: 1, themes: {
    base: [
      { relativePath: 'assets/a.bin', sizeBytes: 1 },
      { relativePath: 'assets/b.bin', sizeBytes: 1 },
      { relativePath: 'assets/missing/a.bin', sizeBytes: 1 },
      { relativePath: 'special.bin', sizeBytes: 1 },
      { relativePath: 'more-specific/other.bin', sizeBytes: 1 }
    ],
    dark: [
      { relativePath: 'files/a.bin', sizeBytes: 1 },
      { relativePath: 'files/b.bin', sizeBytes: 1 }
    ],
    inactive: [{ relativePath: 'other/a.bin', sizeBytes: 1 }]
  } }));

  const catalog = await overridesModule.loadRegisteredResourcePaths(installed, file);
  const byPath = Object.fromEntries(catalog.map(item => [item.sourcePath, item]));
  assert.deepEqual(byPath['/resource/a.bin'].themes.map(theme => theme.themeId),
    ['base', 'dark', 'inactive']);
  assert.equal(byPath['/resource/a.bin'].themes.filter(theme => theme.themeId === 'base').length, 1,
    'overlapping rules from one pack register a concrete path only once');
  assert.equal(byPath['/resource/a.bin'].themes.find(theme => theme.themeId === 'base').previewUri,
    `${themeRoot}base/special.bin`, 'preview follows the most-specific package mapping');
  assert.equal(byPath['/resource/a.bin'].themes.find(theme => theme.themeId === 'dark').previewUri,
    `${themeRoot}dark/files/a.bin`);
  assert.deepEqual(byPath['/resource/b.bin'].themes.map(theme => theme.themeId), ['base', 'dark']);
  assert.deepEqual(catalog.map(item => item.sourcePath), [
    '/resource/a.bin', '/resource/b.bin', '/resource/missing/other.bin'
  ]);
  assert.equal(byPath['/resource/missing/a.bin'], undefined,
    'a broader package mapping does not register files masked by a more-specific rule');

  const defaults = await overridesModule.loadResourceOverrides(file);
  assert.deepEqual({ ...defaults }, {});
  await overridesModule.saveResourceOverride('/resource/a.bin', 'base', file);
  await overridesModule.saveResourceOverride('/resource/b.bin', '@system', file);
  await overridesModule.saveResourceOverride('/resource/orphan.bin', 'inactive', file);
  const saved = JSON.parse(file.text.get(overridesUri));
  assert.equal(saved.version, 1);
  assert.equal(saved.overrides['/resource/a.bin'], 'base');
  assert.equal(saved.overrides['/resource/b.bin'], '@system');

  const afterRemoval = catalog.map(item => ({
    sourcePath: item.sourcePath,
    themes: item.themes.filter(theme => theme.themeId !== 'base' && theme.themeId !== 'inactive')
  })).filter(item => item.themes.length);
  const reconciled = await overridesModule.reconcileResourceOverrides(afterRemoval, file);
  assert.equal(reconciled['/resource/a.bin'], '@default',
    'a removed selected pack falls back to normal resource order');
  assert.equal(reconciled['/resource/b.bin'], '@system');
  assert.equal(reconciled['/resource/orphan.bin'], undefined,
    'settings for paths no longer registered by any pack are removed');

  await overridesModule.saveResourceOverride('/resource/b.bin', 'dark', file);
  await overridesModule.removeThemeFromResourceOverrides('dark', file);
  const afterDarkRemoval = await overridesModule.loadResourceOverrides(file);
  assert.equal(afterDarkRemoval['/resource/a.bin'], '@default');
  assert.equal(afterDarkRemoval['/resource/b.bin'], '@default',
    'deleting an explicitly selected pack clears its choice to Default');

  {
    const legacyFile = makeFile();
    const legacyManifest = manifest('legacy', 'Legacy', [
      { source: '/resource/icons/', destination: 'icons/' }
    ]);
    legacyFile.text.set(`${themeRoot}legacy/canora.json`, legacyManifest);
    const staleInventory = JSON.stringify({ version: 1, themes: {
      legacy: [{ relativePath: 'canora.json', sizeBytes: Buffer.byteLength(legacyManifest) }]
    } });
    legacyFile.text.set(filesUri, staleInventory);
    const listings = {
      [`${themeRoot}legacy/`]: [
        { uri: `${themeRoot}legacy/canora.json`, length: Buffer.byteLength(legacyManifest) },
        { uri: `${themeRoot}legacy/icons/`, length: 0 }
      ],
      [`${themeRoot}legacy/icons/`]: [
        { uri: `${themeRoot}legacy/icons/icon.bin`, length: 123 }
      ]
    };
    legacyFile.listDirectory = async uri => listings[uri] || [];
    legacyFile.readFileInfo = async uri => ({
      length: uri.endsWith('/') ? 0 : Number.NaN,
      type: uri.endsWith('/') ? 'dir' : 'file'
    });
    const legacyCatalog = await overridesModule.loadRegisteredResourcePaths(['legacy'], legacyFile);
    assert.deepEqual(legacyCatalog.map(item => item.sourcePath), ['/resource/icons/icon.bin'],
      'a stale but nonempty inventory is refreshed from the installed directory tree');
    assert.equal(legacyCatalog[0].themes[0].previewUri,
      `${themeRoot}legacy/icons/icon.bin`, 'LVGL preview URI uses the package file path');
    assert.equal(legacyFile.text.get(filesUri), staleInventory,
      'the catalog uses the fresh path snapshot without requiring an index rewrite');
  }

  {
    const recursiveFile = makeFile();
    const recursiveManifest = manifest('nested', 'Nested', [
      { source: '/resource/app/settings/', destination: 'app/settings/' }
    ]);
    recursiveFile.text.set(`${themeRoot}nested/canora.json`, recursiveManifest);
    recursiveFile.readFileInfo = async (uri, recursive) => {
      assert.equal(recursive, true, 'inventory fallback requests recursive Vela file metadata');
      const rootUri = `${themeRoot}nested/`;
      return {
        uri, length: 0, type: 'dir', subFiles: [
          { uri: `${rootUri}canora.json`, length: Buffer.byteLength(recursiveManifest), type: 'file' },
          { uri: `${rootUri}app/`, length: 0, type: 'dir', subFiles: [
            { uri: `${rootUri}app/settings/`, length: 0, type: 'dir', subFiles: [
              { uri: `${rootUri}app/settings/icon.bin`, length: 123, type: 'file' }
            ] }
          ] }
        ]
      };
    };
    recursiveFile.listDirectory = async () => { throw new Error('recursive get should provide the tree'); };
    const recursiveCatalog = await overridesModule.loadRegisteredResourcePaths(['nested'], recursiveFile);
    assert.deepEqual(recursiveCatalog.map(item => item.sourcePath), [
      '/resource/app/settings/icon.bin'
    ]);
  }

  {
    const longFile = makeFile();
    const longSource = `/${'s'.repeat(200)}/`;
    const relativePath = `x/${'f'.repeat(60)}.bin`;
    longFile.text.set(`${themeRoot}long/canora.json`, manifest('long', 'Long', [
      { source: longSource, destination: 'x/' }
    ]));
    longFile.text.set(filesUri, JSON.stringify({ version: 1, themes: {
      long: [{ relativePath, sizeBytes: 1 }]
    } }));
    assert.deepEqual(await overridesModule.loadRegisteredResourcePaths(['long'], longFile), [],
      'an unrepresentable expanded path is skipped without aborting the whole catalog');
  }

  {
    const invalidFile = makeFile();
    const original = JSON.stringify({ version: 1, overrides: { '/resource/a.bin': 'broken' } });
    invalidFile.text.set(`${themeRoot}broken/canora.json`, '{bad manifest');
    invalidFile.text.set(overridesUri, original);
    await assert.rejects(overridesModule.loadRegisteredResourcePaths(['broken'], invalidFile), /manifest 无效/);
    assert.equal(invalidFile.text.get(overridesUri), original,
      'a damaged but still-installed pack must not silently erase saved user choices');
  }

  console.log('Per-file replacement catalog, choice persistence and removed-pack cleanup passed.');
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
