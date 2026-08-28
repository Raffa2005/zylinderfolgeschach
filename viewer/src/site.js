const root = document.documentElement;
const buttons = document.querySelectorAll('.theme-button');

function renderTheme() {
  const dark = root.classList.contains('dark');
  for (const button of buttons) {
    button.textContent = dark ? '☀' : '◐';
    button.setAttribute('aria-label', dark ? 'Use light theme' : 'Use dark theme');
  }
  document.querySelector('meta[name="theme-color"]')?.setAttribute(
    'content',
    dark ? '#181b1a' : '#f4f5f2',
  );
}

for (const button of buttons) {
  button.addEventListener('click', () => {
    const dark = root.classList.toggle('dark');
    try {
      localStorage.setItem('zfs-theme', dark ? 'dark' : 'light');
    } catch {
      // Theme persistence is optional; the toggle itself still works.
    }
    renderTheme();
  });
}

renderTheme();
