/* Host tests for revision-matched native results and immutable font mappings. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const temporary = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'resource-hook-manager-')));
const realTimeout = global.setTimeout;

function record(revision, result = 0, pending = 0, changed = 2, version = 6) {
  const payload = `resource-hook-reload-v1\tng.lst.corona\t${revision}\nRHRS1\t${version}\t${result}\t${pending}\t${changed}\n`;
  let hash = 2166136261;
  for (const byte of Buffer.from(payload)) hash = Math.imul(hash ^ byte, 16777619) >>> 0;
  return `${payload}${hash}\n`.padEnd(256, '\0');
}
function files() {
  const text = new Map(), sizes = new Map(), writes = [];
  return {
    text, sizes, writes,
    readText(o) { text.has(o.uri) ? o.success({ text: text.get(o.uri) }) : o.fail('missing', 301); },
    writeText(o) {
      // Match device validation, rather than silently accepting empty text.
      if (!o.text) { o.fail('invalid text', 202); return; }
      text.set(o.uri, o.text); writes.push(o.uri); o.success();
    },
    readArrayBuffer(o) {
      const buffer = new Uint8Array(o.length);
      if (!o.position) buffer[1] = 1;
      o.success({ buffer });
    },
    writeArrayBuffer(o) { sizes.set(o.uri, Math.max(sizes.get(o.uri) || 0, o.position + o.buffer.length)); o.success(); },
    get(o) { sizes.has(o.uri) ? o.success({ type: 'file', length: sizes.get(o.uri) }) : o.fail('missing', 301); },
    mkdir(o) { o.success(); },
    delete(o) { sizes.delete(o.uri); text.delete(o.uri); o.success(); }
  };
}
function loadBridge(nativeFile) {
  const boundary = path.join(temporary, 'import.js');
  require.cache[boundary] = { id: boundary, filename: boundary, loaded: true,
    exports: { __esModule: true, file: nativeFile } };
  delete require.cache[path.join(temporary, 'file.js')];
  delete require.cache[path.join(temporary, 'theme-bridge.js')];
  return require(path.join(temporary, 'theme-bridge.js'));
}
async function main() {
  execFileSync(path.join(root, 'manager/node_modules/.bin/tsc'), [
    path.join(root, 'manager/src/ts/theme-bridge.ts'), '--outDir', temporary,
    '--module', 'commonjs', '--target', 'es2018', '--lib', 'es2018,dom', '--skipLibCheck', '--allowJs'
  ], { stdio: 'inherit' });
  const file = files();
  const { ThemeBridge, parseReloadResult } = loadBridge(file);
  assert.deepEqual(parseReloadResult(record('g1'), 'g1'), { version: 6, result: 0, pending: false, changed: 2 });
  assert.equal(parseReloadResult(record('old'), 'new'), null);
  assert.equal(parseReloadResult(record('g1').slice(0, 50), 'g1'), null);
  assert.equal(parseReloadResult(record('g1').replace('RHRS1\t6\t0', 'RHRS1\t6\t1'), 'g1'), null);
  assert.equal(parseReloadResult(record('g1', 0, 0, 2, 7), 'g1'), null);
  const bridge = new ThemeBridge();
  await assert.rejects(new Promise((resolve, reject) => file.writeText({
    uri: 'internal://files/reload.result', text: '', success: resolve,
    fail: (message, code) => reject(new Error(`${code}: ${message}`))
  })), /202: invalid text/);
  const installed = await bridge.installAllFirmwareFonts();
  const mappings = file.text.get('internal://files/mappings.tsv').trim().split('\n');
  assert.equal(mappings.length, 12);
  for (const name of ['Regular', 'Medium', 'Demibold'])
    assert(mappings.some(line => line.startsWith(`/tmp/MiSans-${name}.ttf\t`)));
  assert.equal(new Set(mappings.map(line => line.split('\t')[1])).size, 1);
  assert.equal([...file.sizes.values()][0], 6285576);
  assert(file.writes.indexOf('internal://files/reload.result') < file.writes.indexOf('internal://files/reload.request'));
  assert.equal(file.text.get('internal://files/reload.result'), `pending\t${installed.revision}\n`);
  assert.equal(parseReloadResult(file.text.get('internal://files/reload.result'), installed.revision), null);
  const next = await bridge.installAllFirmwareFonts();
  assert.notEqual(installed.generation, next.generation);
  assert.equal(file.sizes.size, 2);
  await bridge.restoreFirmwareFonts();
  assert(!file.text.get('internal://files/mappings.tsv').includes('/tmp/MiSans-'));
  assert.equal(file.sizes.size, 2); // Never unlink referenced generations.

  // Failure of diagnostics must not stop the image/resource reload signal.
  const broken = files();
  const { ThemeBridge: FallbackBridge } = loadBridge(broken);
  const fallback = new FallbackBridge();
  const write = broken.writeText.bind(broken);
  broken.writeText = o => o.uri.endsWith('/reload.result') ? o.fail('invalid text', 202) : write(o);
  const revision = await fallback.requestReload();
  assert.equal(broken.text.get('internal://files/reload.request'),
    `resource-hook-reload-v1\tng.lst.corona\t${revision}\n`);
  assert.match(await fallback.waitForReload(revision), /请求已发送.*无法确认结果/);
  // A later request can recover; the diagnostic failure is not latched forever.
  broken.writeText = write;
  const recovered = await fallback.requestReload();
  broken.text.set('internal://files/reload.result', record(recovered));
  assert.match(await fallback.waitForReload(recovered), /已更新 2/);
  // Sending the actual signal remains mandatory and must still report failure.
  broken.writeText = o => o.uri.endsWith('/reload.request') ? o.fail('denied', 202) : write(o);
  await assert.rejects(fallback.requestReload(), /reload.request.*202/);
  broken.readText = o => o.fail('unreadable', 202);
  await assert.rejects(fallback.waitForReload(recovered), /请求已发送.*读取回执失败/);

  global.setTimeout = callback => { queueMicrotask(callback); return 0; };
  const uri = 'internal://files/reload.result';
  file.text.set(uri, record('good'));
  assert.match(await bridge.waitForReload('good'), /已更新 2/);
  file.text.set(uri, record('no-op', 0, 0, 0));
  assert.match(await bridge.waitForReload('no-op'), /无字体改动/);
  file.text.set(uri, record('legacy', 0, 0, 0, 5));
  assert.match(await bridge.waitForReload('legacy'), /不支持字体/);
  file.text.set(uri, record('limit', -2211, 0, 0));
  await assert.rejects(bridge.waitForReload('limit'), /32 MiB.*-2211/);
  file.text.set(uri, record('partial', -1, 0, 1));
  await assert.rejects(bridge.waitForReload('partial'), /刷新未完成/);
  file.text.set(uri, record('busy', 1, 1, 0));
  await assert.rejects(bridge.waitForReload('busy'), /仍在等待/);
  await assert.rejects(bridge.waitForReload('unseen'), /未收到此请求/);
  file.text.set(uri, record('cancel', 1, 1, 0));
  const waiting = bridge.waitForReload('cancel'); bridge.cancelWait();
  await assert.rejects(waiting, /已停止等待/);
  console.log('Manager mapping, generation, result checksum, stale/partial response, timeout and lifecycle tests passed.');
}
main().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => {
  global.setTimeout = realTimeout;
  fs.rmSync(temporary, { recursive: true, force: true });
});
