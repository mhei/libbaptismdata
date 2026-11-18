/* Exercise the real frontend script against a small DOM test double. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

class Element {
  constructor(tag) { this.tag = tag; this.children = []; this.listeners = {}; this.disabled = false; this.hidden = false; this.checked = false; this.value = ''; }
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this.children = children; }
  setAttribute(name, value) { this[name] = value; }
  addEventListener(name, callback) { this.listeners[name] = callback; }
  focus() {}
  querySelectorAll() {
    return this.children.flatMap(child => [ ...(['input', 'button'].includes(child.tag) ? [child] : []), ...child.querySelectorAll() ]);
  }
  async fire(name) { return this.listeners[name]({ preventDefault() {} }); }
}

(async () => {
  const elements = Object.fromEntries(['editor', 'login', 'status', 'rows', 'save', 'password'].map(id => [id, new Element(id === 'save' ? 'button' : 'form')]));
  elements.editor.append(elements.rows, elements.save);
  const calls = [];
  let failSave = false, requirePassword = false;
  let storedValues = { serial: 'ABC123', model: 'Device' };
  let permissions = { serial: true, model: false, absent: true };
  const document = { querySelector: selector => elements[selector.slice(1)], createElement: tag => new Element(tag) };
  const fetch = async (url, options) => {
    calls.push({ url, options });
    let status = 200;
    if (requirePassword && options.method === 'PUT' && options.headers.Authorization !== `Basic ${btoa('admin:secret')}`) status = 401;
    else if (options.method === 'PUT') status = failSave ? 500 : 204;
    return {
      status, ok: status >= 200 && status < 300,
      json: async () => status >= 400 ? { error: { message: 'Storage failed' } } :
        url.endsWith('metadata') ? { keys: permissions, write_authentication_required: requirePassword } : storedValues
    };
  };
  vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../web/app.js'), 'utf8'), { document, fetch, TextEncoder, btoa });
  await new Promise(setImmediate);
  const field = key => {
    const row = elements.rows.children.find(row => row.children[0].textContent === key);
    return { value: row.children[3].children[0], defined: row.children[2].children[0] };
  };
  assert.equal(field('model').value.readOnly, true);
  assert.equal(field('model').defined.disabled, true);
  assert.equal(field('serial').value.value, 'ABC123');
  for (const key of ['vendor', 'manufacturer', 'manufacturer_url', 'model_url', 'model_number', 'revision']) {
    assert.equal(field(key).defined.checked, false);
    assert.equal(field(key).defined.disabled, true);
    assert.equal(field(key).value.disabled, true);
  }
  assert.equal(field('model').defined.checked, true);
  // Definition changes can be undone without discarding the original value.
  field('serial').defined.checked = false;
  await field('serial').defined.fire('change');
  assert.equal(field('serial').value.value, 'ABC123');
  assert.equal(field('serial').value.disabled, true);
  assert.equal(field('serial').value.className.includes('border-warning'), true);
  field('serial').defined.checked = true;
  await field('serial').defined.fire('change');
  assert.equal(field('serial').value.disabled, false);
  assert.equal(field('serial').value.className, 'form-control');
  field('absent').defined.checked = true;
  await field('absent').defined.fire('change');
  assert.equal(field('absent').value.className.includes('border-warning'), true);
  field('absent').defined.checked = false;
  await field('absent').defined.fire('change');
  assert.equal(field('absent').value.className, 'form-control');

  field('serial').value.value = 'edited';
  await field('serial').value.fire('input');
  assert.equal(field('serial').value.className.includes('border-warning'), true);
  field('serial').value.value = 'ABC123';
  await field('serial').value.fire('input');
  assert.equal(field('serial').value.className, 'form-control');
  field('serial').value.value = '';
  await field('serial').value.fire('input');
  assert.equal(field('serial').value.className.includes('border-warning'), true);
  await elements.editor.fire('submit');
  assert.deepEqual(JSON.parse(calls.at(-1).options.body), { serial: '' });
  assert.equal(elements.status.className, 'alert alert-success');
  assert.equal(field('serial').value.className, 'form-control');
  await field('serial').value.fire('input');
  assert.equal(elements.status.hidden, true);
  field('serial').defined.checked = false;
  await field('serial').defined.fire('change');
  assert.equal(field('serial').value.disabled, true);
  assert.equal(field('absent').value.disabled, true);
  field('absent').defined.checked = true;
  await field('absent').defined.fire('change');
  field('absent').value.value = '';
  await field('absent').value.fire('input');
  await elements.editor.fire('submit');
  assert.deepEqual(JSON.parse(calls.at(-1).options.body), { absent: '', serial: null });
  assert.equal(field('serial').defined.checked, false);
  assert.equal(field('serial').value.disabled, true);
  failSave = true;
  await elements.editor.fire('submit');
  assert.equal(elements.status.className, 'alert alert-danger');
  assert.equal(elements.save.disabled, false);
  assert.equal(field('absent').defined.checked, true);
  await field('absent').value.fire('input');
  assert.equal(elements.status.hidden, true);
  requirePassword = true;
  await elements.editor.fire('submit');
  assert.equal(elements.editor.hidden, false);
  assert.equal(elements.login.hidden, false);
  failSave = false;
  const editedValue = field('absent').value.value;
  elements.password.value = 'secret';
  await elements.login.fire('submit');
  assert.equal(elements.editor.hidden, false);
  assert.equal(elements.login.hidden, true);
  assert.equal(elements.password.value, '');
  assert.equal(field('absent').value.value, editedValue);
  // A fresh page uses public metadata to prompt before the first write.
  vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../web/app.js'), 'utf8'), { document, fetch, TextEncoder, btoa });
  await new Promise(setImmediate);
  field('serial').value.value = 'unsaved edit';
  await field('serial').value.fire('input');
  const beforePrompt = calls.length;
  await elements.editor.fire('submit');
  assert.equal(calls.length, beforePrompt);
  assert.equal(elements.editor.hidden, false);
  assert.equal(elements.login.hidden, false);
  elements.password.value = 'secret';
  await elements.login.fire('submit');
  assert.equal(JSON.parse(calls.at(-1).options.body).serial, 'unsaved edit');
  assert.equal(elements.login.hidden, true);
  // A writable standard key persists as an undefined row after deletion.
  requirePassword = false;
  storedValues = {};
  permissions = { vendor: true };
  vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../web/app.js'), 'utf8'), { document, fetch, TextEncoder, btoa });
  await new Promise(setImmediate);
  for (const key of ['vendor', 'model', 'manufacturer', 'manufacturer_url', 'model_url', 'model_number', 'revision']) {
    assert.equal(field(key).defined.checked, false);
    assert.equal(field(key).value.disabled, true);
  }
  field('vendor').defined.checked = true;
  await field('vendor').defined.fire('change');
  await elements.editor.fire('submit');
  assert.deepEqual(JSON.parse(calls.at(-1).options.body), { vendor: '' });
  assert.equal(field('vendor').defined.checked, true);
  field('vendor').defined.checked = false;
  await field('vendor').defined.fire('change');
  failSave = true;
  await elements.editor.fire('submit');
  assert.equal(field('vendor').value.className.includes('border-warning'), true);
  failSave = false;
  await elements.editor.fire('submit');
  assert.deepEqual(JSON.parse(calls.at(-1).options.body), { vendor: null });
  assert.equal(field('vendor').defined.checked, false);
  assert.equal(field('vendor').value.disabled, true);
  assert.equal(field('vendor').value.className, 'form-control');
  await elements.editor.fire('submit');
  assert.deepEqual(JSON.parse(calls.at(-1).options.body), {});
  console.log('Frontend permissions, empty values, Defined creation/deletion, status reset and login passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
