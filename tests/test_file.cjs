/* Test the typed file functions by mocking only the native JS boundary. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const temporary = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'resource-hook-file-')));

async function main() {
  execFileSync(path.join(root, 'manager/node_modules/.bin/tsc'), [
    path.join(root, 'manager/src/ts/file.ts'), '--outDir', temporary,
    '--module', 'commonjs', '--target', 'es2018', '--lib', 'es2018,dom', '--skipLibCheck', '--allowJs'
  ], { stdio: 'inherit' });
  const api = {}, boundary = path.join(temporary, 'system-file.js');
  require.cache[boundary] = { id: boundary, filename: boundary, loaded: true,
    exports: { __esModule: true, default: api } };
  const file = require(path.join(temporary, 'file.js'));
  const uri = 'internal://files/test.bin', buffer = new Uint8Array([0, 1, 255]);
  let options;
  api.readText = function (o) {
    assert.equal(this, api); options = o;
    queueMicrotask(() => o.success({ text: 'hello' }));
  };
  assert.equal(await file.readText(uri), 'hello');
  assert.equal(options.uri, uri); assert.equal(options.encoding, 'UTF-8');
  api.writeText = o => { options = o; o.success(); };
  assert.equal(await file.writeText(uri, 'content'), undefined);
  assert.equal(options.text, 'content'); assert.equal(options.encoding, 'UTF-8');
  // Empty text is forwarded, not converted into hidden application data.
  api.writeText = o => { assert.equal(o.text, ''); o.fail('invalid text', 202); };
  await assert.rejects(file.writeText(uri, ''), error => error.code === 202);

  api.readArrayBuffer = o => { options = o; o.success({ buffer }); };
  assert.equal(await file.readArrayBuffer(uri), buffer);
  assert(!Object.hasOwn(options, 'position') && !Object.hasOwn(options, 'length'));
  await file.readArrayBuffer(uri, 0, 0);
  assert.equal(options.position, 0); assert.equal(options.length, 0);
  await file.readArrayBuffer(uri, 32768, 32768);
  assert.equal(options.position, 32768); assert.equal(options.length, 32768);
  api.writeArrayBuffer = o => { options = o; o.success(); };
  assert.equal(await file.writeArrayBuffer(uri, buffer), undefined);
  assert.equal(options.buffer, buffer); assert(!Object.hasOwn(options, 'position'));
  await file.writeArrayBuffer(uri, buffer, 0); assert.equal(options.position, 0);
  await file.writeArrayBuffer(uri, buffer, 32768); assert.equal(options.position, 32768);

  api.get = o => { options = o; o.success({ length: 3, type: 'file', ignored: 1 }); };
  assert.deepEqual(await file.readFileInfo(uri), { length: 3, type: 'file' });
  api.get = o => o.success({ length: 0 });
  assert.deepEqual(await file.readFileInfo(uri), { length: 0, type: undefined });
  api.mkdir = o => { options = o; o.success(); };
  assert.equal(await file.makeDirectory(uri), undefined); assert.equal(options.recursive, true);
  await file.makeDirectory(uri, false); assert.equal(options.recursive, false);
  api.delete = o => { options = o; o.success(); };
  assert.equal(await file.deleteFile(uri), undefined); assert.equal(options.uri, uri);

  const methods = [
    ['readText', () => file.readText(uri)],
    ['writeText', () => file.writeText(uri, 'text')],
    ['readArrayBuffer', () => file.readArrayBuffer(uri)],
    ['writeArrayBuffer', () => file.writeArrayBuffer(uri, buffer)],
    ['get', () => file.readFileInfo(uri)],
    ['mkdir', () => file.makeDirectory(uri)],
    ['delete', () => file.deleteFile(uri)]
  ];
  for (const [method, run] of methods) {
    const data = { reason: 'denied' };
    api[method] = o => queueMicrotask(() => o.fail(data, 300));
    await assert.rejects(run(), error => {
      assert(error instanceof Error); assert.equal(error.code, 300);
      assert.equal(error.data, data); assert.equal(error.uri, uri);
      assert(error.operation); assert(error.message.includes(uri)); return true;
    });
    const synchronous = new Error(`native ${method} threw`);
    api[method] = () => { throw synchronous; };
    await assert.rejects(run(), error => error === synchronous);
  }
  for (const [method, run] of [
    ['readText', () => file.readOptionalText(uri)],
    ['readArrayBuffer', () => file.readOptionalArrayBuffer(uri)]
  ]) {
    api[method] = o => o.fail('missing', 301); assert.equal(await run(), null);
    for (const code of [202, 300]) {
      api[method] = o => o.fail('error', code);
      await assert.rejects(run(), error => error.code === code);
    }
    api[method] = () => { throw null; };
    await run().then(() => assert.fail('must reject'), error => assert.equal(error, null));
  }
  api.readText = o => o.success({ text: '' });
  assert.equal(await file.readOptionalText(uri), '');
  api.readArrayBuffer = o => o.success({ buffer });
  assert.equal(await file.readOptionalArrayBuffer(uri), buffer);
  assert(file.isFileNotFound({ code: 301 }));
  for (const value of [null, undefined, 301, '301', { code: 300 }, { code: '301' }])
    assert.equal(file.isFileNotFound(value), false);
  console.log('Typed file text/binary/range/metadata, optional reads and callback/synchronous error tests passed.');
}
main().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => {
  fs.rmSync(temporary, { recursive: true, force: true });
});
