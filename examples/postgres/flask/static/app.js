const presets = JSON.parse(document.querySelector('#presets-data').textContent);
const editor = document.querySelector('#sql');
const run = document.querySelector('#run');
const notice = document.querySelector('#notice');
const meta = document.querySelector('#result-meta');
const results = document.querySelector('#results');
let selected = null;
let selection = 0;

function showNotice(text, error = false) {
  notice.textContent = text;
  notice.classList.toggle('error', error);
}

async function api(path, options = {}) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 35000);
  try {
    const response = await fetch(path, { ...options, signal: controller.signal });
    const body = await response.json();
    if (!response.ok) throw new Error(body.error || 'The request could not be completed.');
    return body;
  } finally {
    clearTimeout(timeout);
  }
}

function renderRows(data, title) {
  const table = document.createElement('table');
  const head = table.createTHead().insertRow();
  data.columns.forEach(name => {
    const item = document.createElement('th');
    item.textContent = name;
    head.append(item);
  });
  const body = table.createTBody();
  data.rows.forEach(row => {
    const item = body.insertRow();
    row.forEach(value => {
      const cell = item.insertCell();
      cell.textContent = value === null ? 'null' : String(value);
      if (value === null) cell.className = 'null';
    });
  });
  results.replaceChildren(table);
  document.querySelector('#result-title').textContent = title;
  meta.textContent = `${data.row_count} rows · ${data.elapsed_ms} ms`;
  showNotice(data.truncated
    ? `Showing the first ${data.row_count} rows. Narrow the query to explore more.`
    : data.row_count ? '' : 'The query returned no rows.');
}

function quoteIdentifier(value) {
  return '"' + value.replaceAll('"', '""') + '"';
}

async function selectTable(table, button) {
  const current = ++selection;
  selected = table;
  document.querySelectorAll('.table-button').forEach(item => item.classList.remove('active'));
  button.classList.add('active');
  meta.textContent = 'Loading table preview…';
  try {
    const data = await api('/table?' + new URLSearchParams({ schema: table.schema, name: table.name }));
    if (current !== selection) return;
    document.querySelector('#details').classList.add('open');
    document.querySelector('#detail-title').textContent = table.schema + '.' + table.name;
    const columns = document.querySelector('#columns');
    columns.replaceChildren();
    data.columns.forEach(column => {
      const item = document.createElement('div');
      item.className = 'column';
      item.textContent = column.name;
      const type = document.createElement('span');
      type.textContent = column.type;
      item.append(type);
      if (column.nullable) {
        const nullable = document.createElement('em');
        nullable.textContent = 'null';
        item.append(nullable);
      }
      columns.append(item);
    });
    renderRows(data.preview, 'Table preview');
  } catch (error) {
    if (current === selection) {
      showNotice(error.name === 'AbortError' ? 'The preview reached its time limit.' : error.message, true);
      meta.textContent = 'Preview unavailable';
    }
  }
}

document.querySelector('#use-table').addEventListener('click', () => {
  if (!selected) return;
  editor.value = 'SELECT *\nFROM ' + quoteIdentifier(selected.schema) + '.' + quoteIdentifier(selected.name) + '\nLIMIT 100';
  editor.focus();
});

document.querySelectorAll('[data-preset]').forEach(button => {
  button.addEventListener('click', () => {
    editor.value = presets[Number(button.dataset.preset)].sql;
    editor.focus();
  });
});

document.querySelector('#query-form').addEventListener('submit', async event => {
  event.preventDefault();
  if (run.disabled) return;
  run.disabled = true;
  run.textContent = 'Running…';
  showNotice('');
  meta.textContent = 'Query in progress';
  try {
    const data = await api('/query', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sql: editor.value, limit: 100 }),
    });
    renderRows(data, 'Query results');
  } catch (error) {
    showNotice(error.name === 'AbortError'
      ? 'The request reached its time limit. Try a smaller query.' : error.message, true);
    meta.textContent = 'Query not completed';
  } finally {
    run.disabled = false;
    run.textContent = 'Run query →';
  }
});

editor.addEventListener('keydown', event => {
  if (event.key === 'Enter' && (event.metaKey || event.ctrlKey)) {
    event.preventDefault();
    document.querySelector('#query-form').requestSubmit();
  }
});

(async () => {
  try {
    const data = await api('/tables');
    const container = document.querySelector('#tables');
    container.replaceChildren();
    document.querySelector('#table-count').textContent = data.tables.length;
    data.tables.forEach(table => {
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'table-button';
      const icon = document.createElement('span');
      icon.className = 'table-icon';
      icon.textContent = '▦';
      icon.setAttribute('aria-hidden', 'true');
      const label = document.createElement('span');
      const name = document.createElement('strong');
      const schema = document.createElement('small');
      name.textContent = table.name;
      schema.textContent = table.schema + ' · ' + table.type;
      label.append(name, schema);
      button.append(icon, label);
      button.addEventListener('click', () => selectTable(table, button));
      container.append(button);
    });
    const preferred = data.tables.findIndex(table => table.name === 'products');
    const index = preferred < 0 ? 0 : preferred;
    if (data.tables[index]) await selectTable(data.tables[index], container.children[index]);
    else container.textContent = 'No tables are available.';
  } catch (error) {
    document.querySelector('#tables').textContent = 'Catalog unavailable';
    document.querySelector('#table-count').textContent = '—';
    showNotice(error.message, true);
  }
})();
