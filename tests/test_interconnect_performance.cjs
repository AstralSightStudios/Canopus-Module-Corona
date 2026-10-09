/* Host regression tests for complete uploads, progress, and native I/O budgets. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!#$%&()*+,./:;<=>?@[]^_`{|}~"';
const themeRoot = 'internal://files/themes/performance/';
const hex4 = value => value.toString(16).padStart(4, '0');
const control = (type, fields) => type + JSON.stringify(fields);
const chunkPacket = (index, chunk, bytes) => `F${hex4(index)}${hex4(chunk)}${encodeBase91(bytes)}`;

function encodeBase91(data) {
  let accumulator = 0;
  let bitCount = 0;
  let output = '';
  for (const byte of data) {
    accumulator |= byte << bitCount;
    bitCount += 8;
    if (bitCount > 13) {
      let value = accumulator & 8191;
      if (value > 88) {
        accumulator >>>= 13;
        bitCount -= 13;
      } else {
        value = accumulator & 16383;
        accumulator >>>= 14;
        bitCount -= 14;
      }
      output += alphabet[value % 91] + alphabet[Math.floor(value / 91)];
    }
  }
  if (bitCount) {
    output += alphabet[accumulator % 91];
    if (bitCount > 7 || accumulator > 90) output += alphabet[Math.floor(accumulator / 91)];
  }
  return output;
}

function makeNativeApi() {
  const text = new Map();
  const binary = new Map();
  const directories = new Set(['internal://files/']);
  const calls = [];
  const hooks = {};
  const native = { text, binary, directories, calls, hooks };
  const operations = {
    readText(o) {
      if (text.has(o.uri)) return o.success({ text: text.get(o.uri) });
      if (binary.has(o.uri)) return o.success({ text: Buffer.from(binary.get(o.uri)).toString('utf8') });
      o.fail('missing', 301);
    },
    writeText(o) { text.set(o.uri, o.text); o.success(); },
    readArrayBuffer(o) {
      if (!binary.has(o.uri)) return o.fail('missing', 301);
      const start = o.position || 0;
      o.success({ buffer: binary.get(o.uri).slice(start, o.length === undefined ? undefined : start + o.length) });
    },
    writeArrayBuffer(o) {
      const previous = binary.get(o.uri) || new Uint8Array(0);
      const start = o.position || 0;
      const next = new Uint8Array(Math.max(previous.length, start + o.buffer.length));
      next.set(previous);
      next.set(o.buffer, start);
      binary.set(o.uri, next);
      o.success();
    },
    get(o) {
      if (binary.has(o.uri)) return o.success({ type: 'file', length: binary.get(o.uri).length });
      if (text.has(o.uri)) return o.success({ type: 'file', length: Buffer.byteLength(text.get(o.uri)) });
      if (directories.has(o.uri)) return o.success({ type: 'dir', length: 0 });
      o.fail('missing', 301);
    },
    mkdir(o) {
      let current = 'internal://';
      for (const part of o.uri.slice(current.length).split('/').filter(Boolean)) {
        current += `${part}/`;
        directories.add(current);
        directories.add(current.slice(0, -1));
      }
      o.success();
    },
    delete(o) { text.delete(o.uri); binary.delete(o.uri); o.success(); },
    rmdir(o) {
      const prefix = o.uri.endsWith('/') ? o.uri : `${o.uri}/`;
      for (const store of [text, binary, directories]) {
        for (const key of store.keys()) if (key === o.uri || key.startsWith(prefix)) store.delete(key);
      }
      o.success();
    }
  };
  for (const [operation, implementation] of Object.entries(operations)) {
    native[operation] = options => {
      const call = { operation, uri: options.uri, position: options.position, bytes: operation === 'writeText'
        ? Buffer.byteLength(options.text) : operation === 'writeArrayBuffer' ? options.buffer.length : 0 };
      calls.push(call);
      if (hooks[operation] && hooks[operation](options, () => implementation(options))) return;
      implementation(options);
    };
  }
  return native;
}

function makeFiles(assetCount = 32, assetSize = 4096) {
  const manifest = {
    format: 'canopus-resource-pack', formatVersion: 1, themeId: 'performance',
    name: 'Performance', version: '1.0.0', author: 'Host tests', description: 'I/O regression fixture',
    targets: ['xiaomi-band-11-4.100.139'],
    mappings: [{ source: '/resource/icons/', destination: 'icons/' }]
  };
  return [{ relativePath: 'corona.json', bytes: Buffer.from(JSON.stringify(manifest)) },
    ...Array.from({ length: assetCount }, (_, index) => ({
      relativePath: `icons/group${index % 4}/asset${index}.bin`,
      bytes: Buffer.from(Array.from({ length: assetSize }, (_, offset) => (offset * 37 + index * 19) & 255))
    }))];
}

async function main(temporary) {
  execFileSync(path.join(root, 'manager/node_modules/.bin/tsc'), [
    path.join(root, 'manager/src/ts/interconnect.ts'), '--outDir', temporary,
    '--module', 'commonjs', '--target', 'es2018', '--lib', 'es2018,dom', '--skipLibCheck', '--allowJs'
  ], { stdio: 'inherit' });
  let activeNative;
  let activeConnection;
  const proxy = {};
  for (const operation of ['readText', 'writeText', 'readArrayBuffer', 'writeArrayBuffer', 'get', 'mkdir', 'delete', 'rmdir'])
    proxy[operation] = options => activeNative[operation](options);
  const boundary = path.join(temporary, 'import.js');
  require.cache[boundary] = { id: boundary, filename: boundary, loaded: true,
    exports: { __esModule: true, app: { terminate() {} }, file: proxy,
      interconnect: { instance: () => activeConnection } } };
  const { InterconnectThemeReceiver } = require(path.join(temporary, 'interconnect.js'));

  function harness() {
    const native = makeNativeApi();
    const sent = [];
    const snapshots = [];
    const connection = { onmessage: null, onopen: null, onclose: null, onerror: null,
      send(options) {
        assert.deepEqual(Object.keys(options.data), ['msg']);
        sent.push(options.data.msg);
        options.success();
      } };
    activeNative = native;
    activeConnection = connection;
    let receiver;
    function restart() {
      if (receiver) receiver.stop();
      receiver = new InterconnectThemeReceiver();
      receiver.subscribe(snapshot => snapshots.push(snapshot));
      receiver.start();
    }
    restart();
    return {
      native, sent, snapshots, connection, restart,
      get receiver() { return receiver; },
      get state() { return receiver.state; }, // TS private fields are deliberately inspected in host JS.
      get progress() { return snapshots.at(-1); },
      async deliver(packet) {
        const start = sent.length;
        connection.onmessage({ data: packet });
        await receiver.queue;
        return sent.slice(start);
      },
      async prepare(index, file, chunkSizeBytes) {
        return this.deliver(control('P', { themeId: 'performance', fileIndex: index,
          sizeBytes: file.bytes.length, chunkSizeBytes, chunkCount: Math.ceil(file.bytes.length / chunkSizeBytes) }));
      },
      async manifest(files, mode = 'replace') {
        const reply = await this.deliver(control('T', { operation: 'begin', themeId: 'performance', mode,
          fileCount: files.length, totalBytes: files.reduce((sum, file) => sum + file.bytes.length, 0) }));
        assert(reply.some(packet => packet.startsWith('T') && JSON.parse(packet.slice(1)).itemType === 'begin'));
        for (const [fileIndex, file] of files.entries()) {
          const result = await this.deliver(control('T', { operation: 'file', themeId: 'performance', fileIndex,
            relativePath: file.relativePath, sizeBytes: file.bytes.length }));
          assert(result.some(packet => packet.startsWith('T') && JSON.parse(packet.slice(1)).fileIndex === fileIndex));
        }
        const end = await this.deliver(control('T', { operation: 'end', themeId: 'performance' }));
        assert(end.some(packet => packet.startsWith('T') && JSON.parse(packet.slice(1)).status === 'ready'));
      }
    };
  }

  function assertProgress(h, expected) {
    assert.equal(h.progress.bytesReceived, expected, 'progress must count unique decoded bytes');
    assert.equal(h.progress.percent, Math.floor(expected * 100 / h.state.totalBytes));
    assert.equal(h.progress.totalBytes, h.state.totalBytes);
  }
  async function sendChunk(h, index, chunk, bytes, expected) {
    const result = await h.deliver(chunkPacket(index, chunk, bytes));
    assert(result.includes(`A${hex4(index)}${hex4(chunk)}`), 'durable chunks must be acknowledged');
    assertProgress(h, expected);
  }

  // 33 files, 32 * 4096 asset bytes, 1024 asset chunks, plus manifest chunks.
  // Measure the old full-snapshot counterfactual for exactly the same successful
  // saves, after save has applied changedFile. No counterfactual data is persisted.
  const h = harness();
  let baselineStateBytes = 0;
  const saveState = h.receiver.stateStorage.save.bind(h.receiver.stateStorage);
  h.receiver.stateStorage.save = async function (next, index, changedFile) {
    await saveState(next, index, changedFile);
    baselineStateBytes += Buffer.byteLength(JSON.stringify(next));
  };
  const files = makeFiles();
  const uniqueParents = new Set(files.map(file => themeRoot + file.relativePath.slice(0, file.relativePath.lastIndexOf('/') + 1)));
  await h.manifest(files);
  let received = 0;
  let assetChunks = 0;
  let progressScans = 0;
  const countFileBytes = h.receiver.receivedBytesForFile;
  assert.equal(typeof countFileBytes, 'function');
  h.receiver.receivedBytesForFile = function (...args) {
    progressScans++;
    return countFileBytes.apply(this, args);
  };
  const directoryCalls = [];
  for (const [index, file] of files.entries()) {
    const chunkSize = 128;
    const count = Math.ceil(file.bytes.length / chunkSize);
    await h.prepare(index, file, chunkSize);
    assertProgress(h, received);
    const order = Array.from({ length: count }, (_, chunk) => count - 1 - chunk);
    for (const chunk of order) {
      const bytes = file.bytes.subarray(chunk * chunkSize, (chunk + 1) * chunkSize);
      const start = h.native.calls.length;
      received += bytes.length;
      const scansBeforeChunk = progressScans;
      await sendChunk(h, index, chunk, bytes, received);
      assert.equal(progressScans, scansBeforeChunk, 'chunk progress must use byte deltas, not scan bitmaps');
      directoryCalls.push(...h.native.calls.slice(start).filter(call => call.operation === 'get' && uniqueParents.has(call.uri)));
      if (index > 0) assetChunks++;
      if (chunk === order[0]) {
        const writes = h.native.calls.filter(call => call.operation.startsWith('write')).length;
        const scansBeforeDuplicate = progressScans;
        await sendChunk(h, index, chunk, bytes, received);
        assert.equal(progressScans, scansBeforeDuplicate, 'duplicate progress must not scan bitmaps');
        assert.equal(h.native.calls.filter(call => call.operation.startsWith('write')).length, writes,
          'duplicate chunks must not write payload or state');
      }
    }
    assert.equal(count, order.length);
    assert((await h.deliver(`C${hex4(index)}`)).includes(`C${hex4(index)}`));
    assert(h.state.files[index].complete);
    assertProgress(h, received);
    assert.deepEqual(Buffer.from(h.native.binary.get(themeRoot + file.relativePath)), file.bytes);
  }
  assert.equal(assetChunks, 1024);
  assert.equal(uniqueParents.size, 5, 'fixture has one root and four asset-group directories');
  assert(directoryCalls.length <= uniqueParents.size * 2 + 2,
    `directory stat budget exceeded: ${directoryCalls.length} calls for ${uniqueParents.size} parents`);
  const finish = await h.deliver(control('T', { operation: 'finish', themeId: 'performance' }));
  assert(finish.some(packet => packet.startsWith('T') && JSON.parse(packet.slice(1)).status === 'ready'));
  assert(h.state.finished);
  assert.equal(h.progress.bytesReceived, received);
  // Count bytes submitted, not final on-disk sizes. Include every binary journal slot,
  // including rotation checkpoints, without assuming the JSON snapshot is current.
  const stateWrites = h.native.calls.filter(call => call.operation.startsWith('write') &&
    call.uri.startsWith('internal://files/interconnect-transfer.json'));
  assert(stateWrites.some(call => call.operation === 'writeArrayBuffer'), 'uploads must persist binary journal records');
  assert(!stateWrites.some(call => call.operation === 'writeText'), 'legacy snapshots must be read-only');
  const stateBytes = stateWrites.reduce((sum, call) => sum + call.bytes, 0);
  const checkpointSlots = new Set(stateWrites.filter(call => call.operation === 'writeArrayBuffer' &&
    call.position === 0).map(call => call.uri));
  assert.equal(checkpointSlots.size, 2, 'more than 1024 delta records must rotate to the other journal slot');
  assert(stateBytes < baselineStateBytes * 0.2,
    `state writes must materially beat same-workload baseline: ${stateBytes} versus ${baselineStateBytes}`);
  h.restart();
  const rotatedResume = await h.prepare(files.length - 1, files.at(-1), 128);
  assert(rotatedResume.some(packet => packet.startsWith('P') && JSON.parse(packet.slice(1)).status === 'complete'));
  assert(h.state.finished, 'finished state must recover from the rotated journal');
  assert(h.state.files.every(file => file.complete));
  assertProgress(h, received);
  h.receiver.stop();

  // Restart with a partial bitmap, replay the manifest, and receive a short tail out of order.
  const resumed = harness();
  const shortFiles = makeFiles(1, 4099);
  await resumed.manifest(shortFiles);
  await resumed.prepare(0, shortFiles[0], 1024);
  await sendChunk(resumed, 0, 0, shortFiles[0].bytes, shortFiles[0].bytes.length);
  await resumed.deliver('C0000');
  await resumed.prepare(1, shortFiles[1], 1024);
  let shortReceived = shortFiles[0].bytes.length + 3;
  await sendChunk(resumed, 1, 4, shortFiles[1].bytes.subarray(4096), shortReceived);
  resumed.restart();
  await resumed.manifest(shortFiles, 'resume');
  const prepared = await resumed.prepare(1, shortFiles[1], 1024);
  assert(prepared.some(packet => packet.startsWith('P') &&
    JSON.stringify(JSON.parse(packet.slice(1)).receivedRanges) === '[[4,4]]'));
  assertProgress(resumed, shortReceived);
  await sendChunk(resumed, 1, 4, shortFiles[1].bytes.subarray(4096), shortReceived);
  for (const chunk of [2, 0, 3, 1]) {
    shortReceived += 1024;
    await sendChunk(resumed, 1, chunk, shortFiles[1].bytes.subarray(chunk * 1024, (chunk + 1) * 1024), shortReceived);
  }
  assert((await resumed.deliver('C0001')).includes('C0001'));
  assert.deepEqual(Buffer.from(resumed.native.binary.get(themeRoot + shortFiles[1].relativePath)), shortFiles[1].bytes);
  const resumedFinish = await resumed.deliver(control('T', { operation: 'finish', themeId: 'performance' }));
  assert(resumedFinish.some(packet => packet.startsWith('T') && JSON.parse(packet.slice(1)).status === 'ready'));
  resumed.restart();
  const completedPrepare = await resumed.prepare(1, shortFiles[1], 1024);
  assert(completedPrepare.some(packet => packet.startsWith('P') && JSON.parse(packet.slice(1)).status === 'complete'));
  assert(resumed.state.finished);
  assertProgress(resumed, shortReceived);
  resumed.receiver.stop();

  // Replace must invalidate a directory cached by a previous upload.
  const replaced = harness();
  await replaced.manifest(shortFiles);
  await replaced.prepare(0, shortFiles[0], 1024);
  await sendChunk(replaced, 0, 0, shortFiles[0].bytes, shortFiles[0].bytes.length);
  assert(replaced.native.directories.has(themeRoot));
  await replaced.manifest(shortFiles);
  assert(!replaced.native.directories.has(themeRoot));
  await replaced.prepare(0, shortFiles[0], 1024);
  const beforeReplace = replaced.native.calls.length;
  await sendChunk(replaced, 0, 0, shortFiles[0].bytes, shortFiles[0].bytes.length);
  assert(replaced.native.calls.slice(beforeReplace).some(call => call.operation === 'get' && call.uri === themeRoot));
  assert(replaced.native.directories.has(themeRoot));
  replaced.receiver.stop();

  await testFailures(harness, shortFiles, sendChunk);
  console.log(`Interconnect performance tests passed (state bytes: ${stateBytes}; full-snapshot baseline: ${baselineStateBytes}; ${(stateBytes * 100 / baselineStateBytes).toFixed(2)}%; directory stats: ${directoryCalls.length}/${uniqueParents.size} unique parents).`);
}

async function testFailures(harness, files, sendChunk) {
  // A failed stat (including a regular-file parent) must never populate the cache.
  for (const failure of ['io', 'file', 'mkdir']) {
    const h = harness();
    await h.manifest(files);
    await h.prepare(0, files[0], 1024);
    if (failure === 'mkdir') {
      h.native.hooks.mkdir = (o) => { if (o.uri !== themeRoot) return false; o.fail('injected mkdir failure', 300); return true; };
    } else {
      h.native.hooks.get = o => {
        if (o.uri !== themeRoot) return false;
        if (failure === 'file') o.success({ type: 'file', length: 1 });
        else o.fail('injected stat failure', 300);
        return true;
      };
    }
    const packet = chunkPacket(0, 0, files[0].bytes);
    assert(!(await h.deliver(packet)).includes('A00000000'));
    assert.equal(h.state.files[0].receivedBitmap, '0');
    delete h.native.hooks.get;
    delete h.native.hooks.mkdir;
    const start = h.native.calls.length;
    await sendChunk(h, 0, 0, files[0].bytes, files[0].bytes.length);
    assert(h.native.calls.slice(start).some(call => call.operation === 'get' && call.uri === themeRoot));
    h.receiver.stop();
  }
  const failedWrite = harness();
  await failedWrite.manifest(files);
  await failedWrite.prepare(0, files[0], 1024);
  failedWrite.native.hooks.writeArrayBuffer = o => {
    if (o.uri !== themeRoot + 'corona.json') return false;
    o.fail('injected write failure', 300); return true;
  };
  assert(!(await failedWrite.deliver(chunkPacket(0, 0, files[0].bytes))).includes('A00000000'));
  assert.equal(failedWrite.state.files[0].receivedBitmap, '0');
  assert(!failedWrite.native.binary.has(themeRoot + 'corona.json'));
  delete failedWrite.native.hooks.writeArrayBuffer;
  await sendChunk(failedWrite, 0, 0, files[0].bytes, files[0].bytes.length);
  failedWrite.receiver.stop();

  // A successful payload write must not ACK before its journal callback resolves.
  const delayedCommit = harness();
  await delayedCommit.manifest(files);
  await delayedCommit.prepare(0, files[0], 1024);
  let commitEntered;
  let completeCommit;
  const waitingCommit = new Promise(resolve => { commitEntered = resolve; });
  delayedCommit.native.hooks.writeArrayBuffer = (o, complete) => {
    if (!o.uri.startsWith('internal://files/interconnect-transfer.json.journal-')) return false;
    completeCommit = complete;
    commitEntered();
    return true;
  };
  const pendingChunk = delayedCommit.deliver(chunkPacket(0, 0, files[0].bytes));
  await waitingCommit;
  assert(delayedCommit.native.binary.has(themeRoot + 'corona.json'), 'payload must already be written');
  assert(!delayedCommit.sent.includes('A00000000'), 'ACK must wait for journal success');
  assert.equal(delayedCommit.state.files[0].receivedBitmap, '0');
  assert.equal(delayedCommit.progress.bytesReceived, 0);
  delete delayedCommit.native.hooks.writeArrayBuffer;
  completeCommit();
  assert((await pendingChunk).includes('A00000000'));
  assert.equal(delayedCommit.state.files[0].receivedBitmap, '1');
  assert.equal(delayedCommit.progress.bytesReceived, files[0].bytes.length);
  delayedCommit.receiver.stop();

  // A failed journal commit may leave a torn native write. It must not publish
  // a bitmap or progress; retry must overwrite the torn append or replace it
  // with a clean checkpoint in the alternate slot.
  const failedCommit = harness();
  await failedCommit.manifest(files);
  await failedCommit.prepare(0, files[0], 1024);
  let commitFailures = 0;
  let failedPosition;
  let failedUri;
  failedCommit.native.hooks.writeArrayBuffer = o => {
    if (!o.uri.startsWith('internal://files/interconnect-transfer.json.journal-')) return false;
    commitFailures++;
    failedPosition = o.position;
    failedUri = o.uri;
    const previous = failedCommit.native.binary.get(o.uri);
    const prefixLength = Math.floor(o.buffer.length / 2);
    const torn = new Uint8Array(Math.max(previous.length, o.position + prefixLength));
    torn.set(previous);
    torn.set(o.buffer.subarray(0, prefixLength), o.position);
    failedCommit.native.binary.set(o.uri, torn);
    o.fail('injected partial journal write failure', 300);
    return true;
  };
  assert(!(await failedCommit.deliver(chunkPacket(0, 0, files[0].bytes))).includes('A00000000'));
  assert.equal(commitFailures, 1);
  assert.equal(failedCommit.state.files[0].receivedBitmap, '0');
  assert.equal(failedCommit.progress.bytesReceived, 0, 'failed journal writes must not count progress');
  assert(failedPosition > 0, 'failure must target a journal append, not the asset');
  delete failedCommit.native.hooks.writeArrayBuffer;
  const retryStart = failedCommit.native.calls.length;
  await sendChunk(failedCommit, 0, 0, files[0].bytes, files[0].bytes.length);
  const retriedJournal = failedCommit.native.calls.slice(retryStart).filter(call =>
    call.operation === 'writeArrayBuffer' && call.uri.startsWith('internal://files/interconnect-transfer.json.journal-'));
  assert.equal(retriedJournal.length, 1);
  const retriedWrite = retriedJournal[0];
  assert((retriedWrite.uri === failedUri && retriedWrite.position === failedPosition) ||
    (retriedWrite.uri !== failedUri && retriedWrite.position === 0),
    'retry must overwrite the torn append or checkpoint into the alternate slot');
  failedCommit.restart();
  await failedCommit.manifest(files, 'resume');
  const retryPrepared = await failedCommit.prepare(0, files[0], 1024);
  assert(retryPrepared.some(packet => packet.startsWith('P') &&
    JSON.stringify(JSON.parse(packet.slice(1)).receivedRanges) === '[[0,0]]'));
  assert.equal(failedCommit.state.files[0].receivedBitmap, '1');
  assert.equal(failedCommit.progress.bytesReceived, files[0].bytes.length);
  failedCommit.receiver.stop();

  // Let a native await complete after disconnect. Its successful stat must not
  // seed the new connection epoch's directory cache, even if durable work finishes.
  const disconnected = harness();
  await disconnected.manifest(files);
  await disconnected.prepare(0, files[0], 1024);
  let release;
  let entered;
  const pending = new Promise(resolve => { entered = resolve; });
  disconnected.native.hooks.mkdir = (o, complete) => {
    if (o.uri !== themeRoot) return false;
    release = complete;
    entered();
    return true;
  };
  const delivery = disconnected.deliver(chunkPacket(0, 0, files[0].bytes));
  await pending;
  disconnected.connection.onclose({ code: 0 });
  disconnected.connection.onopen({ isReconnected: true });
  delete disconnected.native.hooks.mkdir;
  release();
  await delivery;
  await disconnected.deliver('C0000');
  await disconnected.prepare(1, files[1], 1024);
  // Exercise the exact cache key without a replace resetting the cache.
  // Host JS deliberately invokes the private directory helper.
  const start = disconnected.native.calls.length;
  await disconnected.receiver.ensureDirectory(themeRoot);
  assert(disconnected.native.calls.slice(start).some(call => call.operation === 'get' && call.uri === themeRoot),
    'a pre-disconnect await must not cache a directory in a later epoch');
  disconnected.receiver.stop();
}

const temporary = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'resource-hook-interconnect-performance-')));
main(temporary).catch(error => { console.error(error); process.exitCode = 1; })
  .finally(() => fs.rmSync(temporary, { recursive: true, force: true }));
