/* Host lifecycle regressions for ownership-safe automatic Manager cleanup. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const temporary = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'manager-launch-')));
const tick = () => new Promise(resolve => setTimeout(resolve, 0));

async function main() {
  execFileSync(path.join(root, 'manager/node_modules/.bin/tsc'), [
    path.join(root, 'manager/src/ts/interconnect.ts'), '--outDir', temporary,
    '--module', 'commonjs', '--target', 'es2018', '--lib', 'es2018,dom', '--skipLibCheck', '--allowJs'
  ], { stdio: 'inherit' });
  let connection;
  let terminateCalls = 0;
  const sent = [];
  const boundary = path.join(temporary, 'import.js');
  require.cache[boundary] = { id: boundary, filename: boundary, loaded: true,
    exports: { __esModule: true, app: { terminate() { terminateCalls++; } },
      interconnect: { instance: () => connection }, file: {} } };
  const { InterconnectThemeReceiver } = require(path.join(temporary, 'interconnect.js'));
  const manifest = JSON.parse(fs.readFileSync(path.join(root, 'manager/src/manifest.json'), 'utf8'));
  const routes = Object.keys(manifest.router.pages).map(route => route.slice('pages/'.length));
  assert.equal(routes.length, 6, 'every possible initial route is exercised');
  const token = 'cold_check-42';
  const baseHandshake = { version: 2, type: 'response', replyTo: 'probe_42',
    maxTextChars: 18000, maxWindow: 4 };

  function evaluateUx(filename, context) {
    const source = fs.readFileSync(path.join(root, `manager/src/${filename}`), 'utf8');
    const script = /<script>([\s\S]*?)<\/script>/.exec(source)[1]
      .replace(/^import[\s\S]*?from "[^"]+"\r?\n/gm, '')
      .replace('export default', 'globalThis.definition =');
    vm.runInNewContext(script, context, { filename });
    return { source, definition: context.definition };
  }

  let current;
  function boot() {
    if (current) current.app.onDestroy();
    sent.length = 0;
    connection = { onmessage: null, onopen: null, onclose: null, onerror: null,
      send(options) { sent.push(options.data); options.success(); } };
    const context = {
      InterconnectThemeReceiver, file: {}, router: { push() {} },
      initializeResourceCatalogSession() {}, destroyResourceCatalogSession() {},
      initializeModuleControlSession() {}, destroyModuleControlSession() {}
    };
    const { definition: app } = evaluateUx('app.ux', context);
    app.onCreate();
    assert(context.themeReceiver instanceof InterconnectThemeReceiver);
    current = { app, receiver: context.themeReceiver };
    return current;
  }

  function page(name, launch, query = {}) {
    const context = {
      themeReceiver: launch.receiver, DEFAULT_RESOURCE_CHOICE: '@default',
      SYSTEM_RESOURCE_CHOICE: '@system', file: {}, router: { push() {}, back() {} },
      brightness: { setKeepScreenOn() {} }, prompt: { showToast() {} },
      withResourceOperation: operation => operation()
    };
    const { source, definition } = evaluateUx(`pages/${name}/${name}.ux`, context);
    const instance = { ...definition.public, ...definition.private, ...definition };
    // Model documented HAP query injection: only declared public properties receive it.
    for (const key of Object.keys(query)) {
      if (Object.hasOwn(definition.public || {}, key)) instance[key] = query[key];
    }
    const rootTouch = /<div class="page"[^>]*ontouchstart="([^"]+)"/.exec(source);
    assert(rootTouch, `${name}: existing root touchstart must revoke ownership`);
    assert.equal(typeof instance[rootTouch[1]], 'function');
    instance.rootTouch = () => instance[rootTouch[1]]();
    // Avoid unrelated native status/manifest I/O; capture must run before these methods.
    instance.loadModuleStatus = () => Promise.resolve();
    instance.loadManifest = () => Promise.resolve();
    instance.performReload = () => Promise.resolve();
    return instance;
  }

  async function handshake(extra = {}) {
    const start = sent.length;
    connection.onmessage({ data: 'H' + JSON.stringify({ version: 2, type: 'request',
      requestId: 'probe_42', maxTextChars: 18000, ...extra }) });
    await tick();
    return sent.slice(start).map(envelope => JSON.parse(envelope.msg.slice(1)));
  }

  async function quit(rejection, launchToken = token) {
    const before = terminateCalls;
    const start = sent.length;
    connection.onmessage({ data: 'Q' + JSON.stringify({ requestId: 'quit_42', launchToken }) });
    await tick();
    assert.deepEqual(sent.slice(start).map(envelope => JSON.parse(envelope.msg.slice(1))), [rejection
      ? { replyTo: 'quit_42', status: 'reject', errorCode: rejection }
      : { replyTo: 'quit_42', status: 'ready' }]);
    assert.equal(terminateCalls, before + (rejection ? 0 : 1));
    if (rejection) assert.equal(typeof connection.onmessage, 'function', 'rejection preserves the app');
  }

  // onCreate establishes a boot/gate, but an early H cannot invent a launch marker.
  let launch = boot();
  assert.deepEqual(await handshake({ launchToken: token }), [baseHandshake]);
  let home = page('index', launch, { astroboxCheckToken: token });
  assert(Object.hasOwn(home.public, 'astroboxCheckToken'));
  assert(!Object.hasOwn(home.private, 'astroboxCheckToken'), 'query token must not be private');
  let resolveStatus;
  home.loadModuleStatus = () => new Promise(resolve => { resolveStatus = resolve; });
  const initial = home.onInit();
  assert.equal(typeof resolveStatus, 'function');
  assert.deepEqual(await handshake(), [{ ...baseHandshake, launchToken: token }],
    'first-page capture is synchronous, before any native await');
  resolveStatus();
  await initial;
  launch.receiver.start(); // Redundant start is not a receiver restart.
  assert.deepEqual(await handshake(), [{ ...baseHandshake, launchToken: token }]);
  await quit();

  // Ordinary/pre-open boots and every foreign initial route consume the gate without ownership.
  for (const route of routes) {
    launch = boot();
    const first = page(route, launch, route === 'index' ? {} : { astroboxCheckToken: token });
    await first.onInit();
    assert.deepEqual(await handshake(), [baseHandshake], `${route}: no cold-index marker`);
    const reentered = page('index', launch, { astroboxCheckToken: token });
    await reentered.onInit();
    await reentered.onShow();
    if (reentered.onRefresh) await reentered.onRefresh();
    assert.deepEqual(await handshake({ launchToken: token }), [baseHandshake],
      `${route} then index: re-entry/foreground cannot bind ownership`);
    launch.receiver.stop();
    launch.receiver.start();
    await page('index', launch, { astroboxCheckToken: token }).onInit();
    assert.deepEqual(await handshake(), [baseHandshake], 'stop/start does not reopen the gate');
    await quit('not-owner');
  }

  // A manually reopened standard index revokes before touch, with no/same/different query token.
  for (const query of [{}, { astroboxCheckToken: token }, { astroboxCheckToken: 'another_check' }]) {
    launch = boot();
    await page('index', launch, { astroboxCheckToken: token }).onInit();
    assert.deepEqual(await handshake(), [{ ...baseHandshake, launchToken: token }]);
    const reentered = page('index', launch, query);
    await reentered.onInit();
    assert.deepEqual(await handshake(), [baseHandshake], 'repeated onInit revokes before onShow/touch');
    await reentered.onShow();
    assert.deepEqual(await handshake({ launchToken: 'another_check' }), [baseHandshake]);
    await quit('not-owner');
    await quit('not-owner', 'another_check');
  }

  // Every later page initialization is conservative takeover, not just a repeated index.
  for (const route of routes) {
    launch = boot();
    await page('index', launch, { astroboxCheckToken: token }).onInit();
    await page(route, launch, { astroboxCheckToken: token }).onInit();
    assert.deepEqual(await handshake(), [baseHandshake], `${route}: later onInit revokes without touch`);
    await quit('not-owner');
  }

  // Refresh cannot bind public params and revokes even before the first onInit has captured them.
  for (const beforeCapture of [false, true]) {
    launch = boot();
    home = page('index', launch, { astroboxCheckToken: token });
    if (!beforeCapture) {
      await home.onInit();
      assert.deepEqual(await handshake(), [{ ...baseHandshake, launchToken: token }]);
    }
    assert.equal(typeof home.onRefresh, 'function');
    home.astroboxCheckToken = 'another_check';
    home.onRefresh();
    await home.onShow();
    await home.onInit();
    assert.deepEqual(await handshake(), [baseHandshake], 'refresh cannot grant or recover ownership');
    await quit('not-owner');
    await quit('not-owner', 'another_check');
  }

  // Background/minimize then foreground retains the app but never restores automatic-close authority.
  for (const beforeCapture of [false, true]) {
    launch = boot();
    home = page('index', launch, { astroboxCheckToken: token });
    if (!beforeCapture) {
      await home.onInit();
      assert.deepEqual(await handshake(), [{ ...baseHandshake, launchToken: token }]);
    }
    assert.equal(typeof launch.app.onHide, 'function');
    launch.app.onHide();
    if (beforeCapture) await home.onInit();
    await home.onShow();
    assert.deepEqual(await handshake(), [baseHandshake], 'foreground/onShow must not reauthorize');
    await quit('not-owner');
  }

  // These lifecycle takeovers must also block delayed native ready callbacks, without a touch or disconnect.
  for (const takeover of ['normal-index', 'token-index', 'refresh', 'app-hide']) {
    launch = boot();
    home = page('index', launch, { astroboxCheckToken: token });
    await home.onInit();
    const originalSend = connection.send;
    let finishReady;
    connection.send = options => { sent.push(options.data); finishReady = options.success; };
    const start = sent.length;
    const before = terminateCalls;
    connection.onmessage({ data: 'Q' + JSON.stringify({ requestId: 'quit_42', launchToken: token }) });
    await tick();
    assert.deepEqual(sent.slice(start).map(envelope => JSON.parse(envelope.msg.slice(1))),
      [{ replyTo: 'quit_42', status: 'ready' }]);
    assert.equal(typeof finishReady, 'function');
    if (takeover === 'app-hide') launch.app.onHide();
    else if (takeover === 'refresh') home.onRefresh();
    else await page('index', launch, takeover === 'token-index' ? { astroboxCheckToken: token } : {}).onInit();
    connection.send = originalSend;
    finishReady();
    await tick();
    assert.equal(terminateCalls, before, `${takeover}: ready success must not close the new user foreground`);
    assert.equal(typeof connection.onmessage, 'function');
    assert.deepEqual(await handshake(), [baseHandshake]);
    await quit('not-owner');
  }

  // onShow alone can neither capture nor authorize a public query parameter.
  launch = boot();
  home = page('index', launch, { astroboxCheckToken: token });
  await home.onShow();
  assert.deepEqual(await handshake(), [baseHandshake]);
  await home.onInit();
  assert.deepEqual(await handshake(), [{ ...baseHandshake, launchToken: token }]);
  await quit();

  // Root handlers themselves revoke, independently of later-page onInit, including scroll gestures.
  for (const route of routes) {
    launch = boot();
    await page('index', launch, { astroboxCheckToken: token }).onInit();
    const shown = page(route, launch, { astroboxCheckToken: token });
    assert.deepEqual(await handshake(), [{ ...baseHandshake, launchToken: token }]);
    shown.rootTouch();
    await page('index', launch, { astroboxCheckToken: token }).onInit();
    assert.deepEqual(await handshake(), [baseHandshake], `${route}: touch revocation is permanent`);
    await quit('not-owner');
  }

  // Existing navigation/reload hooks also revoke even if no touch event is delivered.
  for (const action of ['openResources', 'openMixMatch', 'requestReload']) {
    launch = boot();
    home = page('index', launch, { astroboxCheckToken: token });
    await home.onInit();
    await home[action]();
    assert.deepEqual(await handshake(), [baseHandshake], `${action}: no automatic close after user action`);
    await quit('not-owner');
  }
  current.app.onDestroy();
  console.log('Manager cold-launch gate, public HAP query, all initial routes and user takeover tests passed.');
}

main().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => {
  fs.rmSync(temporary, { recursive: true, force: true });
});
