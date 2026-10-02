/* Host-only tests for Manager control protocol, shared slot and app sessions. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const temporary = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'resource-hook-control-')));
const REQUEST = 'internal://files/control.request';
const RESPONSE = 'internal://files/control.response';
const APP = 'ng.lst.corona';
const STATUS = 'resource-hook-status-v1';
const RELOAD = 'resource-hook-reload-v2';
const LEGACY = 'resource-hook-reload-v1';

function record(echo, fields) {
  const payload = `${echo}\n${fields.join('\t')}\n`;
  // Independent reference arithmetic rather than the parser's Math.imul.
  let hash = 2166136261n;
  for (let i = 0; i < payload.length; i++)
    hash = ((hash ^ BigInt(payload.charCodeAt(i))) * 16777619n) & 0xffffffffn;
  const text = `${payload}${hash}\n`;
  assert.ok(text.length < 256);
  return text + '\0'.repeat(256 - text.length);
}
function status(id, { state = 'running', error = 0, count = 2, pending = 0,
  operation = STATUS, app = APP, magic = 'RHST1', version = 1 } = {}) {
  return record(`${operation}\t${app}\t${id}`, [magic, version, state, error, count, pending]);
}
function reload(id, { result = 0, pending = 0, changed = 1, state = 'running', error = 0,
  count = 7, operation = RELOAD, app = APP, magic = 'RHRS2', version = 1 } = {}) {
  return record(`${operation}\t${app}\t${id}`,
    [magic, version, result, pending, changed, state, error, count]);
}
function legacy(id, version = 5, options = {}) {
  return record(`${LEGACY}\t${APP}\t${id}`, ['RHRS1', version,
    options.result ?? 0, options.pending ?? 0, options.changed ?? 1]);
}
function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
const turn = () => new Promise(resolve => setImmediate(resolve));
async function promptly(promise) {
  let timer;
  try {
    return await Promise.race([promise, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error('cancellation did not stop polling')), 500);
    })]);
  } finally { clearTimeout(timer); }
}
function statusFile(options = {}) {
  const writes = [];
  let reads = 0, request;
  return {
    writes,
    get reads() { return reads; },
    get id() { return request?.split('\t')[2].trim(); },
    async writeText(uri, text) {
      writes.push([uri, text]);
      assert.ok(text.length, 'Vela cannot write an empty placeholder');
      if (options.writeFailure === uri) throw new Error('write failed');
      if (uri === REQUEST) request = text;
    },
    async readOptionalText(uri) {
      assert.equal(uri, RESPONSE);
      reads++;
      if (options.readFailure) throw new Error('read failed');
      if (options.read) return options.read(this.id, reads);
      return status(this.id, options.status);
    }
  };
}

async function main() {
  execFileSync(path.join(root, 'manager/node_modules/.bin/tsc'), [
    path.join(root, 'manager/src/ts/module-control.ts'), '--outDir', temporary,
    '--module', 'commonjs', '--target', 'es2018', '--lib', 'es2018,dom', '--strict', '--skipLibCheck'
  ], { stdio: 'inherit' });
  const modulePath = path.join(temporary, 'module-control.js');
  function loadBundleCopy() {
    delete require.cache[require.resolve(modulePath)];
    return require(modulePath);
  }
  const api = loadBundleCopy();
  const { sendReloadSignal, parseReloadResult, parseModuleStatus, waitForReload,
    waitForReloadOutcome, queryModuleStatus, getInitialModuleStatus,
    initializeModuleControlSession: initialize, destroyModuleControlSession: destroy,
    withModuleControlOperation: operation } = api;
  const running = (count = 2, refreshPending = false) => ({
    state: 'running', configError: 0, activeRuleCount: count, refreshPending
  });
  const configError = (error = -2103, count = 2, refreshPending = false) => ({
    state: 'config_error', configError: error, activeRuleCount: count, refreshPending
  });

  // Full text framing, exact operation/app/ID and checksum; native padding is optional.
  for (const [parse, valid] of [[parseModuleStatus, status('id')], [parseReloadResult, reload('id')]]) {
    assert.ok(parse(valid, 'id'));
    assert.equal(parse(valid, 'stale'), null);
    assert.equal(parse(valid, ''), null);
    assert.equal(parse(valid, 'bad/id'), null);
    assert.equal(parse(valid, 'x'.repeat(65)), null);
    const end = valid.indexOf('\0');
    const body = valid.slice(0, end);
    for (let length = 0; length < end; length++) {
      const short = valid.slice(0, length);
      assert.equal(parse(short, 'id'), null, `incomplete body: ${length}`);
      assert.equal(parse(short + '\0'.repeat(256 - length), 'id'), null,
        `zero padding cannot repair an incomplete body: ${length}`);
    }
    for (let length = end; length <= valid.length; length++)
      assert.deepEqual(parse(valid.slice(0, length), 'id'), parse(valid, 'id'),
        `complete body with optional padding: ${length}`);
    for (const compatible of [body, body + '\0', valid + '\0', valid.slice(0, -1) + 'x',
      valid.slice(0, end + 3) + 'x' + valid.slice(end + 4)])
      assert.deepEqual(parse(compatible, 'id'), parse(valid, 'id'),
        'native padding does not determine text-payload validity');
    const checksumStart = valid.lastIndexOf('\n', end - 2) + 1;
    for (const bad of [
      '', 'pending\tid\n', body + 'x', body + '\n', body + body,
      body.slice(0, 5) + '\0' + body.slice(5),
      valid.replace('\n', '\r\n').slice(0, 256),
      valid.replace('ng.lst.corona', 'ng.lst.coronb'), valid.replace('id', 'iD'),
      valid.slice(0, checksumStart) + (valid[checksumStart] === '0' ? '1' : '0') + valid.slice(checksumStart + 1),
      valid.slice(0, checksumStart) + '4294967296\n' + '\0'.repeat(256 - checksumStart - 11),
      valid.slice(0, checksumStart) + '0' + valid.slice(checksumStart, -1),
      valid.replace(/\n\0/, '\0\0'), valid.replace('\t1\t', '\t中\t'),
    ]) assert.equal(parse(bad, 'id'), null, JSON.stringify(bad));
  }
  assert.deepEqual(parseModuleStatus(status('x'), 'x'), running());
  assert.deepEqual(parseModuleStatus(status('x', { count: 0 }), 'x'), running(0));
  assert.deepEqual(parseModuleStatus(status('x', { count: 256, pending: 1 }), 'x'), running(256, true));
  assert.deepEqual(parseModuleStatus(status('x', { state: 'config_error', error: -2147483648 }), 'x'),
    configError(-2147483648));
  const maxId = 'A'.repeat(64);
  assert.ok(parseModuleStatus(status(maxId), maxId));
  assert.ok(parseReloadResult(reload(maxId), maxId));
  for (const options of [
    { operation: RELOAD }, { operation: 'resource-hook-status-v2' }, { app: 'other.app' },
    { magic: 'RHST2' }, { version: 0 }, { version: 2 }, { state: 'stopped' },
    { state: 'config_error', error: 0 }, { state: 'config_error', error: 1 },
    { state: 'config_error', error: -2147483649 }, { error: -1 }, { error: 2147483648 },
    ...['+0', '-0', '00', '1.2', '1e2', '', ' ', 'NaN'].map(error => ({ error })),
    ...[-1, 257, 4294967296, '00', '+1', '1.5', '1e2', ''].map(count => ({ count })),
    ...[-1, 2, '01', 'true', ''].map(pending => ({ pending })),
  ]) assert.equal(parseModuleStatus(status('x', options), 'x'), null, JSON.stringify(options));
  for (const fields of [
    ['RHST1', 1, 'running', 0, 1], ['RHST1', 1, 'running', 0, 1, 0, 'extra'],
  ]) assert.equal(parseModuleStatus(record(`${STATUS}\t${APP}\tx`, fields), 'x'), null);
  assert.equal(parseModuleStatus(record(`${STATUS}\tx`, ['RHST1', 1, 'running', 0, 1, 0]), 'x'), null);
  assert.equal(parseModuleStatus(record(`${STATUS}\t${APP}\tx\textra`, ['RHST1', 1, 'running', 0, 1, 0]), 'x'), null);

  assert.deepEqual(parseReloadResult(reload('x'), 'x'), {
    version: 1, result: 0, pending: false, changed: 1, status: running(7)
  });
  assert.deepEqual(parseReloadResult(reload('x', { result: 1, pending: 1, changed: 4294967295, count: 256 }), 'x'), {
    version: 1, result: 1, pending: true, changed: 4294967295, status: running(256, true)
  });
  assert.equal(parseReloadResult(reload('x', { result: -2147483648, state: 'config_error', error: -2103 }), 'x').result,
    -2147483648);
  // Reload errors (e.g. fonts) need not be configuration errors.
  assert.ok(parseReloadResult(reload('x', { result: -2011, pending: 1 }), 'x'));
  for (const options of [
    { operation: STATUS }, { operation: LEGACY }, { app: 'other.app' }, { magic: 'RHRS1' },
    { version: 2 }, { result: 2 }, { result: -2147483649 }, { result: 1, pending: 0 },
    ...['+0', '-0', '00', '1e0', 'NaN', '', '1.5'].map(result => ({ result })),
    ...[-1, 4294967296, '00', '+1', '1.5', '1e2', ''].map(changed => ({ changed })),
    ...[-1, 2, '01', 'true'].map(pending => ({ pending })),
    { state: 'unknown' }, { state: 'running', error: -2103 },
    { state: 'config_error', error: 0 }, { state: 'config_error', error: 2147483647 },
    { state: 'config_error', error: -2147483649 }, { count: 257 }, { count: -1 },
  ]) assert.equal(parseReloadResult(reload('x', options), 'x'), null, JSON.stringify(options));
  for (const fields of [
    ['RHRS2', 1, 0, 0, 0], ['RHRS2', 1, 0, 0, 0, 'running', 0, 2, 'extra'],
  ]) assert.equal(parseReloadResult(record(`${RELOAD}\t${APP}\tx`, fields), 'x'), null);
  for (const version of [5, 6]) {
    assert.equal(parseReloadResult(legacy('x', version), 'x'), null, 'legacy cannot satisfy v2');
    assert.deepEqual(parseReloadResult(legacy('x', version), 'x', LEGACY), {
      version, result: 0, pending: false, changed: 1, status: null
    });
    assert.equal(parseReloadResult(legacy('x', version), 'stale', LEGACY), null);
  }
  assert.equal(parseReloadResult(legacy('x', 4), 'x', LEGACY), null);
  assert.equal(parseReloadResult(reload('x'), 'x', LEGACY), null);
  assert.equal(parseReloadResult(record(`${STATUS}\t${APP}\tx`, ['RHRS1', 5, 0, 0, 1]), 'x', STATUS), null);
  assert.equal(parseReloadResult(record(`${RELOAD}\t${APP}\tx`, ['RHRS1', 5, 0, 0, 1]), 'x'), null);
  assert.equal(parseReloadResult(record(`${LEGACY}\tx`, ['RHRS1', 5, 0, 0, 1]), 'x', LEGACY), null);

  // Writer migrates both paths, sends v2, validates IDs before doing any I/O.
  initialize();
  {
    const writes = [];
    await sendReloadSignal('retry-7', { async writeText(uri, text) { writes.push([uri, text]); } });
    assert.deepEqual(writes, [[RESPONSE, 'pending\tretry-7\n'], [REQUEST, `${RELOAD}\t${APP}\tretry-7\n`]]);
  }
  for (const id of ['', 'bad revision', 'a/b', 'a\nb', 'a\tb', '中', 'x'.repeat(65)]) {
    await assert.rejects(sendReloadSignal(id, {
      async writeText() { assert.fail('invalid revision wrote a file'); }
    }), /重载版本标识无效/);
  }
  {
    const writes = [];
    await sendReloadSignal('optional', {
      async writeText(uri, text) {
        writes.push([uri, text]);
        if (uri === RESPONSE) throw new Error('response unavailable');
      }
    });
    assert.equal(writes[1][0], REQUEST);
  }
  await assert.rejects(sendReloadSignal('write-fail', {
    async writeText(uri) { if (uri === REQUEST) throw new Error('request failed'); }
  }), /request failed/);

  // Queries: finite polling, fresh IDs, preparation required, ignore mismatches.
  initialize();
  const healthyFile = statusFile();
  assert.deepEqual(await queryModuleStatus(healthyFile, 1, 0), {
    status: running(), reason: 'response', message: '模块运行中'
  });
  assert.equal(healthyFile.writes[0][0], RESPONSE);
  assert.equal(healthyFile.writes[1][0], REQUEST);
  const firstId = healthyFile.id;
  assert.equal(healthyFile.writes[0][1], `pending\t${firstId}\n`);
  await queryModuleStatus(healthyFile, 1, 0);
  assert.notEqual(healthyFile.id, firstId);
  const stale = statusFile({ read: (id, polls) => polls === 1 ? status('stale') :
    polls === 2 ? reload(id) : polls === 3 ? status(id).split('\0', 1)[0].slice(0, -1) : status(id) });
  assert.equal((await queryModuleStatus(stale, 5, 0)).reason, 'response');
  assert.equal(stale.reads, 4);
  for (const unavailable of [null, 'pending\tx\n', status('stale')]) {
    const file = statusFile({ read: () => unavailable });
    assert.equal((await queryModuleStatus(file, undefined, 0)).reason, 'timeout');
    assert.equal(file.reads, 5, 'default status poll budget');
  }
  for (const attempts of [0, -1, NaN, Infinity]) {
    const file = statusFile();
    assert.equal((await queryModuleStatus(file, attempts, 0)).reason, 'timeout');
    assert.equal(file.reads, 0);
  }
  assert.deepEqual((await queryModuleStatus(statusFile({ status: { state: 'config_error', error: -2103, count: 256 } }), 1, 0)).status,
    configError(-2103, 256));
  assert.equal((await queryModuleStatus(statusFile({ status: { pending: 1 } }), 1, 0)).status.refreshPending, true);
  const readFail = statusFile({ readFailure: true });
  assert.equal((await queryModuleStatus(readFail, 5, 0)).reason, 'read_error');
  assert.equal(readFail.reads, 1);
  for (const uri of [RESPONSE, REQUEST]) {
    const file = statusFile({ writeFailure: uri });
    assert.equal((await queryModuleStatus(file, 5, 0)).reason, 'write_error');
    assert.equal(file.reads, 0);
    assert.equal(file.writes.length, uri === RESPONSE ? 1 : 2);
  }

  // A null-terminated text API returns only the prefix before the first NUL.
  initialize();
  {
    const textOnly = statusFile({ read: id => status(id).split('\0', 1)[0] });
    assert.deepEqual((await getInitialModuleStatus(textOnly)).status, running());
    assert.equal(textOnly.reads, 1, 'a text-only status reply must not exhaust the poll budget');
    const errorText = statusFile({ read: id => status(id,
      { state: 'config_error', error: -2103, count: 9 }).split('\0', 1)[0] });
    assert.deepEqual((await queryModuleStatus(errorText, 1, 0)).status, configError(-2103, 9));
    let reads = 0;
    const reply = await waitForReloadOutcome('text-only', { async readOptionalText(uri) {
      assert.equal(uri, RESPONSE);
      reads++;
      return reload('text-only', { count: 42 }).split('\0', 1)[0];
    } }, 1, 0);
    assert.deepEqual(reply, { successful: true, message: '重载完成，更新 1 项资源', status: running(42) });
    assert.equal(reads, 1, 'a text-only reload receipt is accepted immediately');
    assert.deepEqual(await waitForReloadOutcome('text-error', { async readOptionalText() {
      return reload('text-error', { result: -2103, state: 'config_error', error: -2103,
        count: 9 }).split('\0', 1)[0];
    } }, 1, 0), { successful: false, message: '模块拒绝重载（-2103）',
      reason: 'rejected', status: configError(-2103, 9) });
    assert.deepEqual((await getInitialModuleStatus(textOnly)).status, configError(-2103, 9));
    assert.equal(textOnly.reads, 1, 'cached text-only receipts do not trigger another status query');
  }

  // Reload waits: accurate native count, no additional query, pending != absent.
  initialize();
  {
    let reads = 0;
    const file = { async readOptionalText(uri) {
      assert.equal(uri, RESPONSE);
      reads++;
      return reads === 1 ? reload('other') : reads === 2 ? status('r') : reload('r', { count: 256, changed: 3 });
    } };
    assert.deepEqual(await waitForReloadOutcome('r', file, 4, 0), {
      successful: true, message: '重载完成，更新 3 项资源', status: running(256)
    });
    assert.equal(reads, 3);
  }
  assert.equal(await waitForReload('r', { async readOptionalText() { return reload('r', { changed: 0 }); } }, 1, 0),
    '模块已响应，重载完成');
  assert.deepEqual(await waitForReloadOutcome('r', {
    async readOptionalText() { return reload('r', { result: -2103, state: 'config_error', error: -2103, count: 256 }); }
  }, 1, 0), { successful: false, message: '模块拒绝重载（-2103）', reason: 'rejected', status: configError(-2103, 256) });
  assert.deepEqual(await waitForReloadOutcome('r', {
    async readOptionalText() { return reload('r', { result: 1, pending: 1, count: 17 }); }
  }, 2, 0), { successful: false, message: '模块已响应，资源刷新仍待完成', reason: 'pending', status: running(17, true) });
  for (const response of [null, status('r'), reload('stale', { pending: 1 }), legacy('r')]) {
    let reads = 0;
    assert.deepEqual(await waitForReloadOutcome('r', { async readOptionalText() { reads++; return response; } }, 2, 0),
      { successful: false, message: '未收到模块响应', reason: 'timeout', status: null });
    assert.equal(reads, 2);
  }
  {
    let reads = 0;
    const pendingThenMissing = await waitForReloadOutcome('r', { async readOptionalText() {
      return ++reads === 1 ? reload('r', { pending: 1, count: 5 }) : null;
    } }, 3, 0);
    assert.equal(pendingThenMissing.reason, 'pending');
    assert.deepEqual(pendingThenMissing.status, running(5, true));
  }
  {
    let reads = 0;
    const complete = await waitForReloadOutcome('r', { async readOptionalText() {
      return reload('r', ++reads === 1 ? { result: 1, pending: 1, count: 18 } : { changed: 0, count: 18 });
    } }, 3, 0);
    assert.equal(complete.successful, true);
    assert.deepEqual(complete.status, running(18));
    assert.equal(reads, 2);
  }
  {
    let reads = 0;
    assert.equal((await waitForReloadOutcome('default', { async readOptionalText() { reads++; return null; } }, undefined, 0)).reason, 'timeout');
    assert.equal(reads, 30, 'default reload poll budget');
    assert.equal((await waitForReloadOutcome('r', { async readOptionalText() { return legacy('r', 5, { pending: 1 }); } }, 1, 0, LEGACY)).reason, 'pending');
  }
  initialize();
  assert.deepEqual(await waitForReloadOutcome('r', { async readOptionalText() { throw new Error('read failed'); } }, 3, 0),
    { successful: false, message: '无法读取模块响应', reason: 'read_error', status: null });
  {
    let reads = 0;
    const failure = await waitForReloadOutcome('r', { async readOptionalText() {
      if (++reads === 1) return reload('r', { pending: 1 });
      throw new Error('read failed');
    } }, 3, 0);
    assert.equal(failure.reason, 'read_error');
    assert.deepEqual(failure.status, running(7, true));
  }
  assert.equal((await waitForReloadOutcome('r', {
    async readOptionalText() { return legacy('r', 6); }
  }, 1, 0, LEGACY)).successful, true);

  // Session-once promise/result, page recreation gets latest reload status.
  initialize();
  {
    const file = statusFile();
    const initial = getInitialModuleStatus(file);
    assert.equal(getInitialModuleStatus(file), initial, 'concurrent first calls share promise');
    const outcome = await initial;
    assert.equal(await getInitialModuleStatus(file), outcome);
    assert.equal(file.writes.length, 2);
    assert.equal(file.reads, 1);
    const error = await waitForReloadOutcome('r', { async readOptionalText() {
      return reload('r', { result: -2103, state: 'config_error', error: -2103, count: 88 });
    } }, 1, 0);
    assert.deepEqual((await getInitialModuleStatus(file)).status, error.status);
    await waitForReloadOutcome('r2', { async readOptionalText() { return reload('r2', { pending: 1, count: 123 }); } }, 1, 0);
    assert.deepEqual((await getInitialModuleStatus(file)).status, running(123, true));
    await waitForReloadOutcome('r3', { async readOptionalText() { return reload('r3', { changed: 0, count: 0 }); } }, 1, 0);
    assert.deepEqual((await getInitialModuleStatus(file)).status, running(0));
    await waitForReloadOutcome('absent', { async readOptionalText() { return null; } }, 1, 0);
    assert.deepEqual(await getInitialModuleStatus(file), {
      status: null, reason: 'timeout', message: '未收到模块响应'
    }, 'unmatched timeout must supersede the old running status');
    assert.equal(file.writes.length, 2, 'reload result updates require no extra status request');
    assert.equal(file.reads, 1, 'no periodic querying');
  }
  {
    // A previously green initial status must not reappear after a reload timeout.
    initialize();
    const file = statusFile();
    assert.deepEqual((await getInitialModuleStatus(file)).status, running());
    const timeout = await waitForReloadOutcome('offline', {
      async readOptionalText() { return null; }
    }, 2, 0);
    assert.equal(timeout.reason, 'timeout');
    const cached = await getInitialModuleStatus(file);
    assert.deepEqual(cached, { status: null, reason: 'timeout', message: timeout.message });
    assert.equal(await getInitialModuleStatus(file), cached);
    assert.equal(file.writes.length, 2, 'returning homepage must not query again');
    assert.equal(file.reads, 1);

    // A valid pending receipt proves online even if subsequent polls are absent.
    let reloadReads = 0;
    const pending = await waitForReloadOutcome('online-pending', {
      async readOptionalText() {
        return ++reloadReads === 1 ? reload('online-pending', { pending: 1, count: 19 }) : null;
      }
    }, 2, 0);
    assert.equal(pending.reason, 'pending');
    assert.deepEqual((await getInitialModuleStatus(file)).status, running(19, true));
    assert.equal((await getInitialModuleStatus(file)).reason, 'response');
    assert.equal(file.writes.length, 2);
    assert.equal(file.reads, 1);
  }
  {
    // Exercise the once-only timeout cache without spending four real seconds.
    initialize();
    const realSetTimeout = global.setTimeout;
    const file = statusFile({ read: () => null });
    global.setTimeout = (callback, _delay) => realSetTimeout(callback, 0);
    try {
      const outcome = await getInitialModuleStatus(file);
      assert.equal(outcome.reason, 'timeout');
      assert.equal(await getInitialModuleStatus(file), outcome);
      assert.equal(file.writes.length, 2);
      assert.equal(file.reads, 5);
    } finally { global.setTimeout = realSetTimeout; }
  }
  for (const options of [{ readFailure: true }, { writeFailure: REQUEST }]) {
    initialize();
    const file = statusFile(options);
    const outcome = await getInitialModuleStatus(file);
    assert.equal(await getInitialModuleStatus(file), outcome);
    assert.equal(file.writes.length, 2, 'initial I/O failures cached too');
  }
  initialize();
  const resetFile = statusFile();
  await getInitialModuleStatus(resetFile);
  const oldId = resetFile.id;
  initialize();
  await getInitialModuleStatus(resetFile);
  assert.notEqual(resetFile.id, oldId);
  assert.equal(resetFile.writes.length, 4);

  // Serialized slot: a status query waits behind the full reload send + wait.
  initialize();
  {
    const release = deferred(), entered = deferred(), trace = [];
    const reloadFile = {
      async writeText(uri, text) { trace.push([uri, text]); },
      async readOptionalText() { entered.resolve(); await release.promise; return reload('queued', { count: 42 }); }
    };
    const first = operation(async () => {
      await sendReloadSignal('queued', reloadFile);
      return waitForReloadOutcome('queued', reloadFile, 1, 0);
    });
    await entered.promise;
    const secondFile = statusFile();
    const second = queryModuleStatus(secondFile, 1, 0);
    const third = operation(async () => { trace.push(['third']); return 3; });
    await turn();
    assert.equal(secondFile.writes.length, 0);
    assert.equal(trace.length, 2, 'send and wait keep one reservation without deadlocking');
    release.resolve();
    assert.equal((await first).successful, true);
    assert.equal((await second).reason, 'response');
    assert.equal(await third, 3);
    assert.equal(trace[2][0], 'third');
  }
  await assert.rejects(operation(async () => { throw new Error('queue failure'); }), /queue failure/);
  assert.equal(await operation(async () => 99), 99, 'a failed operation does not poison the queue');
  {
    const release = deferred(), entered = deferred();
    const firstFile = statusFile({ read: async id => { entered.resolve(); await release.promise; return status(id); } });
    const first = queryModuleStatus(firstFile, 1, 0);
    await entered.promise;
    const secondFile = statusFile();
    const second = queryModuleStatus(secondFile, 1, 0);
    await turn();
    assert.equal(secondFile.writes.length, 0);
    release.resolve();
    await first; await second;
    assert.notEqual(firstFile.id, secondFile.id);
  }

  // Destroy cancels sleeping waits immediately and forbids new requests.
  initialize();
  {
    const file = statusFile({ read: () => null });
    const pending = queryModuleStatus(file, 5, 100000);
    await turn();
    assert.equal(file.reads, 1);
    destroy();
    assert.equal((await promptly(pending)).reason, 'cancelled');
    assert.equal(file.reads, 1);
    assert.equal((await getInitialModuleStatus(file)).reason, 'cancelled');
    assert.equal((await queryModuleStatus(file, 1, 0)).reason, 'cancelled');
    await assert.rejects(sendReloadSignal('destroyed', file), /会话已结束/);
    assert.equal(file.writes.length, 2);
  }
  initialize();
  {
    let reads = 0;
    const pending = waitForReloadOutcome('r', { async readOptionalText() { reads++; return reload('r', { pending: 1 }); } }, 5, 100000);
    await turn();
    destroy();
    const cancelledWait = await promptly(pending);
    assert.equal(cancelledWait.successful, false);
    assert.equal(cancelledWait.reason, 'cancelled', 'teardown is neither a pending timeout nor an absent response');
    assert.deepEqual(cancelledWait.status, running(7, true));
    assert.equal(reads, 1);
  }

  initialize();
  {
    let reads = 0;
    const waiting = waitForReloadOutcome('absent', {
      async readOptionalText() { reads++; return null; }
    }, 5, 100000);
    await turn();
    destroy();
    assert.deepEqual(await promptly(waiting), {
      successful: false, status: null, reason: 'cancelled', message: '模块控制会话已结束'
    });
    assert.equal(reads, 1);
  }

  // In-flight reads cannot contaminate a new session; the old I/O retains slot.
  initialize();
  {
    const entered = deferred(), release = deferred();
    const oldFile = statusFile({ read: async id => { entered.resolve(); await release.promise; return status(id, { count: 250 }); } });
    const old = getInitialModuleStatus(oldFile);
    await entered.promise;
    destroy(); initialize();
    const newFile = statusFile({ status: { count: 1 } });
    const fresh = getInitialModuleStatus(newFile);
    await turn();
    assert.equal(newFile.writes.length, 0, 'session reset does not overlap outstanding old I/O');
    release.resolve();
    assert.equal((await old).reason, 'cancelled');
    assert.deepEqual((await fresh).status, running(1));
    assert.deepEqual((await getInitialModuleStatus(newFile)).status, running(1));
  }
  for (const rejecting of [false, true]) {
    initialize();
    const entered = deferred(), release = deferred();
    const old = waitForReloadOutcome('old', { async readOptionalText() {
      entered.resolve(); await release.promise;
      if (rejecting) throw new Error('late read failure');
      return reload('old', { count: 250 });
    } }, 1, 0);
    await entered.promise;
    destroy(); initialize();
    const newFile = statusFile({ status: { count: 4 } });
    await getInitialModuleStatus(newFile);
    release.resolve();
    const cancelledReload = await old;
    assert.equal(cancelledReload.status, null);
    assert.equal(cancelledReload.reason, 'cancelled');
    assert.deepEqual((await getInitialModuleStatus(newFile)).status, running(4));
  }

  // Destroy between placeholder and request prevents stale write, even if I/O rejects.
  for (const rejecting of [false, true]) {
    initialize();
    const entered = deferred(), release = deferred(), writes = [];
    const file = {
      async writeText(uri) {
        writes.push(uri); entered.resolve(); await release.promise;
        if (rejecting) throw new Error('late write failure');
      },
      async readOptionalText() { assert.fail('cancelled query read'); }
    };
    const old = queryModuleStatus(file, 1, 0);
    await entered.promise;
    destroy(); initialize(); release.resolve();
    assert.equal((await old).reason, 'cancelled');
    assert.deepEqual(writes, [RESPONSE]);
  }
  initialize();
  {
    const entered = deferred(), release = deferred(), writes = [];
    const file = { async writeText(uri) {
      writes.push(uri); entered.resolve(); await release.promise;
    } };
    const old = sendReloadSignal('old', file);
    const rejected = assert.rejects(old, /会话已结束/);
    await entered.promise;
    destroy(); initialize(); release.resolve(); await rejected;
    assert.deepEqual(writes, [RESPONSE]);
  }

  // Old queued callbacks are not invoked; queue resumes for the fresh session.
  initialize();
  {
    const entered = deferred(), release = deferred();
    const first = operation(async () => { entered.resolve(); await release.promise; });
    await entered.promise;
    let invoked = false;
    const stale = operation(async () => { invoked = true; });
    const rejected = assert.rejects(stale, /会话已结束/);
    const staleQueryFile = statusFile();
    const staleQuery = queryModuleStatus(staleQueryFile, 1, 0);
    destroy(); initialize();
    const fresh = operation(async () => 123);
    release.resolve();
    await first; await rejected;
    assert.equal(invoked, false);
    assert.equal((await staleQuery).reason, 'cancelled');
    assert.equal(staleQueryFile.writes.length, 0);
    assert.equal(await fresh, 123);
  }

  // A stale callback finishing send after app reset cannot start a wait in the NEW session.
  initialize();
  {
    const entered = deferred(), release = deferred();
    let reads = 0;
    const file = {
      async writeText(uri) { if (uri === REQUEST) { entered.resolve(); await release.promise; } },
      async readOptionalText() { reads++; return reload('stale', { count: 256 }); }
    };
    const old = operation(async () => {
      await sendReloadSignal('stale', file);
      return waitForReloadOutcome('stale', file, 1, 0);
    });
    await entered.promise;
    destroy(); initialize(); release.resolve();
    const cancelledReload = await old;
    assert.equal(cancelledReload.successful, false);
    assert.equal(cancelledReload.reason, 'cancelled');
    assert.equal(reads, 0);
    const freshFile = statusFile();
    assert.deepEqual((await getInitialModuleStatus(freshFile)).status, running());
  }
  // Vela app/page bundles embed separate module copies but share globalThis.
  {
    const app = api;
    app.initializeModuleControlSession();
    const page = loadBundleCopy();
    assert.notEqual(page.getInitialModuleStatus, app.getInitialModuleStatus);
    const entered = deferred(), release = deferred();
    const file = statusFile({ read: async id => {
      entered.resolve(); await release.promise; return status(id);
    } });
    const first = page.getInitialModuleStatus(file);
    await entered.promise;
    const recreatedPage = loadBundleCopy();
    assert.notEqual(recreatedPage.getInitialModuleStatus, page.getInitialModuleStatus);
    assert.equal(recreatedPage.getInitialModuleStatus(file), first, 'bundle copies share the in-flight initial promise');
    release.resolve();
    const initial = await first;
    assert.equal(await recreatedPage.getInitialModuleStatus(file), initial);
    assert.equal(file.writes.length, 2, 'page rebundling must not repeat the initial query');
    assert.equal(file.reads, 1);

    await recreatedPage.waitForReloadOutcome('absent', { async readOptionalText() { return null; } }, 1, 0);
    assert.equal((await page.getInitialModuleStatus(file)).reason, 'timeout', 'timeout cache updates cross bundles');
    assert.equal((await app.getInitialModuleStatus(file)).status, null);
    await app.waitForReloadOutcome('pending', {
      async readOptionalText() { return reload('pending', { pending: 1, count: 99 }); }
    }, 1, 0);
    assert.deepEqual((await recreatedPage.getInitialModuleStatus(file)).status, running(99, true));
    assert.equal(file.writes.length, 2);
    assert.equal(file.reads, 1);
    app.destroyModuleControlSession();
  }
  {
    const app = api, page = loadBundleCopy();
    app.initializeModuleControlSession();
    const file = statusFile({ read: () => null });
    const waiting = page.queryModuleStatus(file, 5, 100000);
    await turn();
    assert.equal(file.reads, 1);
    app.destroyModuleControlSession();
    assert.equal((await promptly(waiting)).reason, 'cancelled', 'app teardown cancels a different page module copy');
    const recreatedPage = loadBundleCopy();
    assert.equal((await recreatedPage.getInitialModuleStatus(file)).reason, 'cancelled', 'new bundles cannot resurrect a destroyed session');
    assert.equal(file.writes.length, 2);
    assert.equal(file.reads, 1);

    // Fresh IDs remain unique across bundles and sessions even at the same clock tick.
    const realNow = Date.now;
    Date.now = () => 123456789;
    try {
      app.initializeModuleControlSession();
      const firstFile = statusFile();
      await page.getInitialModuleStatus(firstFile);
      const sameSessionFile = statusFile();
      await recreatedPage.queryModuleStatus(sameSessionFile, 1, 0);
      assert.notEqual(sameSessionFile.id, firstFile.id);
      app.destroyModuleControlSession();
      app.initializeModuleControlSession();
      const nextSessionFile = statusFile();
      await loadBundleCopy().getInitialModuleStatus(nextSessionFile);
      assert.notEqual(nextSessionFile.id, firstFile.id);
      assert.notEqual(nextSessionFile.id, sameSessionFile.id);
    } finally { Date.now = realNow; }
    app.destroyModuleControlSession();
  }
  {
    const app = api, page = loadBundleCopy(), recreatedPage = loadBundleCopy();
    app.initializeModuleControlSession();
    const entered = deferred(), release = deferred(), trace = [];
    const firstFile = {
      async writeText(uri, text) { trace.push([uri, text]); },
      async readOptionalText() {
        entered.resolve(); await release.promise; return reload('cross-bundle', { count: 42 });
      }
    };
    const first = app.withModuleControlOperation(async () => {
      await page.sendReloadSignal('cross-bundle', firstFile);
      return recreatedPage.waitForReloadOutcome('cross-bundle', firstFile, 1, 0);
    });
    await entered.promise;
    const queryFile = statusFile();
    const query = recreatedPage.queryModuleStatus(queryFile, 1, 0);
    const nextFile = {
      async writeText(uri, text) {
        assert.equal(queryFile.reads, 1, 'queued cross-bundle status query precedes the next reload');
        trace.push([uri, text]);
      },
      async readOptionalText() { return reload('next-bundle'); }
    };
    const next = page.withModuleControlOperation(async () => {
      await app.sendReloadSignal('next-bundle', nextFile);
      return recreatedPage.waitForReloadOutcome('next-bundle', nextFile, 1, 0);
    });
    await turn();
    assert.equal(queryFile.writes.length, 0, 'other bundle query cannot overlap reload send + wait');
    assert.equal(trace.length, 2, 'other bundle reload cannot overlap either');
    release.resolve();
    assert.equal((await first).successful, true);
    assert.equal((await query).reason, 'response');
    assert.equal((await next).successful, true);
    assert.equal(trace.length, 4);
    app.destroyModuleControlSession();
  }
  {
    // Queue and active-operation ownership survive app reset across module copies.
    const app = api, page = loadBundleCopy(), recreatedPage = loadBundleCopy();
    app.initializeModuleControlSession();
    const entered = deferred(), release = deferred();
    let reads = 0;
    const file = {
      async writeText(uri) { if (uri === REQUEST) { entered.resolve(); await release.promise; } },
      async readOptionalText() { reads++; return reload('old-bundle', { count: 256 }); }
    };
    const old = page.withModuleControlOperation(async () => {
      await app.sendReloadSignal('old-bundle', file);
      return recreatedPage.waitForReloadOutcome('old-bundle', file, 1, 0);
    });
    await entered.promise;
    app.destroyModuleControlSession();
    app.initializeModuleControlSession();
    const freshFile = statusFile({ status: { count: 3 } });
    const fresh = loadBundleCopy().getInitialModuleStatus(freshFile);
    await turn();
    assert.equal(freshFile.writes.length, 0, 'new session/bundle still waits for old in-flight transport I/O');
    release.resolve();
    assert.equal((await old).reason, 'cancelled', 'cross-bundle waiter inherits the old reservation owner');
    assert.equal(reads, 0);
    assert.deepEqual((await fresh).status, running(3));
    assert.deepEqual((await app.getInitialModuleStatus(freshFile)).status, running(3));
    app.destroyModuleControlSession();
  }
  destroy();
  console.log('Module control parsers, v2 reloads, status I/O, serialized slot, once/cache, cancellation and isolated-bundle sharing passed.');
}

main().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => {
  fs.rmSync(temporary, { recursive: true, force: true });
});
