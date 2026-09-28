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
const nativeThemePrefix = '/data/quickapp/files/ng.lst.corona/themes/';
const orderUri = 'internal://files/resource-order.json';
const fileIndexUri = 'internal://files/resource-files.json';
const overridesUri = 'internal://files/resource-overrides.json';

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
  const api = {
    async readOptionalText(uri) {
      return text.has(uri) ? text.get(uri) : null;
    },
    async writeText(uri, value) {
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
  return { api, text, removedDirectories };
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
          return `#/ generated mapping\n/resource/app/\t${nativeThemePrefix}dark/app/`;
        return uri === indexUri ? JSON.stringify(['dark', 'light']) : null;
      }
    });
    await assert.rejects(removeInstalledTheme('dark', storage.api), error =>
      error.resourceStorageReason === 'active-theme');
    assert.deepEqual(storage.removedDirectories, []);
    assert.equal(storage.text.get(indexUri), JSON.stringify(['dark', 'light', 'dark']));
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
  console.log('Installed resource-pack removal, active-theme protection and index updates passed.');
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
