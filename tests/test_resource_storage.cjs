/* Host tests for deleting installed resource packs. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const temporary = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'resource-hook-storage-')));
const indexUri = 'internal://files/interconnect-themes.json';
const mappingsUri = 'internal://files/mappings.tsv';
const themeDestinationPrefix = 'themes/';
const orderUri = 'internal://files/resource-order.json';
const fileIndexUri = 'internal://files/resource-files.json';
const overridesUri = 'internal://files/resource-overrides.json';
const generationsUri = 'internal://files/resource-active-generations.json';

function makeStorage(overrides = {}) {
  const text = new Map([
    [indexUri, JSON.stringify(['dark', 'light', 'dark'])],
    [mappingsUri, ''],
    [orderUri, JSON.stringify({ version: 1, order: ['dark', 'light', '@system'] })],
    [fileIndexUri, JSON.stringify({ version: 1, themes: {
      dark: [{ relativePath: 'dark.bin', sizeBytes: 1 }],
      light: [{ relativePath: 'light.bin', sizeBytes: 1 }]
    } })],
    [overridesUri, JSON.stringify({ version: 1, overrides: {
      '/resource/dark.bin': 'dark', '/resource/light.bin': '@system'
    } })]
  ]);
  const removedDirectories = [];
  const writes = [];
  const api = {
    async readOptionalText(uri) {
      return text.has(uri) ? text.get(uri) : null;
    },
    async writeText(uri, value) {
      writes.push(uri);
      text.set(uri, value);
    },
    async removeDirectory(uri) {
      removedDirectories.push(uri);
    },
    isFileNotFound(error) {
      return error && error.code === 301;
    },
    ...overrides
  };
  return { api, text, removedDirectories, writes };
}

async function main() {
  execFileSync(path.join(root, 'manager/node_modules/.bin/tsc'), [
    path.join(root, 'manager/src/ts/resource-storage.ts'),
    path.join(root, 'manager/src/ts/resource-overrides.ts'),
    path.join(root, 'manager/src/ts/resource-order.ts'),
    path.join(root, 'manager/src/ts/resource-pack.ts'), '--outDir', temporary,
    '--module', 'commonjs', '--target', 'es2018', '--lib', 'es2018,dom', '--skipLibCheck'
  ], { stdio: 'inherit' });
  const { removeInstalledTheme } = require(path.join(temporary, 'resource-storage.js'));

  {
    const storage = makeStorage();
    await removeInstalledTheme('dark', storage.api);
    assert.deepEqual(storage.removedDirectories, ['internal://files/themes/dark/']);
    assert.equal(storage.text.get(indexUri), '["light"]');
    assert.deepEqual(JSON.parse(storage.text.get(orderUri)),
      { version: 1, order: ['light', '@system'] });
    assert.deepEqual(JSON.parse(storage.text.get(fileIndexUri)),
      { version: 1, themes: { light: [{ relativePath: 'light.bin', sizeBytes: 1 }] } });
    assert.deepEqual(JSON.parse(storage.text.get(overridesUri)), { version: 1, overrides: {
      '/resource/dark.bin': '@default', '/resource/light.bin': '@system'
    } });
  }

  {
    const storage = makeStorage({
      async readOptionalText(uri) {
        if (uri === mappingsUri)
          return `#/ generated mapping\n/resource/app/\t${themeDestinationPrefix}dark/app/`;
        return uri === indexUri ? JSON.stringify(['dark', 'light']) : null;
      }
    });
    await assert.rejects(removeInstalledTheme('dark', storage.api), error =>
      error.resourceStorageReason === 'active-theme');
    assert.deepEqual(storage.removedDirectories, []);
    assert.equal(storage.text.get(indexUri), JSON.stringify(['dark', 'light', 'dark']));
  }

  // Publishing a switch-away TSV does not release the prior direct pack until native ack.
  for (const mappings of [
    '/resource/light.bin\tthemes/light/light.bin\n', '', null
  ]) {
    const storage = makeStorage();
    if (mappings === null) storage.text.delete(mappingsUri);
    else storage.text.set(mappingsUri, mappings);
    storage.text.set(generationsUri, JSON.stringify({ version: 1, generations: [],
      protectedThemes: mappings ? ['dark', 'light'] : ['dark'] }));
    const before = new Map(storage.text);
    await assert.rejects(removeInstalledTheme('dark', storage.api), error =>
      error.resourceStorageReason === 'active-theme');
    assert.deepEqual(storage.text, before, 'protected deletion must not rewrite any metadata');
    assert.deepEqual(storage.removedDirectories, []);
    assert.deepEqual(storage.writes, []);

    // Matching acknowledged cleanup narrows protection to the newly active plan.
    const acknowledgedRegistry = JSON.stringify({ version: 1, generations: [],
      protectedThemes: mappings ? ['light'] : [] });
    storage.text.set(generationsUri, acknowledgedRegistry);
    await removeInstalledTheme('dark', storage.api);
    assert.deepEqual(storage.removedDirectories, ['internal://files/themes/dark/']);
    assert.equal(storage.text.get(indexUri), '["light"]');
    assert.deepEqual(JSON.parse(storage.text.get(orderUri)),
      { version: 1, order: ['light', '@system'] });
    assert.deepEqual(JSON.parse(storage.text.get(fileIndexUri)),
      { version: 1, themes: { light: [{ relativePath: 'light.bin', sizeBytes: 1 }] } });
    assert.deepEqual(JSON.parse(storage.text.get(overridesUri)), { version: 1, overrides: {
      '/resource/dark.bin': '@default', '/resource/light.bin': '@system'
    } });
    assert.equal(storage.text.get(generationsUri), acknowledgedRegistry);
  }

  const malformedRegistries = [
    '{invalid', 'null',
    JSON.stringify({ version: 2, generations: [], protectedThemes: ['dark'] }),
    JSON.stringify({ version: 1, generations: 'bad', protectedThemes: ['dark'] }),
    ...[null, 'dark', [42], [''], ['Dark'], ['too_long_theme'], ['../dark'], ['dark', 'dark']]
      .map(protectedThemes => JSON.stringify({ version: 1, generations: [], protectedThemes }))
  ];
  for (const registry of malformedRegistries) {
    const storage = makeStorage();
    storage.text.set(generationsUri, registry);
    const before = new Map(storage.text);
    await assert.rejects(removeInstalledTheme('dark', storage.api));
    assert.deepEqual(storage.text, before, 'malformed protection must fail closed before writes');
    assert.deepEqual(storage.removedDirectories, []);
    assert.deepEqual(storage.writes, []);
  }

  // Older v1 registries without protection metadata remain compatible.
  {
    const storage = makeStorage();
    storage.text.set(generationsUri, JSON.stringify({ version: 1, generations: [] }));
    await removeInstalledTheme('dark', storage.api);
    assert.deepEqual(storage.removedDirectories, ['internal://files/themes/dark/']);
    assert.equal(storage.text.get(indexUri), '["light"]');
  }

  {
    const storage = makeStorage({
      async readOptionalText(uri) {
        return uri === indexUri ? '{invalid' : '';
      }
    });
    await assert.rejects(removeInstalledTheme('dark', storage.api), error =>
      error.resourceStorageReason === 'invalid-index');
    assert.deepEqual(storage.removedDirectories, []);
  }

  {
    const storage = makeStorage({
      async removeDirectory(uri) {
        storage.removedDirectories.push(uri);
        const error = new Error('missing');
        error.code = 301;
        throw error;
      }
    });
    await removeInstalledTheme('dark', storage.api);
    assert.deepEqual(storage.removedDirectories, ['internal://files/themes/dark/']);
    assert.equal(storage.text.get(indexUri), '["light"]');
  }

  await assert.rejects(removeInstalledTheme('../bad', makeStorage().api), error =>
    error.resourceStorageReason === 'invalid-theme-id');
  console.log('Installed resource-pack removal, TSV/durable protection, fail-closed metadata and index updates passed.');
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
