/* Canonical and legacy installed manifest reads, including the actual metadata pages. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'resource-hook-manifest-names-'));
const themeRoot = 'internal://files/themes/old/';
const canonicalUri = `${themeRoot}corona.json`;
const legacyUri = `${themeRoot}canora.json`;
const manifest = JSON.stringify({ format: 'canopus-resource-pack', formatVersion: 1,
  themeId: 'old', name: 'Old installed pack', author: 'Author', mappings: [] });

function page(name, dependencies) {
  const source = fs.readFileSync(path.join(root, `manager/src/pages/${name}/${name}.ux`), 'utf8');
  const script = /<script>([\s\S]*?)<\/script>/.exec(source)[1]
    .replace(/^import[\s\S]*?from "[^"]+"\r?\n/gm, '')
    .replace('export default', 'globalThis.page =');
  const context = { ...dependencies };
  vm.runInNewContext(script, context, { filename: `${name}.ux` });
  return { ...context.page, ...context.page.private, themeId: 'old', visible: true };
}

async function main() {
  try {
    execFileSync(path.join(root, 'manager/node_modules/.bin/tsc'), [
      path.join(root, 'manager/src/ts/resource-pack.ts'), '--outDir', temporary,
      '--module', 'commonjs', '--target', 'es2018', '--lib', 'es2018,dom', '--skipLibCheck'
    ], { stdio: 'inherit' });
    const { readResourcePackManifest: read, parseResourcePackManifest: parse } =
      require(path.join(temporary, 'resource-pack.js'));
    const text = new Map();
    const calls = [];
    const file = { async readOptionalText(uri) { calls.push(uri); return text.get(uri) ?? null; } };
    assert.equal(await read(themeRoot, 'old', file), null);
    text.set(canonicalUri, manifest);
    assert.equal((await read(themeRoot, 'old', file)).name, 'Old installed pack');
    text.delete(canonicalUri);
    text.set(legacyUri, manifest);
    assert.equal((await read(themeRoot, 'old', file)).name, 'Old installed pack');
    text.set(canonicalUri, manifest);
    await assert.rejects(read(themeRoot, 'old', file), /同时包含/);
    text.set(canonicalUri, '{malformed canonical');
    await assert.rejects(read(themeRoot, 'old', file), /同时包含/,
      'valid legacy metadata must not mask malformed canonical metadata');
    text.delete(legacyUri);
    await assert.rejects(read(themeRoot, 'old', file), /corona.json JSON 格式无效/);
    text.set(legacyUri, manifest);
    await assert.rejects(read(themeRoot, 'old', {
      async readOptionalText(uri) {
        if (uri === canonicalUri) throw new Error('canonical read failed');
        assert.fail('a canonical I/O failure must not fall back');
      }
    }), /canonical read failed/);

    for (const filename of ['corona.json', 'canora.json']) {
      for (const destination of [filename, `${filename}/`, `${filename}/asset.bin`]) {
        assert.throws(() => parse(JSON.stringify({ ...JSON.parse(manifest), mappings: [
          { source: destination.endsWith('/') ? '/resource/' : '/resource/file', destination }
        ] })), /不能使用/, destination);
      }
      text.clear();
      text.set(`${themeRoot}${filename}`, manifest);
      const dependencies = { file, readResourcePackManifest: read,
        readInstalledThemeIds: async () => ['old'],
        loadResourceOrder: async () => ['old', '@system'], SYSTEM_STYLE_ID: '@system' };
      const detail = page('resource-detail', dependencies);
      await detail.loadManifest();
      assert.equal(detail.loaded, true, `${filename} details remain readable`);
      assert.equal(detail.name, 'Old installed pack');
      const resources = page('resources', dependencies);
      await resources.loadThemes();
      assert.equal(resources.rows[0].name, 'Old installed pack', `${filename} list metadata remains readable`);

      text.set(canonicalUri, '{invalid canonical');
      await detail.loadManifest();
      assert.equal(detail.loaded, false);
      await resources.loadThemes();
      assert.equal(resources.rows[0].name, 'old', 'invalid metadata stays visible without legacy fallback');
      assert.equal(resources.rows[0].metaLine, 'corona.json 不可用');
    }
    assert(calls.includes(canonicalUri) && calls.includes(legacyUri));
    console.log('Manifest filename compatibility, ambiguity, reserved paths and installed metadata pages passed.');
  } finally {
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
