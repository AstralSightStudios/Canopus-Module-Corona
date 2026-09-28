/* Host tests for the Vela interconnect theme receiver. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const temporary = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'resource-hook-interconnect-')));
const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!#$%&()*+,./:;<=>?@[]^_`{|}~"';

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
  const native = {
    text, binary, directories,
    readText(options) {
      if (text.has(options.uri)) {
        options.success({ text: text.get(options.uri) });
        return;
      }
      if (binary.has(options.uri)) {
        options.success({ text: Buffer.from(binary.get(options.uri)).toString('utf8') });
        return;
      }
      options.fail('missing', 301);
    },
    writeText(options) {
      text.set(options.uri, options.text);
      options.success();
    },
    readArrayBuffer(options) {
      const value = binary.get(options.uri);
      if (!value) { options.fail('missing', 301); return; }
      const start = options.position || 0;
      options.success({ buffer: value.slice(start, options.length === undefined ? undefined : start + options.length) });
    },
    writeArrayBuffer(options) {
      const previous = binary.get(options.uri) || new Uint8Array(0);
      const start = options.position || 0;
      const next = new Uint8Array(Math.max(previous.length, start + options.buffer.length));
      next.set(previous);
      next.set(options.buffer, start);
      binary.set(options.uri, next);
      options.success();
    },
    get(options) {
      if (binary.has(options.uri)) {
        options.success({ type: 'file', length: binary.get(options.uri).length });
      } else if (text.has(options.uri)) {
        options.success({ type: 'file', length: Buffer.byteLength(text.get(options.uri)) });
      } else if (directories.has(options.uri)) {
        options.success({ type: 'dir', length: 0 });
      } else {
        options.fail('missing', 301);
      }
    },
    mkdir(options) {
      let current = 'internal://';
      for (const part of options.uri.slice('internal://'.length).split('/').filter(Boolean)) {
        current += `${part}/`;
        directories.add(current);
        directories.add(current.slice(0, -1));
      }
      options.success();
    },
    delete(options) {
      text.delete(options.uri);
      binary.delete(options.uri);
      options.success();
    },
    rmdir(options) {
      const prefix = options.uri.endsWith('/') ? options.uri : `${options.uri}/`;
      for (const key of [...binary.keys()]) if (key.startsWith(prefix)) binary.delete(key);
      for (const key of [...text.keys()]) if (key.startsWith(prefix)) text.delete(key);
      for (const key of [...directories]) if (key === options.uri || key.startsWith(prefix)) directories.delete(key);
      options.success();
    }
  };
  return native;
}

async function main() {
  assert.equal(alphabet.length, 91);
  assert.equal(encodeBase91(Buffer.from('test')), 'fPNKd');
  execFileSync(path.join(root, 'manager/node_modules/.bin/tsc'), [
    path.join(root, 'manager/src/ts/interconnect.ts'), '--outDir', temporary,
    '--module', 'commonjs', '--target', 'es2018', '--lib', 'es2018,dom', '--skipLibCheck', '--allowJs'
  ], { stdio: 'inherit' });

  const native = makeNativeApi();
  const sent = [];
  const innerSent = () => sent.map(envelope => {
    assert.deepEqual(Object.keys(envelope), ['msg']);
    assert.equal(typeof envelope.msg, 'string');
    return envelope.msg;
  });
  let apkStatus;
  const connection = {
    onmessage: null, onopen: null, onclose: null, onerror: null,
    getApkStatus() { return apkStatus; },
    send(options) { sent.push(options.data); options.success(); }
  };
  const boundary = path.join(temporary, 'import.js');
  require.cache[boundary] = {
    id: boundary, filename: boundary, loaded: true,
    exports: { __esModule: true, file: native, interconnect: { instance: () => connection } }
  };
  const receiverModule = require(path.join(temporary, 'interconnect.js'));
  const { InterconnectThemeReceiver, decodeBase91, validateThemeRelativePath } = receiverModule;
  const { parseResourcePackManifest, serializeResourcePackMappings, validateResourcePackFiles } =
    require(path.join(temporary, 'resource-pack.js'));
  assert.equal(Buffer.from(decodeBase91('fPNKd')).toString(), 'test');
  assert.deepEqual([...decodeBase91(encodeBase91([0, 1, 2, 255]))], [0, 1, 2, 255]);
  for (let length = 0; length <= 128; length++) {
    const bytes = Uint8Array.from({ length }, (_, index) => (index * 37 + length) & 0xff);
    assert.deepEqual([...decodeBase91(encodeBase91(bytes), length)], [...bytes]);
  }
  assert.throws(() => decodeBase91('fPNKd', 5), /预期/);
  assert.throws(() => decodeBase91(' '), /非法字符/);
  assert.equal(validateThemeRelativePath('app/settings/launcher.bin', 'dark'), true);
  for (const unsafe of ['/absolute', 'C:/absolute', '../escape', 'app//x', 'app/./x', 'app/../x', 'app\\x'])
    assert.equal(validateThemeRelativePath(unsafe, 'dark'), false, unsafe);

  const canoraObject = {
    format: 'canopus-resource-pack',
    formatVersion: 1,
    themeId: 'dark',
    name: 'Dark',
    version: '1.0.0',
    author: 'Canopus',
    description: 'Dark resource set',
    targets: ['xiaomi-band-11-4.100.139'],
    mappings: [{
      source: '/resource/icons/',
      destination: 'icons/'
    }]
  };
  const canoraBytes = Buffer.from(JSON.stringify(canoraObject));
  const canoraText = canoraBytes.toString('utf8');
  const parsedCanora = parseResourcePackManifest(canoraText, 'dark');
  assert.equal(parsedCanora.name, 'Dark');
  assert.equal(parsedCanora.mappings.length, 1);
  assert.equal(serializeResourcePackMappings(parsedCanora),
    '/resource/icons/\t/data/quickapp/files/ng.lst.corona/themes/dark/icons/\n');
  assert.doesNotThrow(() => validateResourcePackFiles(parsedCanora, ['canora.json', 'icons/a.bin']));
  assert.throws(() => validateResourcePackFiles(parsedCanora, ['canora.json']), /映射目标.*不存在/);
  assert.throws(() => parseResourcePackManifest(canoraText, 'other'), /不匹配/);
  assert.throws(() => parseResourcePackManifest(JSON.stringify({
    ...canoraObject, mappings: [{ source: '/resource/', destination: '/tmp/theme/' }]
  }), 'dark'), /destination 必须为安全的包内相对路径/);
  assert.throws(() => parseResourcePackManifest(JSON.stringify({
    ...canoraObject, mappings: [{ source: '/resource/', destination: '../outside/' }]
  }), 'dark'), /destination 必须为安全的包内相对路径/);
  assert.throws(() => parseResourcePackManifest(JSON.stringify({
    ...canoraObject, mappings: [{ source: '/resource/\\t', destination: 'icons/' }]
  }), 'dark'), /source 必须为绝对路径/);

  apkStatus = 'connected';
  const startupReceiver = new InterconnectThemeReceiver();
  startupReceiver.start();
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.deepEqual(innerSent().map(packet => JSON.parse(packet.slice(1))), [
    { version: 2, type: 'announce', maxTextChars: 18000, maxWindow: 4 }
  ]);
  assert.deepEqual(sent[0], { msg: 'H{"version":2,"type":"announce","maxTextChars":18000,"maxWindow":4}' });
  startupReceiver.stop();
  for (const openStatus of [true, 1, { status: 1 }, 'OPEN', ' CONNECTED ', { status: 'READY' }]) {
    sent.length = 0;
    apkStatus = openStatus;
    const knownStatusReceiver = new InterconnectThemeReceiver();
    knownStatusReceiver.start();
    await new Promise(resolve => setTimeout(resolve, 0));
    assert.deepEqual(innerSent(), ['H{"version":2,"type":"announce","maxTextChars":18000,"maxWindow":4}']);
    knownStatusReceiver.stop();
  }
  // An unknown/disconnected status must not be mistaken for an open connection.
  sent.length = 0;
  apkStatus = 'disconnected';
  const waitingReceiver = new InterconnectThemeReceiver();
  waitingReceiver.start();
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.deepEqual(innerSent(), []);
  connection.onopen({});
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.deepEqual(innerSent(), ['H{"version":2,"type":"announce","maxTextChars":18000,"maxWindow":4}']);
  waitingReceiver.stop();
  sent.length = 0;
  apkStatus = undefined;

  const states = [];
  let transferPageRequests = 0;
  let receiver = new InterconnectThemeReceiver(() => { transferPageRequests++; });
  const unsubscribe = receiver.subscribe(snapshot => states.push(snapshot));
  receiver.start();
  assert.equal(states.at(-1).phase, 'waiting');
  connection.onerror({ data: 'peer not connected' });
  assert.equal(states.at(-1).phase, 'waiting');
  assert.match(states.at(-1).message, /等待手机端互联连接/);
  connection.onopen({});
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.deepEqual(innerSent(), ['H{"version":2,"type":"announce","maxTextChars":18000,"maxWindow":4}']);

  async function deliver(packet) {
    const start = sent.length;
    connection.onmessage({ data: packet });
    await new Promise(resolve => setTimeout(resolve, 0));
    return innerSent().slice(start);
  }
  const messages = packets => packets.map(packet => packet[0] === 'T' || packet[0] === 'P' || packet[0] === 'E'
    ? JSON.parse(packet.slice(1)) : packet);
  assert.deepEqual(await deliver('H' + JSON.stringify({
    version: 2, type: 'announce', maxTextChars: 18000, maxWindow: 4
  })), []);
  assert.deepEqual(await deliver('H' + JSON.stringify({
    version: 2, type: 'response', replyTo: 'earlier', maxTextChars: 18000, maxWindow: 4
  })), []);
  const request = 'H' + JSON.stringify({
    version: 2, type: 'request', requestId: 'peer_42-abc', maxTextChars: 18000
  });
  const expectedHandshakeResponse = 'H' + JSON.stringify({
    version: 2, type: 'response', replyTo: 'peer_42-abc', maxTextChars: 18000, maxWindow: 4
  });
  assert.deepEqual(await deliver(request), [expectedHandshakeResponse]);
  assert.deepEqual(await deliver(request), [expectedHandshakeResponse]);

  native.text.set('internal://files/mappings.tsv',
    '/resource/icons/\t/data/quickapp/files/ng.lst.corona/themes/locked/icons/\n');
  const activeReply = messages(await deliver('T' + JSON.stringify({
    operation: 'begin', themeId: 'locked', mode: 'replace', fileCount: 1, totalBytes: 2
  })));
  assert(activeReply.some(packet => packet.errorCode === 'active-theme'));

  const manifest = [
    { relativePath: 'canora.json', sizeBytes: canoraBytes.length },
    { relativePath: 'icons/a.bin', sizeBytes: 3 }
  ];
  const header = { themeId: 'dark', mode: 'replace', fileCount: 2, totalBytes: canoraBytes.length + 3 };
  let reply = messages(await deliver('T' + JSON.stringify({ operation: 'begin', ...header })));
  assert(reply.some(packet => packet.operation === 'ack' && packet.itemType === 'begin'));
  reply = messages(await deliver('T' + JSON.stringify({ operation: 'file', themeId: 'dark', fileIndex: 0,
    relativePath: '../bad', sizeBytes: 2 })));
  assert(reply.some(packet => packet.errorCode === 'invalid-manifest' || packet.errorCode === 'invalid-path'));

  for (let fileIndex = 0; fileIndex < manifest.length; fileIndex++) {
    reply = messages(await deliver('T' + JSON.stringify({ operation: 'file', themeId: 'dark', fileIndex,
      ...manifest[fileIndex] })));
    assert(reply.some(packet => packet.operation === 'ack' && packet.itemType === 'file' && packet.fileIndex === fileIndex));
  }
  reply = messages(await deliver('T' + JSON.stringify({ operation: 'end', themeId: 'dark' })));
  assert(reply.some(packet => packet.operation === 'status' && packet.status === 'ready'));
  assert.equal(native.text.has('internal://files/themes/dark/mappings.tsv'), false);
  assert.equal(transferPageRequests, 0); // No navigation before the first valid P packet.
  reply = messages(await deliver('P' + JSON.stringify({ themeId: 'other', fileIndex: 0,
    sizeBytes: 2, chunkSizeBytes: 1, chunkCount: 2 })));
  assert(reply.some(packet => packet.errorCode === 'invalid-manifest'));
  assert.equal(transferPageRequests, 0);

  const canoraChunkSize = Math.ceil(canoraBytes.length / 2);
  const canoraChunkCount = Math.ceil(canoraBytes.length / canoraChunkSize);
  const canoraChunk = index => canoraBytes.slice(index * canoraChunkSize,
    Math.min(canoraBytes.length, (index + 1) * canoraChunkSize));
  reply = messages(await deliver('P' + JSON.stringify({ themeId: 'dark', fileIndex: 0,
    sizeBytes: canoraBytes.length, chunkSizeBytes: canoraChunkSize, chunkCount: canoraChunkCount })));
  assert(reply.some(packet => packet.status === 'ready'));
  assert.equal(transferPageRequests, 1);
  reply = messages(await deliver(`F00000001${encodeBase91(canoraChunk(1))}`));
  assert(reply.includes('A00000001'));
  reply = messages(await deliver('P' + JSON.stringify({ themeId: 'dark', fileIndex: 0,
    sizeBytes: canoraBytes.length, chunkSizeBytes: canoraChunkSize, chunkCount: canoraChunkCount })));
  assert(reply.some(packet => packet.status === 'resume' &&
    JSON.stringify(packet.receivedRanges) === '[[1,1]]'));
  assert.equal(transferPageRequests, 1); // P packets for further files do not navigate again.
  reply = messages(await deliver(`F00000000${encodeBase91(canoraChunk(0))}`));
  assert(reply.includes('A00000000'));
  assert.equal(states.at(-1).phase, 'receiving');
  assert.equal(states.at(-1).percent, Math.floor(canoraBytes.length * 100 / header.totalBytes));
  connection.onerror({ data: 'link lost' });
  assert.equal(states.at(-1).phase, 'error');
  assert.match(states.at(-1).message, /接收进度已保留/);
  assert.equal(states.at(-1).percent, Math.floor(canoraBytes.length * 100 / header.totalBytes));
  connection.onopen({ isReconnected: true });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(states.at(-1).phase, 'ready');
  assert.deepEqual([...native.binary.get('internal://files/themes/dark/canora.json')], [...canoraBytes]);
  const savedProgress = JSON.parse(native.text.get('internal://files/interconnect-transfer.json'));
  assert.equal(savedProgress.files[0].receivedBitmap, '3');
  assert.equal(Object.hasOwn(savedProgress.files[0], 'receivedChunks'), false);

  // A new Manager instance resumes the persisted manifest and acknowledges the stored range.
  receiver.stop();
  unsubscribe();
  const resumedStates = [];
  receiver = new InterconnectThemeReceiver(() => { transferPageRequests++; });
  receiver.subscribe(snapshot => resumedStates.push(snapshot));
  receiver.start();
  await deliver('T' + JSON.stringify({ operation: 'begin', ...header, mode: 'resume' }));
  for (let fileIndex = 0; fileIndex < manifest.length; fileIndex++)
    await deliver('T' + JSON.stringify({ operation: 'file', themeId: 'dark', fileIndex, ...manifest[fileIndex] }));
  await deliver('T' + JSON.stringify({ operation: 'end', themeId: 'dark' }));

  reply = messages(await deliver('P' + JSON.stringify({ themeId: 'dark', fileIndex: 0,
    sizeBytes: canoraBytes.length, chunkSizeBytes: canoraChunkSize, chunkCount: canoraChunkCount })));
  assert(reply.some(packet => packet.status === 'complete' &&
    JSON.stringify(packet.receivedRanges) === '[[0,1]]'));
  assert.equal(transferPageRequests, 2); // A resumed upload on a new app session opens once.
  reply = await deliver('C0000');
  assert(reply.includes('C0000'));
  const completedProgress = JSON.parse(native.text.get('internal://files/interconnect-transfer.json'));
  assert.equal(completedProgress.files[0].receivedBitmap, '');
  reply = await deliver(`F00000000${encodeBase91([97])}`);
  assert(reply.includes('A00000000'));
  assert.equal(JSON.parse(native.text.get('internal://files/interconnect-transfer.json')).files[0].receivedBitmap, '');

  await deliver('P' + JSON.stringify({ themeId: 'dark', fileIndex: 1,
    sizeBytes: 3, chunkSizeBytes: 3, chunkCount: 1 }));
  assert.equal(transferPageRequests, 2);
  reply = await deliver(`F00010000${encodeBase91([1, 2, 255])}`);
  assert(reply.includes('A00010000'));
  assert.deepEqual([...native.binary.get('internal://files/themes/dark/icons/a.bin')], [1, 2, 255]);
  reply = await deliver('C0001');
  assert(reply.includes('C0001'));
  reply = messages(await deliver('T' + JSON.stringify({ operation: 'finish', themeId: 'dark' })));
  assert(reply.some(packet => packet.operation === 'status' && packet.status === 'ready'));
  assert.deepEqual(JSON.parse(native.text.get('internal://files/interconnect-themes.json')), ['dark']);
  assert.equal(native.text.get('internal://files/themes/dark/mappings.tsv'),
    '/resource/icons/\t/data/quickapp/files/ng.lst.corona/themes/dark/icons/\n');
  assert.deepEqual(JSON.parse(native.text.get('internal://files/resource-order.json')),
    { version: 1, order: ['dark', '@system'] });
  assert.deepEqual(JSON.parse(native.text.get('internal://files/resource-files.json')).themes.dark,
    manifest.map(item => ({ relativePath: item.relativePath, sizeBytes: item.sizeBytes })));
  assert.equal(resumedStates.at(-1).phase, 'success');
  assert.equal(resumedStates.at(-1).percent, 100);

  reply = messages(await deliver('T' + JSON.stringify({
    operation: 'begin', themeId: 'legacy', mode: 'replace', fileCount: 1, totalBytes: 2
  })));
  assert(reply.some(packet => packet.operation === 'ack' && packet.itemType === 'begin'));
  await deliver('T' + JSON.stringify({ operation: 'file', themeId: 'legacy', fileIndex: 0,
    relativePath: 'mappings.tsv', sizeBytes: 2 }));
  reply = messages(await deliver('T' + JSON.stringify({ operation: 'end', themeId: 'legacy' })));
  assert(reply.some(packet => packet.errorCode === 'invalid-manifest'));
  assert.deepEqual(JSON.parse(native.text.get('internal://files/interconnect-themes.json')), ['dark']);

  receiver.stop();
  console.log('Interconnect canora manifest, path validation, active-theme guard, Base91, chunk ACK, resume and finish tests passed.');
}
main().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => {
  fs.rmSync(temporary, { recursive: true, force: true });
});
