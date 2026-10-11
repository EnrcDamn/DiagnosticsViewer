const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { test } = require('node:test');
const vm = require('node:vm');

// Exercise the viewer's actual script without a browser or network dependency.
class Element {
  constructor(tag) {
    this.tagName = tag; this.children = []; this.style = {}; this.attributes = {};
    this.listeners = {}; this.hidden = false; this.parent = null; this.text = '';
    this.clientWidth = 740; this.clientHeight = 130;
  }
  get firstChild() { return this.children[0]; }
  get textContent() { return this.text + this.children.map(child => child.textContent).join(''); }
  set textContent(value) { this.replaceChildren(); this.text = String(value); }
  append(...children) {
    for (const child of children) { child.remove(); child.parent = this; this.children.push(child); }
  }
  insertBefore(child, reference) {
    child.remove(); child.parent = this;
    const index = reference ? this.children.indexOf(reference) : this.children.length;
    assert.ok(index >= 0, 'Insertion reference must belong to its parent');
    this.children.splice(index, 0, child);
  }
  remove() {
    if (this.parent) {
      this.parent.children.splice(this.parent.children.indexOf(this), 1); this.parent = null;
    }
  }
  replaceChildren(...children) {
    for (const child of [...this.children]) child.remove();
    this.text = ''; this.append(...children);
  }
  addEventListener(name, callback) { this.listeners[name] = callback; }
  setAttribute(name, value) { this.attributes[name] = value; }
  click() { this.listeners.click(); }
  getContext() {
    return Object.fromEntries(['scale', 'setLineDash', 'beginPath', 'moveTo', 'lineTo', 'stroke', 'fillText']
      .map(name => [name, () => {}]));
  }
}

function viewer() {
  const elements = new Map();
  const document = {
    getElementById(id) {
      if (!elements.has(id)) elements.set(id, new Element(id));
      return elements.get(id);
    },
    createElement: tag => new Element(tag),
  };
  const context = vm.createContext({ document, performance: { now: () => 100 },
    window: { addEventListener() {}, devicePixelRatio: 1 } });
  const html = readFileSync(join(__dirname, 'diagnostics_viewer.html'), 'utf8');
  const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
  vm.runInContext(script.replace(/  draw\(\); tick\(\);\s*$/, ''), context);
  return {
    update(scopes) { context.metrics({ scopes, values: {}, time: 1, load: 1, peak_load: 2, mean_ms: 0.1, p99_ms: 0.2 }); },
    select: context.select,
    rows: () => document.getElementById('scopes').children,
    visible() { return this.rows().filter(row => !row.hidden).map(row => row.firstChild.children[1].textContent); },
    row(name) { return this.rows().find(row => row.firstChild.children[1].textContent === name); },
    button(name) { return this.row(name).firstChild.children[0].firstChild; },
  };
}

function scope(id, name, parents, total = 0.2) {
  return { id, name, parents, self_ms: 0.1, total_ms: total, calls_per_block: 1 };
}
const scopes = () => [scope(0, 'processBlock', []), scope(1, 'getControls', [0]),
  scope(2, 'Looper::process', [0]), scope(3, 'filter', [2]), scope(4, 'mix', [3])];

test('nested branches expand independently and retain child choices when their parent closes', () => {
  const page = viewer(); page.update(scopes());
  assert.deepEqual(page.visible(), ['processBlock']);
  assert.equal(page.button('processBlock').textContent, '+');
  assert.equal(page.button('getControls').hidden, true);
  page.button('processBlock').click();
  assert.deepEqual(page.visible(), ['processBlock', 'getControls', 'Looper::process']);
  assert.equal(page.button('processBlock').attributes['aria-expanded'], 'true');
  assert.equal(page.button('processBlock').attributes['aria-label'], 'Collapse processBlock');
  page.button('Looper::process').click(); page.button('filter').click();
  assert.deepEqual(page.visible(), ['processBlock', 'getControls', 'Looper::process', 'filter', 'mix']);
  assert.equal(page.row('mix').firstChild.style.paddingLeft, '62px');
  page.button('processBlock').click();
  assert.deepEqual(page.visible(), ['processBlock']);
  page.button('processBlock').click();
  assert.equal(page.row('mix').hidden, false);
  page.button('Looper::process').click();
  assert.deepEqual(page.visible(), ['processBlock', 'getControls', 'Looper::process']);
});

test('live measurements preserve expanded branches and existing buttons', () => {
  const page = viewer(); page.update(scopes());
  page.button('processBlock').click(); page.button('Looper::process').click();
  const button = page.button('Looper::process');
  const updated = scopes(); updated[2].total_ms = 1.2345;
  page.update(updated);
  assert.equal(page.button('Looper::process'), button);
  assert.equal(button.textContent, '−');
  assert.equal(page.row('Looper::process').children[2].textContent, '1.2345');
  assert.equal(page.row('filter').hidden, false);
  page.select('another-session'); page.update(scopes());
  assert.deepEqual(page.visible(), ['processBlock']);
});

test('tree follows parent IDs even when scopes arrive out of tree order', () => {
  const page = viewer();
  page.update([scope(0, 'block', []), scope(1, 'child', [2]), scope(2, 'parent', [0]), scope(3, 'sibling', [0])]);
  page.button('block').click(); page.button('parent').click();
  assert.deepEqual(page.visible(), ['block', 'parent', 'child', 'sibling']);
  assert.equal(page.row('child').firstChild.style.paddingLeft, '44px');
});

test('older recordings can collapse their inner scopes under the block', () => {
  const page = viewer();
  page.update(scopes().slice(0, 3).map(({ id, parents, ...record }) => record));
  assert.deepEqual(page.visible(), ['processBlock']);
  page.button('processBlock').click();
  assert.deepEqual(page.visible(), ['processBlock', 'getControls', 'Looper::process']);
});

test('shared scopes show combined timings once and missing parents remain visible', () => {
  const page = viewer();
  page.update([scope(0, 'block', []), scope(1, 'parent', [0]), scope(2, 'shared', [0, 1]),
    scope(3, 'shared-child', [2]), scope(4, 'orphan', [99])]);
  assert.deepEqual(page.visible(), ['block', 'shared', 'orphan']);
  assert.match(page.row('shared').firstChild.children[1].title, /combined/);
  page.button('shared').click(); page.button('block').click();
  assert.deepEqual(page.visible(), ['block', 'parent', 'shared', 'shared-child', 'orphan']);
});

test('recursive parent relationships do not create cycles and removed scopes leave no stale rows', () => {
  const page = viewer();
  page.update([scope(0, 'block', []), scope(1, 'recursive', [1]), scope(2, 'a', [3]), scope(3, 'b', [2])]);
  assert.deepEqual(page.visible(), ['block', 'recursive', 'a']);
  page.button('a').click();
  assert.deepEqual(page.visible(), ['block', 'recursive', 'a', 'b']);
  page.update([scope(0, 'block', [])]);
  assert.equal(page.rows().length, 1);
  assert.equal(page.button('block').hidden, true);
});
