const form = document.querySelector('#query-form');
const editor = document.querySelector('#sql');
const run = document.querySelector('#run');
const results = document.querySelector('#results');
const meta = document.querySelector('#result-meta');
const notice = document.querySelector('#notice');

document.querySelectorAll('.preset').forEach(button => {
  button.addEventListener('click', () => {
    document.querySelectorAll('.preset').forEach(item => item.classList.remove('active'));
    button.classList.add('active');
    editor.value = button.dataset.query;
    editor.focus();
  });
});
editor.addEventListener('keydown', event => {
  if (event.key === 'Enter' && (event.metaKey || event.ctrlKey)) {
    event.preventDefault();
    form.requestSubmit();
  }
});
form.addEventListener('submit', async event => {
  event.preventDefault();
  if (run.disabled) return;
  run.disabled = true;
  run.textContent = 'Running…';
  notice.textContent = '';
  meta.textContent = 'Query in progress';
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 35000);
  try {
    const response = await fetch('/query', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sql: editor.value, limit: 100 }), signal: controller.signal,
    });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error ?? 'The query could not be completed.');
    const table = document.createElement('table');
    const head = table.createTHead().insertRow();
    data.columns.forEach(name => { const cell = document.createElement('th'); cell.textContent = name; head.append(cell); });
    const body = table.createTBody();
    data.rows.forEach(row => {
      const cells = body.insertRow();
      row.forEach(value => {
        const cell = cells.insertCell();
        cell.textContent = value === null ? 'null' : String(value);
        if (value === null) cell.className = 'null';
      });
    });
    results.replaceChildren(table);
    meta.textContent = `${data.row_count} rows · ${data.elapsed_ms} ms`;
    if (data.truncated) notice.textContent = 'Showing the first 100 rows. Narrow the query to explore more.';
    if (!data.rows.length) notice.textContent = 'The query returned no rows.';
  } catch (error) {
    notice.textContent = error.name === 'AbortError' ? 'The request reached its time limit. Try a smaller query.' : error.message;
    meta.textContent = 'Query not completed';
  } finally {
    clearTimeout(timeout);
    run.disabled = false;
    run.textContent = 'Run query →';
  }
});
