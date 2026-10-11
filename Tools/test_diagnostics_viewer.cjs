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
    this.context = {
      strokes: [], path: [], dash: [], globalAlpha: 1,
      scale() {}, fillText() {},
      setLineDash(value) { this.dash = [...value]; },
      beginPath() { this.path = []; },
      moveTo(x, y) { this.path.push(['move', x, y]); },
      lineTo(x, y) { this.path.push(['line', x, y]); },
      stroke() { this.strokes.push({ color: this.strokeStyle, alpha: this.globalAlpha,
        dash: [...this.dash], path: [...this.path] }); },
    };
    return this.context;
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
    update(scopes, extra = {}) { context.metrics({ scopes, values: {}, time: 1, load: 1, peak_load: 2, mean_ms: 0.1, p99_ms: 0.2, ...extra }); },
    select: context.select,
    rows: () => document.getElementById('scopes').children,
    visible() { return this.rows().filter(row => !row.hidden).map(row => row.firstChild.children[1].textContent); },
    row(name) { return this.rows().find(row => row.firstChild.children[1].textContent === name); },
    button(name) { return this.row(name).firstChild.children[0].firstChild; },
    scopeButton(name) { return this.row(name).firstChild.children[1]; },
    element: id => document.getElementById(id),
    traces() { return this.element('graph').context.strokes.slice(1); },
  };
}

function scope(id, name, parents, total = 0.2) {
  return { id, name, parents, self_ms: 0.1, total_ms: total, calls_per_block: 1 };
}
const scopes = () => [scope(0, 'processBlock', []), scope(1, 'getControls', [0]),
  scope(2, 'Looper::process', [0]), scope(3, 'filter', [2]), scope(4, 'mix', [3])];
const idle = record => ({ ...record, self_ms: 0, total_ms: 0, calls_per_block: 0 });

test('unexecuted scopes stay hidden until they run, then retain their learned hierarchy while idle', () => {
  const page = viewer();
  const configured = [scope(0, 'processBlock', []), scope(1, 'Looper::process', []),
    scope(2, 'Looper::loopPlayback', []), scope(3, 'Looper::granularPlayback', []),
    scope(4, 'GranularEngine::renderBlock', [])].map(idle);
  page.update(configured);
  assert.equal(page.rows().length, 0);

  const recording = [scope(0, 'processBlock', []), scope(1, 'Looper::process', [0]),
    ...configured.slice(2)];
  page.update(recording); page.button('processBlock').click();
  assert.deepEqual(page.visible(), ['processBlock', 'Looper::process']);
  assert.equal(page.button('Looper::process').hidden, true);
  assert.equal(page.row('Looper::loopPlayback'), undefined);

  // Positive calls prove execution even when the measured duration is zero.
  const playback = [...recording];
  playback[2] = { ...scope(2, 'Looper::loopPlayback', [1]), self_ms: 0, total_ms: 0, calls_per_block: 0.001 };
  page.update(playback);
  assert.deepEqual(page.visible(), ['processBlock', 'Looper::process']);
  assert.equal(page.button('Looper::process').hidden, false);
  page.button('Looper::process').click();
  assert.deepEqual(page.visible(), ['processBlock', 'Looper::process', 'Looper::loopPlayback']);
  assert.equal(page.row('Looper::loopPlayback').firstChild.style.paddingLeft, '44px');
  const row = page.row('Looper::loopPlayback');
  page.update(playback.map(idle));
  assert.equal(page.row('Looper::loopPlayback'), row);
  assert.deepEqual(page.visible(), ['processBlock', 'Looper::process', 'Looper::loopPlayback']);
  assert.equal(row.children[3].textContent, '0.0');
  assert.equal(page.row('Looper::granularPlayback'), undefined);
  assert.equal(page.row('GranularEngine::renderBlock'), undefined);
});

test('opening an idle session recognizes executed scopes from cumulative parents and block counts', () => {
  const page = viewer();
  page.update([idle(scope(0, 'block', [])), idle(scope(1, 'parent', [0])),
    idle(scope(2, 'child', [1])), idle(scope(3, 'unexecuted', []))], { total_blocks: 10 });
  assert.deepEqual(page.visible(), ['block']);
  page.button('block').click(); page.button('parent').click();
  assert.deepEqual(page.visible(), ['block', 'parent', 'child']);
  assert.equal(page.row('child').firstChild.style.paddingLeft, '44px');
  assert.equal(page.row('unexecuted'), undefined);
});

test('selecting or resetting a session forgets scopes executed in its previous history', () => {
  const page = viewer();
  page.select('session-a'); page.update(scopes());
  page.button('processBlock').click();
  page.select('session-b'); page.update(scopes().map(record => idle({ ...record, parents: [] })));
  assert.equal(page.rows().length, 0);
  page.update(scopes()); page.button('processBlock').click();
  page.select('session-b'); page.update(scopes().map(record => idle({ ...record, parents: [] })));
  assert.equal(page.rows().length, 0);
});

test('older recordings hide unexecuted scopes without mistaking fallback parents for execution', () => {
  const page = viewer();
  const legacy = [scope(0, 'block', []), idle(scope(1, 'unexecuted', [])),
    scope(2, 'executed', [])].map(({ id, parents, ...record }) => record);
  page.update(legacy); page.button('block').click();
  assert.deepEqual(page.visible(), ['block', 'executed']);
  assert.equal(page.row('unexecuted'), undefined);
  page.update(legacy.map(idle));
  assert.deepEqual(page.visible(), ['block', 'executed']);
  assert.equal(page.row('executed').firstChild.style.paddingLeft, '26px');
});

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

const loadedScopes = (average = 20, peak = 40) => [
  { ...scope(0, 'block', []), load: 50, peak_load: 80 },
  { ...scope(1, 'function', [0]), load: average, peak_load: peak },
  { ...scope(2, 'other', [0]), load: 10, peak_load: 15 },
];
const redTrace = (page, peak = false) => page.traces().find(trace => trace.color === (peak ? '#ffabb1' : '#f06d76'));
const plotY = load => 118 - load / 100 * 108;

test('selecting a function draws its red average and dashed peak while fading the global graph', () => {
  const page = viewer();
  page.update(loadedScopes(), { time: 1, load: 50, peak_load: 80 });
  page.update(loadedScopes(30, 60), { time: 2, load: 50, peak_load: 80 });
  page.button('block').click(); page.scopeButton('function').click();
  assert.equal(page.scopeButton('function').tagName, 'button');
  assert.equal(page.scopeButton('function').attributes['aria-pressed'], 'true');
  assert.equal(page.scopeButton('other').attributes['aria-pressed'], 'false');
  assert.equal(page.row('function').className, 'scope-selected');
  assert.deepEqual(page.traces().slice(0, 2).map(trace => trace.alpha), [0.25, 0.25]);
  assert.deepEqual(redTrace(page).path.map(point => point[2]), [plotY(20), plotY(30)]);
  assert.deepEqual(redTrace(page, true).path.map(point => point[2]), [plotY(40), plotY(60)]);
  assert.deepEqual(redTrace(page, true).dash, [5, 3]);
  assert.equal(redTrace(page).alpha, 1);
  assert.equal(page.element('graph-scope').textContent, 'function');
  assert.match(page.element('scope-details').textContent, /Average 30.0% · peak 60.0%/);
  assert.match(page.element('graph').attributes['aria-label'], /function and block/);

  page.scopeButton('other').click();
  assert.equal(page.scopeButton('function').attributes['aria-pressed'], 'false');
  assert.equal(page.scopeButton('other').attributes['aria-pressed'], 'true');
  assert.equal(page.element('graph-scope').textContent, 'other');
  page.element('clear-scope').click();
  assert.deepEqual(page.traces().map(trace => trace.alpha), [1, 1]);
  assert.equal(page.element('scope-legend').hidden, true);
  assert.equal(page.element('clear-scope').hidden, true);
  page.scopeButton('function').click(); page.scopeButton('function').click();
  assert.equal(page.traces().length, 2);
});

test('selection survives updates and collapsed parents, and clears on removal or session reset', () => {
  const page = viewer(); page.select('first');
  page.update(loadedScopes()); page.button('block').click(); page.scopeButton('function').click();
  page.button('block').click();
  assert.equal(page.row('function').hidden, true);
  assert.equal(page.element('graph-scope').textContent, 'function');
  const inactive = loadedScopes().map(record => ({ ...idle(record), load: 0, peak_load: 0 }));
  page.update(inactive);
  assert.equal(page.scopeButton('function').attributes['aria-pressed'], 'true');
  assert.match(page.element('scope-details').textContent, /Average 0.0% · peak 0.0%/);
  page.update([loadedScopes()[0]]);
  assert.equal(page.element('scope-legend').hidden, true);
  assert.equal(page.traces().length, 2);
  page.update(loadedScopes()); page.button('block').click(); page.scopeButton('function').click();
  page.select('first'); page.update(loadedScopes());
  assert.equal(page.scopeButton('function').attributes['aria-pressed'], 'false');
  assert.equal(page.traces()[0].path.length, 1);
  page.button('block').click(); page.scopeButton('function').click(); page.select('second');
  assert.equal(page.element('scope-legend').hidden, true);
  assert.equal(page.traces()[0].path.length, 0);
});

test('legacy scope graphs derive an average from the block budget and leave unavailable peaks blank', () => {
  const page = viewer();
  const legacy = [scope(0, 'block', [], 2), scope(1, 'function', [0], 0.5)]
    .map(({ id, parents, ...record }) => record);
  page.update(legacy, { load: 50, peak_load: 80, mean_ms: 2 });
  page.button('block').click(); page.scopeButton('function').click();
  assert.equal(redTrace(page).path[0][2], plotY(12.5));
  assert.equal(redTrace(page, true).path.length, 0);
  assert.match(page.element('scope-details').textContent, /peak unavailable in this recording/);
  page.update(legacy, { load: 0, peak_load: 0, mean_ms: 0 });
  assert.equal(redTrace(page).path.at(-1)[2], plotY(0));
  page.scopeButton('block').click();
  assert.match(page.element('scope-details').textContent, /peak 0.0%/);
});

test('scope history follows IDs across reordering and breaks lines at missing data', () => {
  const page = viewer();
  page.update(loadedScopes(10, 20), { time: 1 });
  page.update(loadedScopes(20, 30).filter(record => record.id !== 1), { time: 2 });
  page.update(loadedScopes(30, 40).reverse(), { time: 3 });
  page.button('block').click(); page.scopeButton('function').click();
  assert.deepEqual(redTrace(page).path.map(point => point[0]), ['move', 'move']);
  assert.deepEqual(redTrace(page).path.map(point => point[2]), [plotY(10), plotY(30)]);
  page.update(loadedScopes(40, 50), { time: 4 });
  assert.deepEqual(redTrace(page).path.map(point => point[0]), ['move', 'move', 'line']);
});

test('graph scales for scope loads above 100 percent and bounds the retained history', () => {
  const page = viewer();
  page.update(loadedScopes(150, 250), { time: 0 });
  page.button('block').click(); page.scopeButton('function').click();
  assert.ok(redTrace(page, true).path[0][2] >= 10);
  assert.match(page.element('scope-details').textContent, /peak 250.0%/);
  for (let index = 1; index <= 125; ++index)
    page.update(loadedScopes(), { time: index / 10 });
  assert.equal(redTrace(page).path.length, 120);
  page.update(loadedScopes(30, 40), { time: 50 });
  assert.equal(redTrace(page).path.length, 1);
  assert.equal(redTrace(page).path[0][2], plotY(30));
});
