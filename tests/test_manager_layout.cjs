/* Host regression checks for rounded-screen list bounds and rectangular layouts. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const root = path.resolve(__dirname, '..');
const pages = [
  ['resources', '.resource-list'],
  ['mix-match', '.resource-list'],
  ['mix-match-select', '.choice-list']
];

function blockAfter(source, start) {
  const open = source.indexOf('{', start);
  let depth = 1;
  let end = open + 1;
  while (depth && end < source.length) {
    if (source[end] === '{') depth += 1;
    if (source[end] === '}') depth -= 1;
    end += 1;
  }
  assert.equal(depth, 0, 'CSS block must be closed');
  return { body: source.slice(open + 1, end - 1), end };
}

function rule(source, selector) {
  const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  const matches = [...source.matchAll(new RegExp(`${escaped}\\s*\\{([^{}]*)\\}`, 'g'))];
  return Object.fromEntries(matches.flatMap(match =>
    [...match[1].matchAll(/([\w-]+)\s*:\s*([^;]+);/g)]
      .map(([, key, value]) => [key, value.trim()])));
}

function stylesForScreen(style, width, height, density) {
  let active = style.slice(0, style.indexOf('@media'));
  const media = /@media\s*\((aspect-ratio|width):\s*([\d/]+)\)\s*\{/g;
  for (let match; (match = media.exec(style));) {
    const block = blockAfter(style, match.index);
    media.lastIndex = block.end;
    const [, feature, value] = match;
    const [numerator, denominator] = value.split('/').map(Number);
    const applies = feature === 'width'
      ? width / density === numerator
      : width * denominator === height * numerator;
    if (applies) active += block.body;
  }
  return active;
}

let resourcesSource;
for (const [page, listSelector] of pages) {
  const source = fs.readFileSync(path.join(root, `manager/src/pages/${page}/${page}.ux`), 'utf8');
  const style = /<style>([\s\S]*?)<\/style>/.exec(source)[1];
  assert.match(source, /<div class="bottom-spacer"><\/div>\s*<\/scroll>/,
    `${page}: safe-area spacer must be the last scrolling child`);

  for (const density of [1, 2, 2.1]) {
    const active = stylesForScreen(style, 212, 520, density);
    const list = rule(active, listSelector);
    const spacer = rule(active, '.bottom-spacer');
    assert.equal(list.top, '96px');
    assert.equal(list.height, '424px');
    assert.equal(spacer.display, 'flex');
    assert.equal(spacer.height, '52px');
    assert.equal(spacer['flex-shrink'], '0');
    assert.equal(parseInt(list.top) + parseInt(list.height), 520);
    assert.equal(parseInt(list.height) - parseInt(spacer.height), 372);
    assert(active.includes('height: 424px;'), 'ratio must match independently of density');
  }

  const rectangular = stylesForScreen(style, 336, 480, 2.1);
  assert(!rectangular.includes('height: 424px;'), `${page}: rectangular screen must not expand`);
  assert.equal(rule(rectangular, '.bottom-spacer').display, 'none');
  const wide = blockAfter(style, style.indexOf('@media (width: 160)')).body;
  assert.equal(rule(wide, listSelector).top, '60px');
  assert.equal(rule(wide, listSelector).height, '408px');
  assert.equal(rule(wide, '.header').height, '60px');
  const otherScreen = stylesForScreen(style, 212, 468, 1);
  assert(!otherScreen.includes('height: 424px;'), 'same width with another ratio stays unchanged');
  assert.equal(rule(otherScreen, listSelector).height, '372px');
  assert.equal(rule(otherScreen, '.bottom-spacer').display, 'none');
  if (page === 'resources') resourcesSource = source;
}

for (const [page, action, image, handler] of [
  ['index', 'reload', 'reload', 'requestReload'],
  ['resource-detail', 'delete', 'del', 'onDeleteTap']
]) {
  const source = fs.readFileSync(path.join(root, `manager/src/pages/${page}/${page}.ux`), 'utf8');
  const style = /<style>([\s\S]*?)<\/style>/.exec(source)[1];
  const template = /<template>([\s\S]*?)<\/template>/.exec(source)[1];
  const narrowSelector = `.${action}-action`;
  const wideSelector = `.${action}-action-wide`;
  const wrapperSelector = `.${action}-action-wrap`;

  for (const [width, height, density, wide] of [
    [212, 520, 1, false],
    [212, 520, 2, false],
    [336, 480, 2.1, true],
    [212, 468, 1, false]
  ]) {
    const active = stylesForScreen(style, width, height, density);
    const narrow = rule(active, narrowSelector);
    const wideAction = rule(active, wideSelector);
    const wrapper = rule(active, wrapperSelector);
    assert.equal(narrow.display || 'flex', wide ? 'none' : 'flex', `${page}: original image visibility`);
    assert.equal(wideAction.display, wide ? 'flex' : 'none', `${page}: wide image visibility`);
    assert.equal(narrow.width, '150px');
    assert.equal(narrow.height, '78px');
    assert.equal(wideAction.width, '324px');
    assert.equal(wideAction.height, '80px');
    assert.equal(wrapper.height, wide ? '80px' : '78px');
    assert.equal(wrapper.bottom, '10px');
    if (wide) assert(parseInt(wideAction.width) <= width, 'wide action must fit on screen');
  }

  for (const suffix of ['', '-wide']) {
    const tag = template.match(new RegExp(`<img class="${action}-action${suffix}"[^>]*>`))[0];
    assert(tag.includes(`src="/common/images/${image}${suffix}.png"`));
    assert(tag.includes(`onclick="${handler}"`), `${page}: both images retain their action`);
    if (action === 'reload') assert(tag.includes('opacity: {{sendingReload ? 0.4 : 1}};'));
  }

  const png = fs.readFileSync(path.join(root, `manager/src/common/images/${image}-wide.png`));
  assert.equal(png.subarray(0, 8).toString('hex'), '89504e470d0a1a0a');
  assert.equal(png.readUInt32BE(16), 324);
  assert.equal(png.readUInt32BE(20), 80);
}

const script = /<script>([\s\S]*?)<\/script>/.exec(resourcesSource)[1]
  .replace(/^import[\s\S]*?from "[^"]+"\n/gm, '')
  .replace('export default', 'globalThis.page =');
const context = {};
vm.runInNewContext(script, context);
const scrollCalls = [];
const page = { ...context.page.private, ...context.page,
  rows: Array.from({ length: 4 }, () => ({})), visible: true,
  $element() { return { scrollTo(options) { scrollCalls.push(options); } }; } };
page.handleScroll({ scrollY: 10000 });
assert.equal(page.scrollOffset, 4 * 140 + 52 - 424,
  'scroll clamp includes the trailing spacer without changing the old maximum');
assert.equal(96 + 4 * 140 - page.scrollOffset, 468,
  'the final row ends above the rounded bottom');
page.dragId = 'dragging';
page.touching = true;
page.scrollOffset = 0;
page.lastY = 425;
page.scrollAtEdge();
assert.equal(scrollCalls.length, 0);
page.lastY = 450;
page.scrollAtEdge();
assert.equal(scrollCalls.length, 1, 'edge scrolling must start inside the safe bottom area');
assert(scrollCalls[0].top > 0);

console.log('Manager rounded-screen layout tests passed.');
