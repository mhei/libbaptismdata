'use strict';
const root = '/api/v1/baptism-data';
const editor = document.querySelector('#editor');
const login = document.querySelector('#login');
const status = document.querySelector('#status');
const rows = document.querySelector('#rows');
const save = document.querySelector('#save');
const descriptions = {
  "serial": "Traditional serial number as string, does not necessarily consist of digits only. Usually, this serial appears also on a label on the device.",
  "serial#": "Traditional serial number as string, does not necessarily consist of digits only. Unlike `serial`, this is an internal serial number, e.g. of the internal controller board, not of the whole device.",
  "board_revision": "Revision of internal (main) PCB",
  "manufacturer": "Name of vendor, may include the legal form, typically used in product property lists or as text for the following URL",
  "manufacturer_url": "URL to vendor website, ideally not a deep-link so that it is available for the whole product lifetime",
  "vendor": "Name of vendor, not including any legal form. Is typically used with `model` to form a unique product name aka `vendor` + `single whitespace` + `model`",
  "model": "A human-friedly name of the model, aka the product name; without vendor name, without any device specific data/numbers (serial numbers/MAC addresses)",
  "model_url": "URL to model website (if any), as above, ideally not a deep-link",
  "model_number": "A model number as string (if any)",
  "revision": "Product revision, very often this is not defined for the first product revision so that users of this variable should consider reasonable fallback/default values."
};
const standardKeys = ['vendor', 'model', 'manufacturer', 'manufacturer_url', 'model_url', 'model_number', 'revision'];
let authorization;
let writeAuthenticationRequired = false;
let fields = [];

function message(text, kind = 'info') {
  status.textContent = text;
  status.className = `alert alert-${kind}`;
  status.hidden = !text;
}

async function request(path, options = {}) {
  const response = await fetch(path, {
    ...options, cache: 'no-store',
    headers: { 'X-Requested-With': 'XMLHttpRequest', ...options.headers, ...(authorization ? { Authorization: authorization } : {}) }
  });
  if (response.status === 401) {
    authorization = undefined;
    login.hidden = false;
    document.querySelector('#password').focus();
    throw new Error('Please sign in with the configured password.');
  }
  if (!response.ok) {
    const error = await response.json().catch(() => null);
    throw new Error(error?.error?.message || `Request failed (HTTP ${response.status}).`);
  }
  return response.status === 204 ? null : response.json();
}

function updateDirty(field) {
  const dirty = field.defined.checked !== field.exists ||
    (field.defined.checked && field.value.value !== field.original);
  field.value.className = dirty ? 'form-control border-warning bg-warning-subtle' : 'form-control';
  field.row.className = dirty ? 'table-warning' : '';
}

async function load() {
  const [values, metadata] = await Promise.all([request(root), request('/api/v1/metadata')]);
  writeAuthenticationRequired = metadata.write_authentication_required;
  rows.replaceChildren();
  fields = [];
  const keys = new Set([...standardKeys, ...Object.keys(metadata.keys), ...Object.keys(values)]);
  for (const key of [...keys].sort()) {
    const writable = Object.hasOwn(metadata.keys, key) && metadata.keys[key] === true;
    const exists = Object.hasOwn(values, key);
    const row = document.createElement('tr');
    const name = document.createElement('th');
    name.scope = 'row';
    name.textContent = key;
    if (Object.hasOwn(descriptions, key)) {
      const description = document.createElement('small');
      description.className = 'd-block text-secondary fw-normal';
      description.textContent = descriptions[key];
      name.append(description);
    }
    const rights = document.createElement('td');
    rights.textContent = writable ? '✎' : '🔒';
    rights.title = writable ? 'Read-write' : 'Read-only';
    rights.setAttribute('aria-label', rights.title);
    const cell = document.createElement('td');
    const value = document.createElement('input');
    value.type = 'text';
    value.className = 'form-control';
    value.value = exists ? values[key] : '';
    value.readOnly = !writable;
    value.disabled = !exists;
    value.setAttribute('aria-label', `${key} value${writable ? '' : ' (read only)'}`);
    cell.append(value);
    const definedCell = document.createElement('td');
    const defined = document.createElement('input');
    defined.type = 'checkbox';
    defined.className = 'form-check-input';
    defined.disabled = !writable;
    defined.checked = exists;
    defined.setAttribute('aria-label', `Defined ${key}`);
    definedCell.append(defined);
    row.append(name, rights, definedCell, cell);
    rows.append(row);
    const field = { key, value, defined, writable, exists, original: value.value, row };
    fields.push(field);
    value.addEventListener('input', () => { updateDirty(field); message(''); });
    defined.addEventListener('change', () => {
      value.disabled = !defined.checked;
      updateDirty(field);
      message('');
    });
  }
  editor.hidden = false;
  login.hidden = true;
}

login.addEventListener('submit', async event => {
  event.preventDefault();
  const password = document.querySelector('#password');
  // Encode UTF-8 explicitly; btoa alone cannot represent non-ASCII passwords.
  authorization = `Basic ${btoa(Array.from(new TextEncoder().encode(`admin:${password.value}`), b => String.fromCharCode(b)).join(''))}`;
  password.value = '';
  await saveChanges();
});

editor.addEventListener('submit', async event => {
  event.preventDefault();
  await saveChanges();
});

async function saveChanges() {
  if (writeAuthenticationRequired && !authorization) {
    login.hidden = false;
    document.querySelector('#password').focus();
    message('Please sign in to save changes.');
    return;
  }
  const changes = Object.create(null);
  for (const field of fields) {
    if (field.writable && (field.exists || field.defined.checked))
      changes[field.key] = field.defined.checked ? field.value.value : null;
  }
  // Freeze the form until persistence completes, keeping status tied to the
  // submitted values and preventing a response from hiding newer edits.
  const controls = [...editor.querySelectorAll('input, button')];
  const disabled = controls.map(control => control.disabled);
  controls.forEach(control => { control.disabled = true; });
  message('Saving baptism data…');
  try {
    await request(root, { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(changes) });
    for (const field of fields) {
      if (Object.hasOwn(changes, field.key)) {
        field.exists = changes[field.key] !== null;
        if (!field.exists) { field.value.value = ''; field.defined.checked = false; }
        field.original = field.value.value;
        updateDirty(field);
      }
    }
    login.hidden = true;
    message('Baptism data successfully saved to device.', 'success');
  } catch (error) { message(error.message, 'danger'); }
  finally {
    controls.forEach((control, index) => { control.disabled = disabled[index]; });
    for (const field of fields) field.value.disabled = !field.defined.checked;
  }
}

load().then(() => message('')).catch(error => message(error.message, 'danger'));
