/* Host tests for the Vela interconnect theme receiver. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const temporary = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'resource-hook-interconnect-')));
const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!#$%&()*+,./:;<=>?@[]^_`{|}~"';
const mappingsUri = 'internal://files/mappings.tsv';
const generationsUri = 'internal://files/resource-active-generations.json';

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
  const mutations = [];
  const native = {
    text, binary, directories, mutations,
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
      mutations.push(['writeText', options.uri]);
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
      mutations.push(['writeArrayBuffer', options.uri]);
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
      mutations.push(['mkdir', options.uri]);
      let current = 'internal://';
      for (const part of options.uri.slice('internal://'.length).split('/').filter(Boolean)) {
        current += `${part}/`;
        directories.add(current);
        directories.add(current.slice(0, -1));
      }
      options.success();
    },
    delete(options) {
      mutations.push(['delete', options.uri]);
      text.delete(options.uri);
      binary.delete(options.uri);
      options.success();
    },
    rmdir(options) {
      mutations.push(['rmdir', options.uri]);
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
  const nativeStorageSnapshot = () => ({
    text: new Map(native.text),
    binary: new Map([...native.binary].map(([uri, bytes]) => [uri, [...bytes]])),
    directories: new Set(native.directories),
    mutationCount: native.mutations.length
  });
  const sent = [];
  const innerSent = () => sent.map(envelope => {
    assert.deepEqual(Object.keys(envelope), ['msg']);
    assert.equal(typeof envelope.msg, 'string');
    return envelope.msg;
  });
  let apkStatus;
  let terminateCalls = 0;
  let onTerminate = () => {};
  const app = { terminate() { terminateCalls++; onTerminate(); } };
  const connection = {
    onmessage: null, onopen: null, onclose: null, onerror: null,
    getApkStatus() { return apkStatus; },
    send(options) { sent.push(options.data); options.success(); }
  };
  const boundary = path.join(temporary, 'import.js');
  require.cache[boundary] = {
    id: boundary, filename: boundary, loaded: true,
    exports: { __esModule: true, app, file: native, interconnect: { instance: () => connection } }
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
  assert.equal(validateThemeRelativePath('app/settings/launcher.bin', 'a'.repeat(64)), true);
  assert.equal(validateThemeRelativePath('app/settings/launcher.bin', 'a'.repeat(65)), false);
  assert.equal(validateThemeRelativePath('app/settings/launcher.bin', 'Bad_ID'), false);
  for (const unsafe of ['/absolute', 'C:/absolute', '../escape', 'app//x', 'app/./x', 'app/../x', 'app\\x'])
    assert.equal(validateThemeRelativePath(unsafe, 'dark'), false, unsafe);

  const coronaObject = {
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
  const coronaBytes = Buffer.from(JSON.stringify(coronaObject));
  const coronaText = coronaBytes.toString('utf8');
  const parsedCorona = parseResourcePackManifest(coronaText, 'dark');
  assert.equal(parsedCorona.name, 'Dark');
  assert.equal(Object.hasOwn(parsedCorona, 'versionCode'), false, 'old manifests do not gain a default code');
  for (const versionCode of [0, 1, Number.MAX_SAFE_INTEGER]) {
    const parsed = parseResourcePackManifest(JSON.stringify({ ...coronaObject, versionCode }), 'dark');
    assert.equal(parsed.versionCode, versionCode);
    assert.deepEqual(parseResourcePackManifest(JSON.stringify(parsed), 'dark'), parsed);
  }
  for (const versionCode of [null, '1', true, {}, [], -1, 1.5, Number.MAX_SAFE_INTEGER + 1])
    assert.throws(() => parseResourcePackManifest(JSON.stringify({ ...coronaObject, versionCode }), 'dark'),
      /versionCode/);
  assert.throws(() => parseResourcePackManifest(coronaText.replace('"version":"1.0.0"',
    '"versionCode":1e400'), 'dark'), /versionCode/);
  assert.equal(parsedCorona.mappings.length, 1);
  assert.equal(serializeResourcePackMappings(parsedCorona),
    '/resource/icons/\tthemes/dark/icons/\n');
  const id64 = 'a'.repeat(64);
  const id65 = 'a'.repeat(65);
  const parsed64 = parseResourcePackManifest(JSON.stringify({ ...coronaObject, themeId: id64 }), id64);
  assert.equal(parsed64.themeId, id64);
  assert.throws(() => parseResourcePackManifest(JSON.stringify({ ...coronaObject, themeId: id65 }), id65),
    /themeId/);
  assert.throws(() => parseResourcePackManifest(JSON.stringify({ ...coronaObject, themeId: '' }), ''),
    /themeId/);
  assert.throws(() => parseResourcePackManifest(JSON.stringify({ ...coronaObject, themeId: 'Bad_ID' }), 'Bad_ID'),
    /themeId/);
  assert.doesNotThrow(() => validateResourcePackFiles(parsedCorona, ['corona.json', 'icons/a.bin']));
  assert.throws(() => validateResourcePackFiles(parsedCorona, ['corona.json']), /映射目标.*不存在/);
  assert.throws(() => parseResourcePackManifest(coronaText, 'other'), /不匹配/);
  assert.throws(() => parseResourcePackManifest(JSON.stringify({
    ...coronaObject, mappings: [{ source: '/resource/', destination: '/tmp/theme/' }]
  }), 'dark'), /destination 必须为安全的包内相对路径/);
  assert.throws(() => parseResourcePackManifest(JSON.stringify({
    ...coronaObject, mappings: [{ source: '/resource/', destination: '../outside/' }]
  }), 'dark'), /destination 必须为安全的包内相对路径/);
  assert.throws(() => parseResourcePackManifest(JSON.stringify({
    ...coronaObject, mappings: [{ source: '/resource/\\t', destination: 'icons/' }]
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
    '/resource/icons/\tthemes/locked/icons/\n');
  const beforeActiveReplace = nativeStorageSnapshot();
  const activeReply = messages(await deliver('T' + JSON.stringify({
    operation: 'begin', themeId: 'locked', mode: 'replace', fileCount: 1, totalBytes: 2
  })));
  assert(activeReply.some(packet => packet.errorCode === 'active-theme'));
  assert.deepEqual(nativeStorageSnapshot(), beforeActiveReplace);

  let invalidCaseIndex = 0;
  for (const entries of [
    [{ relativePath: 'corona.json', sizeBytes: 1 }, { relativePath: 'canora.json', sizeBytes: 1 }],
    [{ relativePath: 'nested/corona.json', sizeBytes: 1 }],
    [{ relativePath: 'nested/canora.json', sizeBytes: 1 }],
    ...['corona.json', 'canora.json'].flatMap(relativePath => [
      [{ relativePath, sizeBytes: 0 }], [{ relativePath, sizeBytes: 65537 }],
      [{ relativePath: 'corona.json', sizeBytes: 1 }, { relativePath: `${relativePath}/asset.bin`, sizeBytes: 1 }],
      [{ relativePath: 'canora.json', sizeBytes: 1 }, { relativePath: `${relativePath}/`, sizeBytes: 1 }]
    ])
  ]) {
    const themeId = 'dark';
    entries.push({ relativePath: 'case.bin', sizeBytes: 100 + invalidCaseIndex++ });
    await deliver('T' + JSON.stringify({ operation: 'begin', themeId, mode: 'replace',
      fileCount: entries.length, totalBytes: entries.reduce((sum, entry) => sum + entry.sizeBytes, 0) }));
    const packets = [];
    for (const [fileIndex, entry] of entries.entries())
      packets.push(...messages(await deliver('T' + JSON.stringify({ operation: 'file', themeId, fileIndex, ...entry }))));
    packets.push(...messages(await deliver('T' + JSON.stringify({ operation: 'end', themeId }))));
    assert(packets.some(packet => packet.status === 'reject'), JSON.stringify(entries));
  }

  const manifest = [
    { relativePath: 'corona.json', sizeBytes: coronaBytes.length },
    { relativePath: 'icons/a.bin', sizeBytes: 3 }
  ];
  const header = { themeId: 'dark', mode: 'replace', fileCount: 2, totalBytes: coronaBytes.length + 3 };
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

  const coronaChunkSize = Math.ceil(coronaBytes.length / 2);
  const coronaChunkCount = Math.ceil(coronaBytes.length / coronaChunkSize);
  const coronaChunk = index => coronaBytes.slice(index * coronaChunkSize,
    Math.min(coronaBytes.length, (index + 1) * coronaChunkSize));
  reply = messages(await deliver('P' + JSON.stringify({ themeId: 'dark', fileIndex: 0,
    sizeBytes: coronaBytes.length, chunkSizeBytes: coronaChunkSize, chunkCount: coronaChunkCount })));
  assert(reply.some(packet => packet.status === 'ready'));
  assert.equal(transferPageRequests, 1);
  reply = messages(await deliver(`F00000001${encodeBase91(coronaChunk(1))}`));
  assert(reply.includes('A00000001'));
  reply = messages(await deliver('P' + JSON.stringify({ themeId: 'dark', fileIndex: 0,
    sizeBytes: coronaBytes.length, chunkSizeBytes: coronaChunkSize, chunkCount: coronaChunkCount })));
  assert(reply.some(packet => packet.status === 'resume' &&
    JSON.stringify(packet.receivedRanges) === '[[1,1]]'));
  assert.equal(transferPageRequests, 1); // P packets for further files do not navigate again.
  reply = messages(await deliver(`F00000000${encodeBase91(coronaChunk(0))}`));
  assert(reply.includes('A00000000'));
  assert.equal(states.at(-1).phase, 'receiving');
  assert.equal(states.at(-1).percent, Math.floor(coronaBytes.length * 100 / header.totalBytes));
  connection.onerror({ data: 'link lost' });
  assert.equal(states.at(-1).phase, 'error');
  assert.match(states.at(-1).message, /接收进度已保留/);
  assert.equal(states.at(-1).percent, Math.floor(coronaBytes.length * 100 / header.totalBytes));
  connection.onopen({ isReconnected: true });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(states.at(-1).phase, 'ready');
  assert.deepEqual([...native.binary.get('internal://files/themes/dark/corona.json')], [...coronaBytes]);
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
    sizeBytes: coronaBytes.length, chunkSizeBytes: coronaChunkSize, chunkCount: coronaChunkCount })));
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
    '/resource/icons/\tthemes/dark/icons/\n');
  assert.deepEqual(JSON.parse(native.text.get('internal://files/resource-order.json')),
    { version: 1, order: ['dark', '@system'] });
  assert.deepEqual(JSON.parse(native.text.get('internal://files/resource-files.json')).themes.dark,
    manifest.map(item => ({ relativePath: item.relativePath, sizeBytes: item.sizeBytes })));
  assert.equal(resumedStates.at(-1).phase, 'success');
  assert.equal(resumedStates.at(-1).percent, 100);

  const transferStateUri = 'internal://files/interconnect-transfer.json';
  const installedUri = 'internal://files/interconnect-themes.json';
  const finishedDarkState = native.text.get(transferStateUri);
  for (const fileCount of [0, -1, 1.5, '129', null, 65537, Number.MAX_SAFE_INTEGER + 1]) {
    reply = messages(await deliver('T' + JSON.stringify({
      operation: 'begin', themeId: 'legacy', mode: 'replace', fileCount, totalBytes: 2
    })));
    assert(reply.some(packet => packet.errorCode === 'invalid-manifest'), `reject count ${fileCount}`);
    assert.equal(native.text.get(transferStateUri), finishedDarkState, 'invalid headers preserve saved progress');
  }
  reply = messages(await deliver('T' + JSON.stringify({
    operation: 'begin', themeId: 'legacy', mode: 'replace', fileCount: 129,
    totalBytes: 64 * 1024 * 1024 + 1
  })));
  assert(reply.some(packet => packet.errorCode === 'invalid-manifest'));
  assert.equal(native.text.get(transferStateUri), finishedDarkState, 'the 64 MiB bound is unchanged');

  // The largest representable file index is 0xffff, allowing 65,536 files, not 65,535.
  reply = messages(await deliver('T' + JSON.stringify({
    operation: 'begin', themeId: 'legacy', mode: 'replace', fileCount: 65536,
    totalBytes: 64 * 1024 * 1024
  })));
  assert(reply.some(packet => packet.operation === 'ack' && packet.itemType === 'begin'));
  assert.equal(JSON.parse(native.text.get(transferStateUri)).fileCount, 65536);

  // One directory mapping can back more than 128 actual transferred files.
  const manyFiles = [
    { relativePath: 'canora.json', bytes: Buffer.from(JSON.stringify({ ...coronaObject, themeId: 'legacy' })) },
    ...Array.from({ length: 129 }, (_, index) => ({
      relativePath: `icons/${index}.bin`, bytes: Buffer.from([index])
    }))
  ];
  const manyHeader = { themeId: 'legacy', mode: 'replace', fileCount: manyFiles.length,
    totalBytes: manyFiles.reduce((sum, entry) => sum + entry.bytes.length, 0) };
  reply = messages(await deliver('T' + JSON.stringify({ operation: 'begin', ...manyHeader })));
  assert(reply.some(packet => packet.operation === 'ack' && packet.itemType === 'begin'));
  for (const [fileIndex, entry] of manyFiles.entries()) {
    if (fileIndex === manyFiles.length - 1) {
      reply = messages(await deliver('T' + JSON.stringify({ operation: 'end', themeId: 'legacy' })));
      assert(reply.some(packet => packet.status === 'reject' && packet.errorCode === 'invalid-manifest'),
        'a large manifest must still contain the declared number of entries');
    }
    reply = messages(await deliver('T' + JSON.stringify({ operation: 'file', themeId: 'legacy', fileIndex,
      relativePath: entry.relativePath, sizeBytes: entry.bytes.length })));
    assert(reply.some(packet => packet.operation === 'ack' && packet.fileIndex === fileIndex));
  }
  reply = messages(await deliver('T' + JSON.stringify({ operation: 'file', themeId: 'legacy',
    fileIndex: manyFiles.length, relativePath: 'icons/extra.bin', sizeBytes: 0 })));
  assert(reply.some(packet => packet.errorCode === 'invalid-manifest'));
  reply = messages(await deliver('T' + JSON.stringify({ operation: 'end', themeId: 'legacy' })));
  assert(reply.some(packet => packet.operation === 'status' && packet.status === 'ready'));
  reply = messages(await deliver('T' + JSON.stringify({ operation: 'finish', themeId: 'legacy' })));
  assert(reply.some(packet => packet.errorCode === 'invalid-manifest'), 'unreceived files cannot be installed');
  const savedManyManifest = native.text.get(transferStateUri);

  function restartReceiver(launchToken) {
    receiver.stop();
    receiver = new InterconnectThemeReceiver();
    receiver.captureLaunchContext(launchToken);
    receiver.subscribe(snapshot => resumedStates.push(snapshot));
    receiver.start();
  }
  // Loading large inventories must retain all existing persisted-state validation.
  const invalidSavedStates = [
    state => { state.fileCount = 65537; },
    state => { state.fileCount = 0; },
    state => { state.fileCount = 129.5; },
    state => { state.manifestSeen = state.fileCount + 1; },
    state => { state.manifestSeen--; },
    state => { state.files.pop(); },
    state => { state.files.push({ ...state.files[1], relativePath: 'icons/extra.bin', sizeBytes: 0 }); },
    state => { state.files[1].relativePath = '../escape'; },
    state => { state.files[2].relativePath = state.files[1].relativePath; },
    state => { state.totalBytes = 64 * 1024 * 1024 + 1; }
  ];
  for (const mutate of invalidSavedStates) {
    const state = JSON.parse(savedManyManifest);
    mutate(state);
    const invalidText = JSON.stringify(state);
    native.text.set(transferStateUri, invalidText);
    restartReceiver();
    reply = messages(await deliver('T' + JSON.stringify({ operation: 'begin', ...manyHeader, mode: 'resume' })));
    assert(reply.some(packet => packet.errorCode === 'invalid-manifest'));
    assert.equal(native.text.get(transferStateUri), invalidText, 'invalid loaded states must not be rewritten');
  }

  native.text.set(transferStateUri, savedManyManifest);
  restartReceiver();
  reply = messages(await deliver('T' + JSON.stringify({ operation: 'begin', ...manyHeader,
    mode: 'resume', fileCount: manyHeader.fileCount - 1 })));
  assert(reply.some(packet => packet.errorCode === 'invalid-manifest'));
  assert.equal(native.text.get(transferStateUri), savedManyManifest, 'resume requires a matching count');
  reply = messages(await deliver('T' + JSON.stringify({ operation: 'begin', ...manyHeader, mode: 'resume' })));
  assert(reply.some(packet => packet.operation === 'ack' && packet.itemType === 'begin'),
    'a new receiver accepts a persisted manifest larger than 128 files');
  reply = messages(await deliver('T' + JSON.stringify({ operation: 'file', themeId: 'legacy', fileIndex: 0,
    relativePath: 'corona.json', sizeBytes: manyFiles[0].bytes.length })));
  assert(reply.some(packet => packet.errorCode === 'invalid-manifest'),
    'resume must not alias legacy and canonical manifest paths');
  for (const [fileIndex, entry] of manyFiles.entries()) {
    reply = messages(await deliver('T' + JSON.stringify({ operation: 'file', themeId: 'legacy', fileIndex,
      relativePath: entry.relativePath, sizeBytes: entry.bytes.length })));
    assert(reply.some(packet => packet.operation === 'ack' && packet.fileIndex === fileIndex));
  }
  reply = messages(await deliver('T' + JSON.stringify({ operation: 'end', themeId: 'legacy' })));
  assert(reply.some(packet => packet.operation === 'status' && packet.status === 'ready'));
  for (const [fileIndex, entry] of manyFiles.entries()) {
    reply = messages(await deliver('P' + JSON.stringify({ themeId: 'legacy', fileIndex,
      sizeBytes: entry.bytes.length, chunkSizeBytes: entry.bytes.length, chunkCount: 1 })));
    assert(reply.some(packet => packet.status === 'ready'));
    const fileHex = fileIndex.toString(16).padStart(4, '0');
    assert.deepEqual(await deliver(`F${fileHex}0000${encodeBase91(entry.bytes)}`), [`A${fileHex}0000`]);
    assert.deepEqual(await deliver(`C${fileHex}`), [`C${fileHex}`]);
    assert.deepEqual(Buffer.from(native.binary.get(`internal://files/themes/legacy/${entry.relativePath}`)),
      entry.bytes);
  }
  reply = messages(await deliver('T' + JSON.stringify({ operation: 'finish', themeId: 'legacy' })));
  assert(reply.some(packet => packet.operation === 'status' && packet.status === 'ready'));
  assert.equal(resumedStates.at(-1).phase, 'success');
  assert.equal(resumedStates.at(-1).fileCount, manyFiles.length);
  assert.equal(resumedStates.at(-1).percent, 100);
  assert.deepEqual(JSON.parse(native.text.get(installedUri)), ['dark', 'legacy']);
  assert.deepEqual(JSON.parse(native.text.get('internal://files/resource-order.json')).order,
    ['legacy', 'dark', '@system']);
  assert.deepEqual(JSON.parse(native.text.get('internal://files/resource-files.json')).themes.legacy,
    manyFiles.map(entry => ({ relativePath: entry.relativePath, sizeBytes: entry.bytes.length })));
  assert.equal(native.text.get('internal://files/themes/legacy/mappings.tsv'),
    '/resource/icons/\tthemes/legacy/icons/\n');

  reply = messages(await deliver('T' + JSON.stringify({
    operation: 'begin', themeId: 'legacy', mode: 'replace', fileCount: 1, totalBytes: 2
  })));
  assert(reply.some(packet => packet.operation === 'ack' && packet.itemType === 'begin'));
  await deliver('T' + JSON.stringify({ operation: 'file', themeId: 'legacy', fileIndex: 0,
    relativePath: 'mappings.tsv', sizeBytes: 2 }));
  reply = messages(await deliver('T' + JSON.stringify({ operation: 'end', themeId: 'legacy' })));
  assert(reply.some(packet => packet.errorCode === 'invalid-manifest'));
  assert.deepEqual(JSON.parse(native.text.get('internal://files/interconnect-themes.json')), ['dark']);

  async function transferCapacityPack(ruleCount) {
    // Mapping-rule capacity remains independent of the number of transferred files.
    const bytes = Buffer.from(JSON.stringify({ ...coronaObject, themeId: 'legacy',
      mappings: Array.from({ length: ruleCount }, (_, index) => ({
        source: `/resource/${index}.bin`, destination: 'shared.bin'
      })) }));
    const files = [
      { relativePath: 'corona.json', bytes },
      { relativePath: 'shared.bin', bytes: Buffer.from([7]) }
    ];
    let packets = messages(await deliver('T' + JSON.stringify({
      operation: 'begin', themeId: 'legacy', mode: 'replace', fileCount: files.length,
      totalBytes: bytes.length + 1
    })));
    assert(packets.some(packet => packet.operation === 'ack' && packet.itemType === 'begin'));
    for (const [fileIndex, entry] of files.entries()) {
      packets = messages(await deliver('T' + JSON.stringify({
        operation: 'file', themeId: 'legacy', fileIndex,
        relativePath: entry.relativePath, sizeBytes: entry.bytes.length
      })));
      assert(packets.some(packet => packet.operation === 'ack' && packet.itemType === 'file'));
    }
    packets = messages(await deliver('T' + JSON.stringify({ operation: 'end', themeId: 'legacy' })));
    assert(packets.some(packet => packet.operation === 'status' && packet.status === 'ready'));
    for (const [fileIndex, entry] of files.entries()) {
      const chunkSizeBytes = Math.min(4096, entry.bytes.length);
      const chunkCount = Math.ceil(entry.bytes.length / chunkSizeBytes);
      packets = messages(await deliver('P' + JSON.stringify({ themeId: 'legacy', fileIndex,
        sizeBytes: entry.bytes.length, chunkSizeBytes, chunkCount })));
      assert(packets.some(packet => packet.status === 'ready'));
      const fileHex = fileIndex.toString(16).padStart(4, '0');
      for (let index = 0; index < chunkCount; index++) {
        const chunkHex = index.toString(16).padStart(4, '0');
        const chunk = entry.bytes.slice(index * chunkSizeBytes, (index + 1) * chunkSizeBytes);
        const acknowledgements = await deliver(`F${fileHex}${chunkHex}${encodeBase91(chunk)}`);
        assert(acknowledgements.includes(`A${fileHex}${chunkHex}`));
      }
      assert((await deliver(`C${fileHex}`)).includes(`C${fileHex}`));
    }
    return messages(await deliver('T' + JSON.stringify({ operation: 'finish', themeId: 'legacy' })));
  }

  reply = await transferCapacityPack(256);
  assert(reply.some(packet => packet.operation === 'status' && packet.status === 'ready'));
  const capacityTsv = native.text.get('internal://files/themes/legacy/mappings.tsv');
  assert.equal(capacityTsv.split('\n').filter(Boolean).length, 256);
  assert.ok(Buffer.byteLength(capacityTsv) <= 32 * 1024);
  assert.deepEqual(JSON.parse(native.text.get('internal://files/interconnect-themes.json')), ['dark', 'legacy']);
  assert.equal(JSON.parse(native.text.get('internal://files/resource-files.json')).themes.legacy.length, 2);

  reply = await transferCapacityPack(257);
  assert(reply.some(packet => packet.errorCode === 'invalid-manifest'));
  assert.match(resumedStates.at(-1).message, /256 条/);
  assert.equal(native.text.has('internal://files/themes/legacy/mappings.tsv'), false,
    '257 rules must not publish a derived TSV or register the rejected package');
  assert.deepEqual(JSON.parse(native.text.get('internal://files/interconnect-themes.json')), ['dark']);

  // Exercise the highest wire index without sending 65,536 manifest/control sequences.
  // All earlier files are complete in this persisted transfer; only 0xffff needs one chunk.
  const wireFiles = Array.from({ length: 65536 }, (_, index) => ({
    relativePath: index === 0 ? 'corona.json' : `icons/${index}.bin`,
    sizeBytes: index === 0 || index === 65535 ? 1 : 0,
    chunkSizeBytes: 1,
    chunkCount: index === 0 || index === 65535 ? 1 : 0,
    receivedBitmap: index === 65535 ? '0' : '',
    complete: index !== 65535
  }));
  native.text.set(transferStateUri, JSON.stringify({
    version: 1, themeId: 'wirebound', mode: 'replace', fileCount: wireFiles.length,
    totalBytes: 2, files: wireFiles, manifestComplete: true, manifestReceiving: false,
    manifestSeen: wireFiles.length, finished: false
  }));
  restartReceiver();
  reply = messages(await deliver('P' + JSON.stringify({ themeId: 'wirebound', fileIndex: 65536,
    sizeBytes: 1, chunkSizeBytes: 1, chunkCount: 1 })));
  assert(reply.some(packet => packet.errorCode === 'invalid-manifest'));
  reply = messages(await deliver('P' + JSON.stringify({ themeId: 'wirebound', fileIndex: 65535,
    sizeBytes: 1, chunkSizeBytes: 1, chunkCount: 1 })));
  assert(reply.some(packet => packet.fileIndex === 65535 && packet.status === 'ready'));
  assert.deepEqual(await deliver(`Fffff0000${encodeBase91([7])}`), ['Affff0000']);
  assert.deepEqual(await deliver('Cffff'), ['Cffff']);
  assert.deepEqual([...native.binary.get('internal://files/themes/wirebound/icons/65535.bin')], [7]);
  reply = messages(await deliver('C10000'));
  assert(reply.some(packet => packet.errorCode === 'invalid-manifest'), 'C still requires exactly four hex digits');

  const protectedReplacePacket = 'T' + JSON.stringify({
    operation: 'begin', themeId: 'dark', mode: 'replace', fileCount: 1, totalBytes: 2
  });
  function seedPriorPack() {
    native.text.set(transferStateUri, finishedDarkState);
    native.text.set(installedUri, JSON.stringify(['dark']));
    native.text.set('internal://files/themes/dark/corona.json', coronaText);
    native.binary.set('internal://files/themes/dark/icons/a.bin', Uint8Array.from([1, 2, 255]));
    native.directories.add('internal://files/themes/dark/');
    native.directories.add('internal://files/themes/dark/icons/');
  }

  // The newer TSV may be published even though native still depends on the prior direct pack.
  for (const mappings of [
    '/resource/icons/a.bin\tthemes/light/icons/a.bin\n', '', null
  ]) {
    seedPriorPack();
    if (mappings === null) native.text.delete(mappingsUri);
    else native.text.set(mappingsUri, mappings);
    native.text.set(generationsUri, JSON.stringify({ version: 1, generations: [],
      protectedThemes: mappings ? ['dark', 'light'] : ['dark'] }));
    restartReceiver();
    const before = nativeStorageSnapshot();
    reply = messages(await deliver(protectedReplacePacket));
    assert(reply.some(packet => packet.errorCode === 'active-theme'));
    assert.equal(reply.some(packet => packet.operation === 'ack'), false);
    assert.deepEqual(nativeStorageSnapshot(), before,
      'durable protection must reject replacement without touching files, index or transfer state');

    // Matching acknowledged cleanup releases the old pack but keeps the new direct dependency.
    const acknowledgedRegistry = JSON.stringify({ version: 1, generations: [],
      protectedThemes: mappings ? ['light'] : [] });
    native.text.set(generationsUri, acknowledgedRegistry);
    reply = messages(await deliver(protectedReplacePacket));
    assert(reply.some(packet => packet.operation === 'ack' && packet.itemType === 'begin'));
    assert.equal(native.text.has('internal://files/themes/dark/corona.json'), false);
    assert.equal(native.binary.has('internal://files/themes/dark/icons/a.bin'), false);
    assert.deepEqual(JSON.parse(native.text.get(installedUri)), []);
    assert.equal(JSON.parse(native.text.get(transferStateUri)).manifestReceiving, true);
    assert.equal(native.text.get(generationsUri), acknowledgedRegistry);
  }

  const malformedRegistries = [
    '{invalid', 'null',
    JSON.stringify({ version: 2, generations: [], protectedThemes: ['dark'] }),
    JSON.stringify({ version: 1, generations: 'bad', protectedThemes: ['dark'] }),
    ...[null, 'dark', [42], [''], ['Dark'], ['x'.repeat(65)], ['../dark'], ['dark', 'dark']]
      .map(protectedThemes => JSON.stringify({ version: 1, generations: [], protectedThemes }))
  ];
  for (const [index, registry] of malformedRegistries.entries()) {
    seedPriorPack();
    native.text.set(mappingsUri, index % 2 ? '/resource/icons/\tthemes/light/icons/\n' : '');
    native.text.set(generationsUri, registry);
    restartReceiver();
    const before = nativeStorageSnapshot();
    reply = messages(await deliver(protectedReplacePacket));
    assert(reply.some(packet => typeof packet.errorCode === 'string'));
    assert.equal(reply.some(packet => packet.operation === 'ack'), false);
    assert.deepEqual(nativeStorageSnapshot(), before,
      'malformed durable protection must fail closed without any mutations');
  }

  // A legacy v1 registry without protectedThemes must not block otherwise inactive replacement.
  seedPriorPack();
  native.text.set(mappingsUri, '');
  native.text.set(generationsUri, JSON.stringify({ version: 1, generations: [] }));
  restartReceiver();
  reply = messages(await deliver(protectedReplacePacket));
  assert(reply.some(packet => packet.operation === 'ack' && packet.itemType === 'begin'));
  assert.equal(native.binary.has('internal://files/themes/dark/icons/a.bin'), false);

  // List replies are direct objects, not JSON strings nested in msg.
  async function queryList(packet = 'L{"requestId":"list_42"}') {
    const start = sent.length;
    connection.onmessage({ data: packet });
    await new Promise(resolve => setTimeout(resolve, 0));
    return sent.slice(start).map(value => JSON.parse(JSON.stringify(value)));
  }
  const listState = states.at(-1);
  const listPageRequests = transferPageRequests;
  native.text.delete(installedUri);
  let listReply = await queryList();
  assert.deepEqual(listReply, [{ msg: 'L', replyTo: 'list_42', pageIndex: 0,
    done: true, total: 0, items: [] }]);

  native.text.set(installedUri, JSON.stringify(['legacy', 'dark', 'broken']));
  for (const themeId of ['legacy', 'dark', 'broken']) {
    native.binary.delete(`internal://files/themes/${themeId}/corona.json`);
    native.binary.delete(`internal://files/themes/${themeId}/canora.json`);
  }
  native.text.set('internal://files/themes/dark/corona.json', coronaText);
  native.text.delete('internal://files/themes/dark/canora.json');
  native.text.delete('internal://files/themes/legacy/corona.json');
  native.text.set('internal://files/themes/legacy/canora.json', JSON.stringify({
    ...coronaObject, themeId: 'legacy', name: 'Legacy', version: undefined, author: undefined
  }));
  native.text.set('internal://files/themes/broken/corona.json', '{invalid');
  const beforeList = nativeStorageSnapshot();
  listReply = await queryList();
  assert.deepEqual(listReply[0].items, [
    { themeId: 'broken', name: 'broken', metadataStatus: 'unavailable' },
    { themeId: 'dark', name: 'Dark', version: '1.0.0', author: 'Canopus', metadataStatus: 'ok' },
    { themeId: 'legacy', name: 'Legacy', metadataStatus: 'ok' }
  ]);
  assert.equal(listReply[0].total, 3);
  assert.deepEqual(await queryList(), listReply);
  assert.deepEqual(nativeStorageSnapshot(), beforeList, 'listing must not migrate or write storage');
  for (const versionCode of [0, Number.MAX_SAFE_INTEGER]) {
    native.text.set('internal://files/themes/dark/corona.json', JSON.stringify({ ...coronaObject, versionCode }));
    assert.deepEqual((await queryList())[0].items[1], {
      themeId: 'dark', name: 'Dark', version: '1.0.0', versionCode, author: 'Canopus', metadataStatus: 'ok'
    });
  }
  for (const versionCode of [null, '1', -1, 1.5, Number.MAX_SAFE_INTEGER + 1]) {
    native.text.set('internal://files/themes/dark/corona.json', JSON.stringify({ ...coronaObject, versionCode }));
    assert.deepEqual((await queryList())[0].items[1], {
      themeId: 'dark', name: 'dark', metadataStatus: 'unavailable'
    });
  }
  native.text.set('internal://files/themes/dark/corona.json', coronaText);
  native.text.set('internal://files/themes/legacy/corona.json', '{}');
  assert.equal((await queryList())[0].items[2].metadataStatus, 'unavailable');

  for (const invalidIndex of ['{invalid', '["dark","dark"]', '["../escape"]']) {
    native.text.set(installedUri, invalidIndex);
    assert.deepEqual(await queryList(), [{ msg: 'L', replyTo: 'list_42', errorCode: 'list-failed' }]);
  }
  for (const invalidRequest of ['L{invalid', 'L{}', 'L{"requestId":"bad id"}', 'L[]'])
    assert.deepEqual(await queryList(invalidRequest), [{ msg: 'L', errorCode: 'invalid-request' }]);
  assert.deepEqual(await queryList('L' + JSON.stringify({ requestId: 'list_42', padding: 'x'.repeat(18000) })),
    [{ msg: 'L', replyTo: 'list_42', errorCode: 'invalid-request' }]);

  // A small negotiated budget forces multiple pages without nested escaping.
  connection.onmessage({ data: 'H{"version":2,"type":"request","requestId":"budget","maxTextChars":256}' });
  await new Promise(resolve => setTimeout(resolve, 0));
  native.text.set(installedUri, JSON.stringify(['legacy', 'dark', 'broken']));
  listReply = await queryList();
  assert(listReply.length > 1);
  assert.deepEqual(listReply.flatMap(page => page.items).map(item => item.themeId), ['broken', 'dark', 'legacy']);
  listReply.forEach((page, index) => {
    assert.equal(page.msg, 'L');
    assert.equal(page.replyTo, 'list_42');
    assert.equal(page.pageIndex, index);
    assert.equal(page.total, 3);
    assert.equal(page.done, index === listReply.length - 1);
    assert(JSON.stringify(page).length <= 256);
  });
  native.text.set(installedUri, '["dark"]');
  native.text.set('internal://files/themes/dark/corona.json', JSON.stringify({
    ...coronaObject, name: '"'.repeat(128), author: '\\'.repeat(128)
  }));
  assert.deepEqual(await queryList(), [{ msg: 'L', replyTo: 'list_42', errorCode: 'response-too-large' }]);

  // Native read/send failures stay local to the query, preserving upload state.
  const originalReadText = native.readText;
  native.readText = options => options.uri === installedUri
    ? options.fail('unreadable', 500) : originalReadText(options);
  assert.deepEqual(await queryList(), [{ msg: 'L', replyTo: 'list_42', errorCode: 'list-failed' }]);
  native.readText = originalReadText;
  native.text.delete(installedUri);
  const originalSend = connection.send;
  connection.send = options => options.fail('disconnected', 500);
  assert.deepEqual(await queryList(), []);
  connection.send = originalSend;
  assert.equal((await queryList())[0].done, true, 'a send failure must not poison the receiver queue');
  assert.deepEqual(states.at(-1), listState);
  assert.equal(transferPageRequests, listPageRequests);

  // Q requires cold-launch ownership; rejection never changes upload UI/storage.
  sent.length = 0;
  const launchToken = 'check_42-abc';
  const quitPacket = 'Q' + JSON.stringify({ requestId: 'quit_42', launchToken });
  const quitReply = packets => packets.map(packet => {
    assert.equal(packet[0], 'Q');
    return JSON.parse(packet.slice(1));
  });
  const restartOwnedReceiver = () => restartReceiver(launchToken);
  let expectedTerminateCalls = 0;
  async function checkQuit(rejection) {
    const before = nativeStorageSnapshot();
    const snapshot = resumedStates.at(-1);
    assert.deepEqual(quitReply(await deliver(quitPacket)), [rejection
      ? { replyTo: 'quit_42', status: 'reject', errorCode: rejection }
      : { replyTo: 'quit_42', status: 'ready' }]);
    if (!rejection) expectedTerminateCalls++;
    assert.equal(terminateCalls, expectedTerminateCalls);
    assert.deepEqual(nativeStorageSnapshot(), before, 'Q never mutates resumable state');
    assert.deepEqual(resumedStates.at(-1), snapshot, 'Q never changes upload UI');
  }
  const handshake = async (extra = {}) => (await deliver('H' + JSON.stringify({
    version: 2, type: 'request', requestId: 'owner_probe', maxTextChars: 18000, ...extra
  }))).map(packet => JSON.parse(packet.slice(1)));
  const unownedHandshake = { version: 2, type: 'response', replyTo: 'owner_probe',
    maxTextChars: 18000, maxWindow: 4 };

  // Pre-open/ordinary launch: H stays valid and never echoes a request token.
  restartReceiver();
  assert.deepEqual(await handshake({ launchToken }), [unownedHandshake]);
  receiver.captureLaunchContext(launchToken); // A later index/relaunch cannot adopt ownership.
  assert.deepEqual(await handshake(), [unownedHandshake]);
  await checkQuit('not-owner');
  assert.deepEqual(quitReply(await deliver('Q{"requestId":"quit_42"}')),
    [{ replyTo: 'quit_42', status: 'reject', errorCode: 'invalid-request' }]);

  // First context is consumed even when missing or invalid; subsequent visits cannot replace it.
  for (const token of [undefined, null, '', 42, {}, [], true, 'bad token', 'x'.repeat(65), '\n',
    'check\n', 'check\r', 'check\r\n', ' check', 'check ', '查询', 'check\u0080']) {
    restartReceiver(token);
    receiver.captureLaunchContext(launchToken);
    assert.deepEqual(await handshake({ launchToken }), [unownedHandshake]);
    await checkQuit('not-owner');
  }
  for (const token of ['a', 'x'.repeat(64)]) {
    restartReceiver(token);
    assert.deepEqual(await handshake(), [{ ...unownedHandshake, launchToken: token }]);
    assert.deepEqual(quitReply(await deliver('Q' + JSON.stringify({ requestId: 'quit_42', launchToken: token }))),
      [{ replyTo: 'quit_42', status: 'ready' }]);
    assert.equal(terminateCalls, ++expectedTerminateCalls);
  }
  // A mismatched Q cannot exit even while the original cold context is still owned.
  restartOwnedReceiver();
  assert.deepEqual(await handshake({ launchToken: 'different_token' }),
    [{ ...unownedHandshake, launchToken }]);
  const beforeMismatch = nativeStorageSnapshot();
  assert.deepEqual(quitReply(await deliver('Q' + JSON.stringify({
    requestId: 'quit_42', launchToken: 'different_token'
  }))), [{ replyTo: 'quit_42', status: 'reject', errorCode: 'not-owner' }]);
  assert.deepEqual(nativeStorageSnapshot(), beforeMismatch);
  assert.equal(terminateCalls, expectedTerminateCalls);

  // Any repeated page-context capture revokes permission before touch; it never adopts a new token.
  for (const token of [undefined, launchToken, 'different_token']) {
    restartOwnedReceiver();
    assert.deepEqual(await handshake(), [{ ...unownedHandshake, launchToken }]);
    receiver.captureLaunchContext(token);
    assert.deepEqual(await handshake({ launchToken }), [unownedHandshake]);
    await checkQuit('not-owner');
    assert.deepEqual(quitReply(await deliver('Q' + JSON.stringify({
      requestId: 'quit_42', launchToken: 'different_token'
    }))), [{ replyTo: 'quit_42', status: 'reject', errorCode: 'not-owner' }]);
  }

  restartOwnedReceiver();
  const beforeQuit = nativeStorageSnapshot();
  const quitSnapshot = resumedStates.at(-1);
  for (const invalid of ['Q{invalid', 'Q{}', 'Q[]', 'Q{"requestId":"bad id"}',
    'Q{"requestId":""}', 'Q' + JSON.stringify({ requestId: 'x'.repeat(65), launchToken }),
    'Q' + JSON.stringify({ requestId: 'quit_42\n', launchToken })]) {
    assert.deepEqual(quitReply(await deliver(invalid)), [{ status: 'reject', errorCode: 'invalid-request' }]);
  }
  for (const token of [undefined, null, '', 42, {}, [], true, 'bad token', 'x'.repeat(65), '\n',
    'check\n', 'check\r', 'check\r\n', ' check', 'check ', '查询', 'check\u0080']) {
    assert.deepEqual(quitReply(await deliver('Q' + JSON.stringify({ requestId: 'quit_42', launchToken: token }))),
      [{ replyTo: 'quit_42', status: 'reject', errorCode: 'invalid-request' }]);
  }
  assert.deepEqual(quitReply(await deliver('Q' + JSON.stringify({ requestId: 'quit_42', launchToken,
    padding: 'x'.repeat(18000) }))), [{ replyTo: 'quit_42', status: 'reject', errorCode: 'invalid-request' }]);
  assert.deepEqual(nativeStorageSnapshot(), beforeQuit);
  assert.deepEqual(resumedStates.at(-1), quitSnapshot);
  assert.equal(terminateCalls, expectedTerminateCalls);
  // Matching token permits owned idle cleanup even with a persisted unfinished manifest.
  await checkQuit();

  // User takeover is permanent, including interaction before first-page capture.
  restartOwnedReceiver();
  receiver.preserveForUser();
  receiver.captureLaunchContext(launchToken);
  assert.deepEqual(await handshake(), [unownedHandshake]);
  await checkQuit('not-owner');
  receiver.stop();
  receiver = new InterconnectThemeReceiver();
  receiver.preserveForUser();
  receiver.start();
  receiver.captureLaunchContext(launchToken);
  assert.deepEqual(await handshake(), [unownedHandshake]);
  await checkQuit('not-owner');

  // Stop/start and every uncertain connection lifecycle revoke rather than restoring ownership.
  for (const reset of ['restart', 'onclose', 'onerror', 'reconnected']) {
    restartOwnedReceiver();
    if (reset === 'restart') { receiver.stop(); receiver.start(); }
    else if (reset === 'reconnected') connection.onopen({ isReconnected: true });
    else connection[reset]({ code: 500 });
    receiver.captureLaunchContext(launchToken);
    assert.deepEqual(await handshake(), [unownedHandshake]);
    await checkQuit('not-owner');
  }

  // Valid T begin/replays mark this session busy, not merely loading disk state.
  const manifestState = JSON.parse(beforeQuit.text.get(transferStateUri));
  const beginPacket = state => 'T' + JSON.stringify({ operation: 'begin', themeId: state.themeId,
    mode: state.mode, fileCount: state.fileCount, totalBytes: state.totalBytes });
  const partialState = { ...JSON.parse(finishedDarkState), finished: false,
    files: JSON.parse(finishedDarkState).files.map(entry => ({ ...entry, complete: false,
      receivedBitmap: '0'.repeat(Math.ceil(entry.chunkCount / 4)) })) };
  for (const state of [manifestState, partialState,
    { ...JSON.parse(finishedDarkState), finished: false },
    { ...JSON.parse(finishedDarkState), mode: 'resume', manifestReceiving: true, manifestSeen: 0 }]) {
    native.text.set(transferStateUri, JSON.stringify(state));
    restartOwnedReceiver();
    const begin = messages(await deliver(beginPacket({ ...state,
      mode: state.manifestComplete ? 'resume' : 'replace' })));
    assert(begin.some(packet => packet.operation === 'ack' && packet.itemType === 'begin'));
    assert.deepEqual(await handshake(), [unownedHandshake], 'accepted begin revokes the H ownership marker');
    assert.deepEqual(quitReply(await deliver('Q' + JSON.stringify({
      requestId: 'quit_42', launchToken: 'different_token'
    }))), [{ replyTo: 'quit_42', status: 'reject', errorCode: 'not-owner' }]);
    await checkQuit('busy');
    connection.onclose({ code: 1 });
    await checkQuit('not-owner'); // Disconnect clears activity, never restores close permission.
  }

  // Valid direct P revokes ownership and marks uploads busy; restart cannot regain it.
  native.text.set(transferStateUri, JSON.stringify(partialState));
  restartOwnedReceiver();
  const preparePacket = 'P' + JSON.stringify({ themeId: 'dark', fileIndex: 0,
    sizeBytes: coronaBytes.length, chunkSizeBytes: coronaChunkSize, chunkCount: coronaChunkCount });
  assert(messages(await deliver(preparePacket)).some(packet => packet.status === 'ready'));
  assert.deepEqual(await handshake(), [unownedHandshake], 'accepted P revokes the H ownership marker');
  await checkQuit('busy');
  receiver.stop();
  receiver.start();
  receiver.captureLaunchContext(launchToken);
  await checkQuit('not-owner');

  // A native await must not let pre-close handlers reactivate the new connection.
  // Also queue duplicates before close: they must never run in the new generation.
  for (const [packet, eventName] of [
    [beginPacket({ ...partialState, mode: 'resume' }), 'onclose'],
    [preparePacket, 'onerror']
  ]) {
    native.text.set(transferStateUri, JSON.stringify(partialState));
    native.text.set(installedUri, '[]');
    restartOwnedReceiver();
    let resumeRead;
    native.readText = options => options.uri === transferStateUri
      ? (resumeRead = () => originalReadText(options)) : originalReadText(options);
    const start = sent.length;
    connection.onmessage({ data: packet });
    await new Promise(resolve => setTimeout(resolve, 0));
    assert.equal(typeof resumeRead, 'function');
    connection.onmessage({ data: packet });
    connection.onmessage({ data: preparePacket });
    connection[eventName]({ code: 500 });
    connection.onopen({ isReconnected: true });
    resumeRead();
    await new Promise(resolve => setTimeout(resolve, 0));
    native.readText = originalReadText;
    const replies = innerSent().slice(start);
    assert.equal(replies.filter(reply => reply[0] === packet[0]).length, 1,
      'only the already-running handler may finish; stale queued duplicates are skipped');
    if (packet[0] === 'T') {
      assert.equal(replies.some(reply => reply[0] === 'P'), false, 'queued pre-close P is skipped');
      assert.equal(JSON.parse(native.text.get(transferStateUri)).manifestReceiving, true,
        'in-flight begin preserves existing durable resume behavior');
    } else {
      assert.deepEqual(JSON.parse(native.text.get(transferStateUri)), partialState);
    }
    assert.deepEqual(await queryList(), [{ msg: 'L', replyTo: 'list_42', pageIndex: 0,
      done: true, total: 0, items: [] }]);
    sent.length = 0;
    await checkQuit('not-owner');
  }

  // New replace begin can also be interrupted while awaiting the active-theme check.
  seedPriorPack();
  native.text.set(mappingsUri, '');
  native.text.set(generationsUri, JSON.stringify({ version: 1, generations: [] }));
  restartOwnedReceiver();
  let resumeActiveCheck;
  native.readText = options => options.uri === mappingsUri
    ? (resumeActiveCheck = () => originalReadText(options)) : originalReadText(options);
  connection.onmessage({ data: beginPacket({ ...JSON.parse(finishedDarkState), mode: 'replace' }) });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(typeof resumeActiveCheck, 'function');
  connection.onclose({ code: 1 });
  connection.onopen({ isReconnected: true });
  resumeActiveCheck();
  await new Promise(resolve => setTimeout(resolve, 0));
  native.readText = originalReadText;
  const replacedState = JSON.parse(native.text.get(transferStateUri));
  assert.equal(replacedState.manifestReceiving, true);
  assert.deepEqual(replacedState.files, [], 'already-started replace persists resumable state as before');
  assert.equal((await queryList())[0].done, true);
  sent.length = 0;
  await checkQuit('not-owner');

  // Interrupting P persistence must not reactivate the upload after its callback.
  native.text.set(transferStateUri, JSON.stringify(partialState));
  restartOwnedReceiver();
  const originalWriteText = native.writeText;
  let resumePersist;
  native.writeText = options => options.uri === transferStateUri
    ? originalWriteText({ ...options, success: () => { resumePersist = options.success; } })
    : originalWriteText(options);
  connection.onmessage({ data: 'P' + JSON.stringify({ themeId: 'dark', fileIndex: 0,
    sizeBytes: coronaBytes.length, chunkSizeBytes: coronaBytes.length, chunkCount: 1 }) });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(typeof resumePersist, 'function');
  connection.onerror({ code: 500 });
  connection.onopen({ isReconnected: true });
  resumePersist();
  await new Promise(resolve => setTimeout(resolve, 0));
  native.writeText = originalWriteText;
  assert.equal(JSON.parse(native.text.get(transferStateUri)).files[0].chunkCount, 1,
    'in-flight persistence is not rolled back or discarded');
  assert.equal((await queryList())[0].done, true);
  sent.length = 0;
  await checkQuit('not-owner');

  // Generation is rechecked after waiting for the shared resource lock, and
  // stop/start on the same receiver invalidates queued work just like close/error.
  const { withResourceOperation: lockOperation } = require(path.join(temporary, 'resource-overrides.js'));
  for (const reset of ['onclose', 'onerror', 'restart']) {
    native.text.set(transferStateUri, JSON.stringify(partialState));
    restartOwnedReceiver();
    let releaseLock;
    const locked = lockOperation(() => new Promise(resolve => { releaseLock = resolve; }));
    await new Promise(resolve => setTimeout(resolve, 0));
    const start = sent.length;
    const before = nativeStorageSnapshot();
    connection.onmessage({ data: beginPacket({ ...partialState, mode: 'resume' }) });
    connection.onmessage({ data: preparePacket });
    connection.onmessage({ data: quitPacket });
    await new Promise(resolve => setTimeout(resolve, 0));
    if (reset === 'restart') { receiver.stop(); receiver.start(); }
    else connection[reset]({ code: 500 });
    releaseLock();
    await locked;
    await new Promise(resolve => setTimeout(resolve, 0));
    assert.deepEqual(innerSent().slice(start), [], 'stale resource-lock waiters and queued P do not execute');
    assert.deepEqual(nativeStorageSnapshot(), before);
    assert.equal((await queryList())[0].done, true);
    sent.length = 0;
    await checkQuit('not-owner');
  }

  // onerror clears activity even if the upload UI was already in error.
  native.text.set(transferStateUri, JSON.stringify(partialState));
  restartOwnedReceiver();
  await deliver(preparePacket);
  await deliver('E{"errorCode":"interrupted"}');
  assert.equal(resumedStates.at(-1).phase, 'error');
  await checkQuit('busy');
  connection.onerror({ code: 500 });
  await checkQuit('not-owner');

  // Successful finish clears busy but cannot restore permission or close the success screen.
  seedPriorPack();
  native.text.set(mappingsUri, '');
  native.text.set(generationsUri, JSON.stringify({ version: 1, generations: [] }));
  restartOwnedReceiver();
  const finishedState = JSON.parse(finishedDarkState);
  await deliver(beginPacket({ ...finishedState, mode: 'resume' }));
  await checkQuit('busy');
  for (const [fileIndex, entry] of finishedState.files.entries())
    await deliver('T' + JSON.stringify({ operation: 'file', themeId: 'dark', fileIndex,
      relativePath: entry.relativePath, sizeBytes: entry.sizeBytes }));
  await deliver('T{"operation":"end","themeId":"dark"}');
  assert(messages(await deliver('T{"operation":"finish","themeId":"dark"}')).some(
    packet => packet.operation === 'status' && packet.status === 'ready'));
  assert.equal(resumedStates.at(-1).phase, 'success');
  assert.deepEqual(await handshake(), [unownedHandshake]);
  receiver.captureLaunchContext(launchToken);
  await checkQuit('not-owner');

  // Corrupt/unreadable persisted state and failed L cannot prevent owned idle cleanup.
  native.text.set(transferStateUri, '{invalid');
  restartOwnedReceiver();
  await checkQuit();
  restartOwnedReceiver();
  let transferStateReads = 0;
  native.readText = options => {
    if (options.uri === transferStateUri) { transferStateReads++; options.fail('unreadable', 500); }
    else if (options.uri === installedUri) options.fail('unreadable', 500);
    else originalReadText(options);
  };
  const failedList = await queryList();
  assert.deepEqual(failedList, [{ msg: 'L', replyTo: 'list_42', errorCode: 'list-failed' }]);
  sent.length = 0;
  await checkQuit();
  assert.equal(transferStateReads, 0, 'idle Q does not read or validate transfer state');
  native.readText = originalReadText;

  // Invalid T begin cannot make an otherwise idle session busy or revoke its ownership.
  restartOwnedReceiver();
  assert(messages(await deliver('T{"operation":"begin","themeId":"BAD","mode":"replace","fileCount":1,"totalBytes":1}')).some(
    packet => packet.errorCode === 'invalid-manifest'));
  await checkQuit();

  // The resource lock must be acquired before deciding to exit.
  native.text.delete(transferStateUri);
  restartOwnedReceiver();
  const { withResourceOperation } = require(path.join(temporary, 'resource-overrides.js'));
  let releaseOperation;
  const operation = withResourceOperation(() => new Promise(resolve => { releaseOperation = resolve; }));
  await new Promise(resolve => setTimeout(resolve, 0));
  const quitStart = sent.length;
  connection.onmessage({ data: quitPacket });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(sent.length, quitStart);
  assert.equal(terminateCalls, expectedTerminateCalls);
  // Even if the preceding operation leaves unfinished durable state, idle Q exits.
  native.text.set(transferStateUri, beforeQuit.text.get(transferStateUri));
  releaseOperation();
  await operation;
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.deepEqual(quitReply(innerSent().slice(quitStart)), [{ replyTo: 'quit_42', status: 'ready' }]);
  assert.equal(terminateCalls, ++expectedTerminateCalls);
  assert.equal(native.text.get(transferStateUri), beforeQuit.text.get(transferStateUri));

  // An interrupted write must drain before Q can reject, with activity/permission revoked.
  native.text.set(transferStateUri, JSON.stringify(partialState));
  restartOwnedReceiver();
  await deliver(preparePacket);
  const originalWriteArrayBuffer = native.writeArrayBuffer;
  let finishWrite;
  native.writeArrayBuffer = options => originalWriteArrayBuffer({ ...options,
    success: () => { finishWrite = options.success; } });
  const writeStart = sent.length;
  connection.onmessage({ data: `F00000000${encodeBase91(coronaBytes.slice(0, coronaChunkSize))}` });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(typeof finishWrite, 'function');
  connection.onerror({ code: 500 });
  connection.onmessage({ data: quitPacket });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(sent.length, writeStart, 'Q waits for in-flight write callback');
  assert.equal(terminateCalls, expectedTerminateCalls);
  finishWrite();
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.deepEqual(innerSent().slice(writeStart), [
    'A00000000', 'Q{"replyTo":"quit_42","status":"reject","errorCode":"not-owner"}'
  ]);
  assert.equal(terminateCalls, expectedTerminateCalls);
  assert.equal(JSON.parse(native.text.get(transferStateUri)).files[0].receivedBitmap, '1');
  native.writeArrayBuffer = originalWriteArrayBuffer;

  // Old ready callbacks cannot terminate after disconnect, reconnect or same-instance restart.
  for (const reset of ['onclose', 'onerror', 'restart', 'reconnected']) {
    restartOwnedReceiver();
    let finishStaleQuit;
    connection.send = options => { sent.push(options.data); finishStaleQuit = options.success; };
    const staleStart = sent.length;
    connection.onmessage({ data: quitPacket });
    await new Promise(resolve => setTimeout(resolve, 0));
    assert.deepEqual(quitReply(innerSent().slice(staleStart)), [{ replyTo: 'quit_42', status: 'ready' }]);
    assert.equal(typeof finishStaleQuit, 'function');
    connection.send = originalSend;
    if (reset === 'restart') { receiver.stop(); receiver.start(); }
    else if (reset !== 'reconnected') connection[reset]({ code: 500 });
    if (reset !== 'restart') connection.onopen({ isReconnected: true });
    receiver.captureLaunchContext(launchToken);
    finishStaleQuit();
    await new Promise(resolve => setTimeout(resolve, 0));
    assert.equal(terminateCalls, expectedTerminateCalls);
    assert.equal(typeof connection.onmessage, 'function', 'old ready must not stop the new receiver');
    await checkQuit('not-owner');
  }

  // User takeover while ready-send is pending must suppress termination without reconnecting.
  restartOwnedReceiver();
  let finishUserQuit;
  connection.send = options => { sent.push(options.data); finishUserQuit = options.success; };
  const takeoverStart = sent.length;
  connection.onmessage({ data: quitPacket });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.deepEqual(quitReply(innerSent().slice(takeoverStart)), [{ replyTo: 'quit_42', status: 'ready' }]);
  assert.equal(typeof finishUserQuit, 'function');
  receiver.preserveForUser();
  receiver.captureLaunchContext(launchToken);
  finishUserQuit();
  await new Promise(resolve => setTimeout(resolve, 0));
  connection.send = originalSend;
  assert.equal(terminateCalls, expectedTerminateCalls);
  assert.equal(typeof connection.onmessage, 'function', 'user takeover keeps the receiver open');
  assert.deepEqual(await handshake(), [unownedHandshake]);
  await checkQuit('not-owner');

  // Failed ready-send must not exit or poison the queue; retry can succeed.
  native.text.set(transferStateUri, finishedDarkState);
  restartOwnedReceiver();
  connection.send = options => options.fail('disconnected', 500);
  assert.deepEqual(await deliver(quitPacket), []);
  assert.equal(terminateCalls, expectedTerminateCalls);
  connection.send = originalSend;
  let acknowledgeQuit;
  connection.send = options => { sent.push(options.data); acknowledgeQuit = options.success; };
  const readyStart = sent.length;
  connection.onmessage({ data: quitPacket });
  connection.onmessage({ data: 'T{"operation":"begin","themeId":"next","mode":"replace","fileCount":1,"totalBytes":1}' });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.deepEqual(quitReply(innerSent().slice(readyStart)), [{ replyTo: 'quit_42', status: 'ready' }]);
  assert.equal(terminateCalls, expectedTerminateCalls, 'termination waits for native send success');
  let operationRan = false;
  const behindQuit = withResourceOperation(async () => { operationRan = true; });
  onTerminate = () => {
    assert.equal(operationRan, false, 'lock is still held during termination');
    assert.equal(connection.onmessage, null, 'receiver stopped before termination');
  };
  acknowledgeQuit();
  await behindQuit;
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.equal(terminateCalls, ++expectedTerminateCalls);
  assert.equal(native.text.get(transferStateUri), finishedDarkState, 'queued upload never starts after ready');
  connection.send = originalSend;
  onTerminate = () => {};

  // Q validates cold ownership itself; no persisted upload/H prerequisite is needed.
  native.text.delete(transferStateUri);
  restartOwnedReceiver();
  assert.deepEqual(quitReply(await deliver(quitPacket)), [{ replyTo: 'quit_42', status: 'ready' }]);
  assert.equal(terminateCalls, ++expectedTerminateCalls);
  receiver.stop();
  console.log('Interconnect transfer, durable protection, list metadata and ownership-safe quit tests passed.');
}
main().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => {
  fs.rmSync(temporary, { recursive: true, force: true });
});
