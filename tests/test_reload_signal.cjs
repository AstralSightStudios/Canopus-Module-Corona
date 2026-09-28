/* Host tests for the Manager's reload signal writer. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const temporary = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'resource-hook-reload-')));

function makeResponse(revision, { version = 5, result = 0, pending = 0, changed = 1 } = {}) {
  const signal = `resource-hook-reload-v1\tng.lst.corona\t${revision}\n`;
  const status = `RHRS1\t${version}\t${result}\t${pending}\t${changed}`;
  const payload = `${signal}${status}\n`;
  let hash = 2166136261;
  for (let i = 0; i < payload.length; i++)
    hash = Math.imul(hash ^ payload.charCodeAt(i), 16777619) >>> 0;
  const record = `${payload}${hash}\n`;
  return record + '\0'.repeat(256 - record.length);
}

async function main() {
  execFileSync(path.join(root, 'manager/node_modules/.bin/tsc'), [
    path.join(root, 'manager/src/ts/reload-signal.ts'), '--outDir', temporary,
    '--module', 'commonjs', '--target', 'es2018', '--lib', 'es2018,dom', '--skipLibCheck'
  ], { stdio: 'inherit' });
  const { sendReloadSignal, parseReloadResult, waitForReload, waitForReloadOutcome } =
    require(path.join(temporary, 'reload-signal.js'));

  {
    const writes = [];
    await sendReloadSignal('123456', {
      async writeText(uri, text) { writes.push([uri, text]); }
    });
    assert.deepEqual(writes, [
      ['internal://files/reload.result', 'pending\t123456\n'],
      ['internal://files/reload.request', 'resource-hook-reload-v1\tng.lst.corona\t123456\n']
    ]);
  }

  {
    const writes = [];
    await sendReloadSignal('retry-7', {
      async writeText(uri, text) {
        writes.push([uri, text]);
        if (uri.endsWith('reload.result')) throw new Error('optional result unavailable');
      }
    });
    const request = writes[writes.length - 1];
    assert.equal(request[0], 'internal://files/reload.request');
    assert.equal(request[1], 'resource-hook-reload-v1\tng.lst.corona\tretry-7\n');
  }

  {
    let requestWritten = false;
    await assert.rejects(sendReloadSignal('bad revision', {
      async writeText(uri) {
        if (uri.endsWith('reload.request')) requestWritten = true;
      }
    }), /重载版本标识无效/);
    assert.equal(requestWritten, false);
  }

  await assert.rejects(sendReloadSignal('42', {
    async writeText(uri) {
      if (uri.endsWith('reload.request')) throw new Error('signal write failed');
    }
  }), /signal write failed/);

  const response = makeResponse('123456');
  assert.deepEqual(parseReloadResult(response, '123456'), {
    version: 5, result: 0, pending: false, changed: 1
  });
  assert.equal(parseReloadResult(response, 'stale-revision'), null);
  const recordEnd = response.indexOf('\\0');
  const checksumStart = response.lastIndexOf('\\n', recordEnd - 2) + 1;
  const badChecksum = response.slice(0, checksumStart) +
    (response[checksumStart] === '0' ? '1' : '0') + response.slice(checksumStart + 1);
  assert.equal(parseReloadResult(badChecksum, '123456'), null);

  {
    let polls = 0;
    const message = await waitForReload('123456', {
      async readOptionalText(uri) {
        assert.equal(uri, 'internal://files/reload.result');
        polls++;
        return polls === 1 ? null : response;
      }
    }, 3, 0);
    assert.equal(message, '重载完成，更新 1 项资源');
    assert.equal(polls, 2);
  }

  assert.deepEqual(await waitForReloadOutcome('123456', {
    async readOptionalText() { return response; }
  }, 1, 0), { successful: true, message: '重载完成，更新 1 项资源' });
  assert.deepEqual(await waitForReloadOutcome('123456', {
    async readOptionalText() { return makeResponse('123456', { result: -2103 }); }
  }, 1, 0), { successful: false, message: '模块拒绝重载（-2103）' });

  assert.equal(await waitForReload('123456', {
    async readOptionalText() { return makeResponse('123456', { result: -2103 }); }
  }, 1, 0), '模块拒绝重载（-2103）');
  assert.equal(await waitForReload('123456', {
    async readOptionalText() { return null; }
  }, 1, 0), '未收到模块响应');
  console.log('Reload signal format, checksummed module responses and wait behavior passed.');
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
