/* Host tests for bounded, crash-safe transfer persistence. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const temporary = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'canopus-transfer-storage-')));
const uri = 'internal://files/interconnect-transfer.json';

function makeNative() {
  const binary = new Map();
  const text = new Map();
  const writes = [];
  const native = {
    binary, text, writes, partial: null, ioFailure: null, ioFailureUri: null, preallocate: false,
    readText(o) {
      if (native.ioFailure === 'readText') return o.fail('I/O', 300);
      if (text.has(o.uri)) o.success({ text: text.get(o.uri) });
      else o.fail('missing', 301);
    },
    readArrayBuffer(o) {
      if (native.ioFailure === 'readArrayBuffer' && (!native.ioFailureUri || native.ioFailureUri === o.uri))
        return o.fail('I/O', 300);
      if (!binary.has(o.uri)) return o.fail('missing', 301);
      const start = o.position || 0;
      o.success({ buffer: binary.get(o.uri).slice(start, o.length === undefined ? undefined : start + o.length) });
    },
    get(o) {
      if (native.ioFailure === 'get' && (!native.ioFailureUri || native.ioFailureUri === o.uri))
        return o.fail('I/O', 300);
      if (!binary.has(o.uri)) return o.fail('missing', 301);
      o.success({ length: binary.get(o.uri).length, type: 'file' });
    },
    delete(o) { binary.delete(o.uri); text.delete(o.uri); o.success(); },
    writeArrayBuffer(o) {
      const old = binary.get(o.uri) || new Uint8Array();
      const start = o.position || 0;
      const count = native.partial === null ? o.buffer.length :
        Math.min(o.buffer.length, typeof native.partial === 'function' ? native.partial(o.buffer) : native.partial);
      const next = new Uint8Array(Math.max(old.length, start + (native.preallocate ? o.buffer.length : count)));
      next.set(old);
      next.set(o.buffer.slice(0, count), start);
      binary.set(o.uri, next);
      writes.push({ uri: o.uri, position: start, bytes: o.buffer.length, persisted: count });
      if (native.partial !== null) { native.partial = null; o.fail('partial write', 300); }
      else o.success();
    }
  };
  return native;
}

const native = makeNative();
function reset() {
  native.binary.clear(); native.text.clear(); native.writes.length = 0;
  native.partial = native.ioFailure = native.ioFailureUri = null;
  native.preallocate = false;
}
function entry(relativePath = 'icons/a.bin') {
  return { relativePath, sizeBytes: 100, chunkSizeBytes: null, chunkCount: null,
    receivedBitmap: '', complete: false };
}
function state(files = [], themeId = 'dark') {
  return { version: 1, themeId, mode: 'replace', fileCount: Math.max(1, files.length),
    totalBytes: Math.max(100, files.length * 100), files, manifestComplete: files.length > 0,
    manifestReceiving: files.length === 0, manifestSeen: files.length, finished: false };
}
function sum(bytes) {
  let hash = 0x811c9dc5;
  for (const byte of bytes) hash = Math.imul(hash ^ byte, 0x01000193);
  return hash >>> 0;
}
function frameOffsets(bytes) {
  const offsets = [];
  for (let offset = 0; Buffer.from(bytes.slice(offset, offset + 8)).toString() === 'ICTJ0001';) {
    offsets.push(offset);
    offset += 32 + parseInt(Buffer.from(bytes.slice(offset + 8, offset + 16)).toString(), 16);
  }
  return offsets;
}
function editFrame(bytes, offset, transform) {
  const length = parseInt(Buffer.from(bytes.slice(offset + 8, offset + 16)).toString(), 16);
  const value = JSON.parse(Buffer.from(bytes.slice(offset + 24, offset + 24 + length)).toString());
  transform(value);
  const body = Buffer.from(JSON.stringify(value).replace(/[\u007f-\uffff]/g,
    c => '\\u' + c.charCodeAt(0).toString(16).padStart(4, '0')));
  const lead = Buffer.from('ICTJ0001' + body.length.toString(16).padStart(8, '0') + sum(body).toString(16).padStart(8, '0'));
  return Uint8Array.from(Buffer.concat([Buffer.from(bytes.slice(0, offset)), lead, body,
    Buffer.from('ICTJEND!'), Buffer.alloc(8)]));
}

async function main() {
  execFileSync(path.join(root, 'manager/node_modules/.bin/tsc'), [
    path.join(root, 'manager/src/ts/transfer-storage.ts'), '--outDir', temporary,
    '--module', 'commonjs', '--target', 'es2018', '--lib', 'es2018,dom', '--skipLibCheck', '--allowJs'
  ], { stdio: 'inherit' });
  const boundary = path.join(temporary, 'import.js');
  require.cache[boundary] = { id: boundary, filename: boundary, loaded: true,
    exports: { __esModule: true, file: native } };
  const { TransferStorage } = require(path.join(temporary, 'transfer-storage.js'));
  const store = () => new TransferStorage(uri);
  const load = () => store().load();
  const slot = n => `${uri}.journal-${n}`;

  // Empty storage and pre-journal v1 snapshots; detailed path/chunk validation
  // remains the receiver's responsibility, rather than duplicating that policy.
  assert.equal(await load(), null);
  const legacy = state([entry()]);
  native.text.set(uri, JSON.stringify(legacy));
  assert.deepEqual(await load(), legacy);
  native.text.set(uri, '{');
  await assert.rejects(load());
  native.text.set(uri, JSON.stringify({ ...legacy, version: 2 }));
  await assert.rejects(load(), /Invalid legacy/);
  native.text.set(uri, JSON.stringify({ ...legacy, files: {} }));
  await assert.rejects(load(), /Invalid legacy/);
  native.text.set(uri, JSON.stringify(legacy));
  let s = store();
  const unicode = state([entry('图标/😀/é/\ud800.bin')]);
  await s.save(unicode);
  assert.deepEqual(await load(), unicode);
  assert.equal(native.text.get(uri), JSON.stringify(legacy), 'legacy is never rewritten');
  assert.ok([...native.binary.get(slot(0))].every(byte => byte < 128), 'journal is ASCII-safe');

  // Alternate two slots through many replace/reset operations; stale generations
  // cannot leak their files or delta records into the latest session.
  for (let i = 0; i < 10; i++) {
    const next = state(i % 2 ? [] : [entry(`files/${i}.bin`)], `theme${i}`);
    await s.save(next);
    await s.save({ ...next, mode: 'resume' }, -1);
    assert.deepEqual(await load(), { ...next, mode: 'resume' });
  }
  assert.equal(native.binary.size, 2);
  assert.ok(native.writes.some(w => w.uri === slot(0)) && native.writes.some(w => w.uri === slot(1)));

  // Partial append: no premature array commit; cold recovery and same-instance
  // retry both preserve the previous durable state.
  reset(); s = store();
  let current = state();
  await s.save(current);
  const pending = { ...current, manifestSeen: 1, manifestReceiving: false, manifestComplete: true };
  native.partial = bytes => Math.floor(bytes.length / 2);
  await assert.rejects(s.save(pending, 0, entry()), /partial write/);
  assert.equal(pending.files.length, 0);
  assert.deepEqual(await load(), current);
  await s.save(pending, 0, entry());
  assert.deepEqual(await load(), pending);
  current = pending;
  const completed = { ...entry(), chunkSizeBytes: 100, chunkCount: 1,
    receivedBitmap: '', complete: true };
  native.partial = bytes => Math.floor(bytes.length / 2);
  await assert.rejects(s.save({ ...current }, 0, completed));
  const restarted = store();
  const recovered = await restarted.load();
  assert.deepEqual(recovered, current);
  await restarted.save({ ...recovered }, 0, completed);
  assert.deepEqual((await load()).files[0], completed);

  // A failed large append followed by a shorter retry must ignore old residual
  // bytes, both on the next restart and after further appends.
  reset(); s = store(); current = state([entry()]);
  await s.save(current);
  const large = { ...entry('x'.repeat(8000)), receivedBitmap: 'f'.repeat(512) };
  native.partial = bytes => bytes.length - 80;
  await assert.rejects(s.save({ ...current }, 0, large));
  assert.deepEqual(await load(), current);
  const shorter = entry('short.bin');
  native.partial = bytes => Math.floor(bytes.length / 2);
  await assert.rejects(s.save({ ...current }, 0, shorter));
  assert.deepEqual(await load(), current, 'a second partial retry preserves acknowledged state');
  await s.save({ ...current }, 0, shorter);
  assert.deepEqual((await load()).files[0], shorter);
  s = store(); current = await s.load();
  await s.save({ ...current, mode: 'resume' }, -1);
  assert.equal((await load()).mode, 'resume');

  // Partial inactive checkpoints must not invalidate acknowledged deltas, nor
  // fall back to an older legacy snapshot. Repeat across both slot directions.
  for (let turn = 0; turn < 3; turn++) {
    const before = await load();
    const next = state([entry(`replacement/${turn}.bin`)], `new${turn}`);
    native.partial = bytes => Math.floor(bytes.length / 2);
    await assert.rejects(s.save(next));
    assert.deepEqual(await load(), before);
    await s.save(next);
    await s.save({ ...next, finished: true }, -1);
    assert.equal((await load()).finished, true);
  }

  // Preallocated files can expose zero-filled unwritten space, including a
  // partially written prefix, length/checksum header, or suffix. Recover the
  // active acknowledged session in either slot direction, not legacy.
  const cuts = [0, 4, 13, bytes => bytes.length - 12];
  for (const direction of [0, 1]) {
    for (const cut of cuts) {
      reset(); s = store();
      native.text.set(uri, JSON.stringify(legacy));
      current = state([entry('active.bin')], 'active');
      await s.save(current);
      if (direction === 1) await s.save(current);
      current = { ...current, mode: 'resume' };
      await s.save(current, -1);
      native.preallocate = true; native.partial = cut;
      await assert.rejects(s.save(state([entry('discard.bin')], 'discard')));
      assert.deepEqual(await load(), current, 'torn inactive base must not block active recovery');
      await s.save(state([entry('retry.bin')], 'retry'));
      assert.equal((await load()).themeId, 'retry');
    }
  }
  // A torn final delta keeps the full valid prefix and forces a clean checkpoint
  // on retry. It must not discard previously acknowledged records in that slot.
  for (const cut of cuts) {
    reset(); s = store(); current = state([entry()]);
    await s.save(current);
    current = { ...current, mode: 'resume' };
    await s.save(current, -1);
    native.preallocate = true; native.partial = cut;
    await assert.rejects(s.save({ ...current, finished: true }, -1));
    assert.deepEqual(await load(), current);
    const cold = store(); const prior = await cold.load();
    await cold.save({ ...prior, finished: true }, -1);
    assert.equal((await load()).finished, true);
  }
  // Only a single incomplete first-migration checkpoint may use legacy. Two
  // unusable existing slots cannot silently resurrect an old transfer session.
  reset(); native.text.set(uri, JSON.stringify(legacy)); s = store();
  native.preallocate = true; native.partial = 4;
  await assert.rejects(s.save(state([entry('new.bin')], 'new')));
  assert.deepEqual(await load(), legacy);
  native.binary.set(slot(1), native.binary.get(slot(0)).slice());
  await assert.rejects(load(), /No valid transfer checkpoint/);

  // More than 1024 records trigger a checkpoint; headers do not serialize files.
  reset(); s = store(); current = state([entry()]);
  await s.save(current);
  for (let i = 0; i < 2052; i++) {
    current = { ...current, manifestReceiving: i % 2 === 0 };
    await s.save(current, -1);
  }
  assert.deepEqual(await load(), current);
  assert.equal(native.writes.filter(w => w.position === 0).length, 3);
  assert.equal(native.binary.size, 2);
  for (const bytes of native.binary.values()) {
    assert.ok(frameOffsets(bytes).length <= 1025);
    assert.ok(bytes.length < 1024 * 1024 + 4096);
  }
  // A complete invalid delta is corruption, not an unacknowledged partial tail.
  const activeUri = native.writes[native.writes.length - 1].uri;
  let bytes = native.binary.get(activeUri).slice();
  const offset = frameOffsets(bytes)[1];
  const intact = bytes.slice();
  bytes[offset + 24] ^= 1;
  native.binary.set(activeUri, bytes);
  await assert.rejects(load(), /checksum/);
  native.binary.set(activeUri, editFrame(intact, offset, value => { value.previous ^= 1; }));
  await assert.rejects(load(), /chain/);

  // A zero-filled suffix in a middle record is not a final torn write: the
  // following nonzero frame proves a committed stream exists beyond it.
  native.binary.set(activeUri, intact.slice());
  const middle = intact.slice();
  const middleLength = parseInt(Buffer.from(middle.slice(offset + 8, offset + 16)).toString(), 16);
  middle.fill(0, offset + 24 + middleLength + 4, offset + 32 + middleLength);
  native.binary.set(activeUri, middle);
  await assert.rejects(load(), /boundary/);
  const badPrefix = intact.slice(); badPrefix.fill(0, offset + 4, offset + 8);
  native.binary.set(activeUri, badPrefix);
  await assert.rejects(load(), /prefix/);

  // A corrupted complete base cannot quietly resurrect an obsolete legacy state.
  reset(); native.text.set(uri, JSON.stringify(legacy)); s = store();
  await s.save(state([entry('new.bin')]));
  bytes = native.binary.get(slot(0)).slice(); bytes[24] ^= 1;
  native.binary.set(slot(0), bytes);
  await assert.rejects(load(), /checksum/);

  // Be conservative even with complete corruption in an older inactive slot:
  // no broad catch-and-fallback can lose ACKed progress from an unknown slot.
  reset(); s = store(); await s.save(legacy); await s.save(state([entry('latest.bin')], 'latest'));
  bytes = native.binary.get(slot(0)).slice(); bytes[24] ^= 1;
  native.binary.set(slot(0), bytes);
  await assert.rejects(load(), /checksum/);
  bytes = native.binary.get(slot(1)).slice(); bytes[24] ^= 1;
  native.binary.set(slot(1), bytes);
  native.text.set(uri, JSON.stringify(legacy));
  await assert.rejects(load(), /checksum/);

  // A successful callback containing fewer bytes than get reported is an I/O
  // inconsistency, not a torn on-disk frame that may discard acknowledged deltas.
  reset(); s = store(); await s.save(legacy);
  const originalReadArrayBuffer = native.readArrayBuffer;
  native.readArrayBuffer = o => originalReadArrayBuffer({ ...o,
    success: result => o.success({ buffer: result.buffer.slice(0, 24) }) });
  await assert.rejects(load(), /Short transfer journal read/);
  native.readArrayBuffer = originalReadArrayBuffer;

  // Native errors other than 301 must propagate, even if legacy or the other
  // checkpoint could provide an apparently usable fallback.
  reset(); native.text.set(uri, JSON.stringify(legacy)); s = store(); await s.save(legacy);
  for (const operation of ['get', 'readArrayBuffer']) {
    native.ioFailure = operation;
    await assert.rejects(load(), error => error.code === 300);
    native.ioFailure = null;
  }
  // Check the second/inactive slot too; a valid first slot cannot mask its I/O.
  await s.save(legacy);
  for (const operation of ['get', 'readArrayBuffer']) {
    native.ioFailure = operation; native.ioFailureUri = slot(1);
    await assert.rejects(load(), error => error.code === 300);
    native.ioFailure = native.ioFailureUri = null;
  }
  reset(); native.text.set(uri, JSON.stringify(legacy)); native.ioFailure = 'readText';
  await assert.rejects(load(), error => error.code === 300);

  // Cumulative native bytes should be O(base + changed entries), not O(N*events).
  reset(); s = store();
  current = state(Array.from({ length: 4096 }, (_, i) => entry(`icons/${i}.bin`)));
  await s.save(current);
  const baseBytes = native.writes[0].bytes;
  const oldSlice = current.files.slice;
  current.files.slice = () => { throw new Error('unexpected per-event array copy'); };
  for (let i = 0; i < 500; i++) {
    const changed = { ...current.files[i], chunkSizeBytes: 100, chunkCount: 1,
      receivedBitmap: i % 2 ? '1' : '0' };
    await s.save({ ...current }, i, changed);
  }
  delete current.files.slice;
  assert.equal(current.files.slice, oldSlice);
  const totalBytes = native.writes.reduce((total, w) => total + w.bytes, 0);
  assert.ok(totalBytes < baseBytes + 500 * 1024, `excessive writes: ${totalBytes}`);
  assert.ok(totalBytes < baseBytes * 3);
  assert.deepEqual(await load(), current);
  console.log(`transfer-storage: PASS (500 incremental updates: ${totalBytes} native bytes; base ${baseBytes})`);
}

main().catch(error => { console.error(error); process.exitCode = 1; })
  .finally(() => fs.rmSync(temporary, { recursive: true, force: true }));
